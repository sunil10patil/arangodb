////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include <Agency/AsyncAgencyComm.h>
#include "gtest/gtest.h"

#include "analysis/analyzers.hpp"
#include "analysis/token_attributes.hpp"
#include "index/norm.hpp"
#include <filesystem>

#include "IResearch/IResearchTestCommon.h"
#include "IResearch/RestHandlerMock.h"
#include "IResearch/common.h"
#include "Mocks/LogLevels.h"
#include "Mocks/Servers.h"
#include "Mocks/StorageEngineMock.h"

#include "Agency/Store.h"
#include "ApplicationFeatures/CommunicationFeaturePhase.h"
#include "ApplicationFeatures/GreetingsFeaturePhase.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/AstNode.h"
#include "Aql/Function.h"
#include "Aql/OptimizerRulesFeature.h"
#include "Aql/QueryRegistry.h"
#include "Basics/files.h"
#include "Cluster/AgencyCache.h"
#include "Cluster/ClusterFeature.h"
#include "FeaturePhases/BasicFeaturePhaseServer.h"
#include "FeaturePhases/ClusterFeaturePhase.h"
#include "FeaturePhases/DatabaseFeaturePhase.h"
#include "FeaturePhases/V8FeaturePhase.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/AgencyMock.h"
#include "IResearch/ExpressionContextMock.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchVPackTermAttribute.h"
#include "IResearch/VelocyPackHelper.h"
#include "Indexes/IndexFactory.h"
#include "Network/NetworkFeature.h"
#include "Random/RandomFeature.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/FlushFeature.h"
#include "Metrics/MetricsFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "RestServer/UpgradeFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "Scheduler/SchedulerFeature.h"
#include "Sharding/ShardingFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/ExecContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "V8Server/V8DealerFeature.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/Indexes.h"
#include "velocypack/Slice.h"

#if USE_ENTERPRISE
#include "Enterprise/Ldap/LdapFeature.h"
#endif

namespace {

struct TestIndex : public arangodb::Index {
  TestIndex(arangodb::IndexId id, arangodb::LogicalCollection& collection,
            arangodb::velocypack::Slice const& definition)
      : arangodb::Index(id, collection, definition) {}
  bool canBeDropped() const override { return false; }
  bool hasSelectivityEstimate() const override { return false; }
  bool isHidden() const override { return false; }
  bool isSorted() const override { return false; }
  std::unique_ptr<arangodb::IndexIterator> iteratorForCondition(
      arangodb::ResourceMonitor& /* monitor */,
      arangodb::transaction::Methods* /* trx */,
      arangodb::aql::AstNode const* /* node */,
      arangodb::aql::Variable const* /* reference */,
      arangodb::IndexIteratorOptions const& /* opts */, arangodb::ReadOwnWrites,
      int) override {
    return nullptr;
  }
  void load() override {}
  size_t memory() const override { return sizeof(Index); }
  arangodb::Index::IndexType type() const override {
    return arangodb::Index::TRI_IDX_TYPE_UNKNOWN;
  }
  char const* typeName() const override { return "testType"; }
  void unload() override {}
};

class ReNormalizingAnalyzer final
    : public irs::analysis::TypedAnalyzer<ReNormalizingAnalyzer> {
 public:
  static constexpr std::string_view type_name() noexcept {
    return "ReNormalizingAnalyzer";
  }

  ReNormalizingAnalyzer() = default;

  irs::attribute* get_mutable(irs::type_info::type_id type) noexcept final {
    if (type == irs::type<TestAttribute>::id()) {
      return &_attr;
    }
    return nullptr;
  }

  static ptr make(std::string_view args) {
    auto slice = arangodb::iresearch::slice(args);
    if (slice.isNull()) {
      throw std::exception{};
    }
    if (slice.isNone()) {
      return nullptr;
    }
    return std::make_unique<ReNormalizingAnalyzer>();
  }

  // test implementation
  // string will be normalized as is. But object will be converted!
  // need this to test comparison "old-normalized"  against "new-normalized"
  static bool normalize(std::string_view args, std::string& definition) {
    auto slice = arangodb::iresearch::slice(args);
    arangodb::velocypack::Builder builder;
    if (slice.isString()) {
      VPackObjectBuilder scope(&builder);
      arangodb::iresearch::addStringRef(
          builder, "args", arangodb::iresearch::getStringRef(slice));
    } else if (slice.isObject() && slice.hasKey("args") &&
               slice.get("args").isString()) {
      VPackObjectBuilder scope(&builder);
      auto inputDef = arangodb::iresearch::getStringRef(slice.get("args"));
      arangodb::iresearch::addStringRef(builder, "args",
                                        inputDef == "123" ? "321" : inputDef);
    } else {
      return false;
    }

    definition = builder.buffer()->toString();

    return true;
  }

  bool next() final { return false; }

  bool reset(std::string_view) final { return false; }

 private:
  TestAttribute _attr;
};

REGISTER_ANALYZER_VPACK(ReNormalizingAnalyzer, ReNormalizingAnalyzer::make,
                        ReNormalizingAnalyzer::normalize);

class TestTokensTypedAnalyzer final
    : public irs::analysis::TypedAnalyzer<TestTokensTypedAnalyzer> {
 public:
  static constexpr std::string_view type_name() noexcept {
    return "iresearch-tokens-typed";
  }

  static ptr make(std::string_view args) {
    return std::make_unique<TestTokensTypedAnalyzer>(args);
  }

  static bool normalize(std::string_view args, std::string& out) {
    out.assign(args.data(), args.size());
    return true;
  }

  explicit TestTokensTypedAnalyzer(std::string_view args) {
    VPackSlice slice(irs::ViewCast<irs::byte_type>(args).data());
    if (slice.hasKey("type")) {
      auto type = slice.get("type").stringView();
      if (type == "number") {
        _returnType.value = arangodb::iresearch::AnalyzerValueType::Number;
        _typedValue =
            arangodb::aql::AqlValue(arangodb::aql::AqlValueHintDouble(1));
        _vpackTerm.value = _typedValue.slice();
      } else if (type == "bool") {
        _returnType.value = arangodb::iresearch::AnalyzerValueType::Bool;
      } else if (type == "string") {
        _returnType.value = arangodb::iresearch::AnalyzerValueType::String;
        _term.value = irs::ViewCast<irs::byte_type>(std::string_view{_strVal});
      } else {
        // Failure here means we have unexpected type
        EXPECT_TRUE(false);
      }
    }
  }

  bool reset(std::string_view data) final {
    if (!irs::IsNull(data)) {
      _strVal = data;
    } else {
      _strVal.clear();
    }
    return true;
  }

  bool next() final {
    if (!_strVal.empty()) {
      switch (_returnType.value) {
        case arangodb::iresearch::AnalyzerValueType::Bool:
          _typedValue = arangodb::aql::AqlValue(
              arangodb::aql::AqlValueHintBool(_strVal.size() % 2 == 0));
          _vpackTerm.value = _typedValue.slice();
          break;
        case arangodb::iresearch::AnalyzerValueType::Number:
          _typedValue =
              arangodb::aql::AqlValue(arangodb::aql::AqlValueHintDouble(
                  static_cast<double>(_strVal.size() % 2)));
          _vpackTerm.value = _typedValue.slice();
          break;
        case arangodb::iresearch::AnalyzerValueType::String:
          _term.value =
              irs::ViewCast<irs::byte_type>(std::string_view{_strVal});
          break;
        default:
          // New return type was added?
          EXPECT_TRUE(false);
          break;
      }
      _strVal.pop_back();
      return true;
    } else {
      return false;
    }
  }

  irs::attribute* get_mutable(irs::type_info::type_id type) noexcept final {
    if (type == irs::type<irs::term_attribute>::id()) {
      return &_term;
    }
    if (type == irs::type<irs::increment>::id()) {
      return &_inc;
    }
    if (type ==
        irs::type<arangodb::iresearch::AnalyzerValueTypeAttribute>::id()) {
      return &_returnType;
    }
    if (type == irs::type<arangodb::iresearch::VPackTermAttribute>::id()) {
      return &_vpackTerm;
    }
    return nullptr;
  }

 private:
  std::string _strVal;
  irs::term_attribute _term;
  arangodb::iresearch::VPackTermAttribute _vpackTerm;
  irs::increment _inc;
  arangodb::iresearch::AnalyzerValueTypeAttribute _returnType;
  arangodb::aql::AqlValue _typedValue;
};

REGISTER_ANALYZER_VPACK(TestTokensTypedAnalyzer, TestTokensTypedAnalyzer::make,
                        TestTokensTypedAnalyzer::normalize);

struct Analyzer {
  std::string_view type;
  VPackSlice properties;
  arangodb::iresearch::Features features;

  Analyzer() = default;
  Analyzer(std::string_view const t, std::string_view const p,
           arangodb::iresearch::Features f = {})
      : type(t), features(f) {
    if (irs::IsNull(p)) {
      properties = VPackSlice::nullSlice();
    } else {
      propBuilder = VPackParser::fromJson(p.data(), p.size());
      properties = propBuilder->slice();
    }
  }

 private:
  // internal VPack storage. Not to be used outside
  std::shared_ptr<VPackBuilder> propBuilder;
};

std::map<std::string_view, Analyzer> const& staticAnalyzers() {
  static const std::map<std::string_view, Analyzer> analyzers = {
      {"identity",
       {"identity",
        std::string_view{},
        {arangodb::iresearch::FieldFeatures::NORM, irs::IndexFeatures::FREQ}}},
      {"text_de",
       {"text",
        "{ \"locale\": \"de.UTF-8\", \"stopwords\": [ ] "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
      {"text_en",
       {"text",
        "{ \"locale\": \"en.UTF-8\", \"stopwords\": [ ] "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
      {"text_es",
       {"text",
        "{ \"locale\": \"es.UTF-8\", \"stopwords\": [ ] "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
      {"text_fi",
       {"text",
        "{ \"locale\": \"fi.UTF-8\", \"stopwords\": [ ] "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
      {"text_fr",
       {"text",
        "{ \"locale\": \"fr.UTF-8\", \"stopwords\": [ ] "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
      {"text_it",
       {"text",
        "{ \"locale\": \"it.UTF-8\", \"stopwords\": [ ] "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
      {"text_nl",
       {"text",
        "{ \"locale\": \"nl.UTF-8\", \"stopwords\": [ ] "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
      {"text_no",
       {"text",
        "{ \"locale\": \"no.UTF-8\", \"stopwords\": [ ] "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
      {"text_pt",
       {"text",
        "{ \"locale\": \"pt.UTF-8\", \"stopwords\": [ ] "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
      {"text_ru",
       {"text",
        "{ \"locale\": \"ru.UTF-8\", \"stopwords\": [ ] "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
      {"text_sv",
       {"text",
        "{ \"locale\": \"sv.UTF-8\", \"stopwords\": [ ] "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
      {"text_zh",
       {"text",
        "{ \"locale\": \"zh.UTF-8\", \"stopwords\": [ ], \"stemming\": false "
        "}",
        {arangodb::iresearch::FieldFeatures::NORM,
         irs::IndexFeatures::FREQ | irs::IndexFeatures::POS}}},
  };

  return analyzers;
}

// AqlValue entries must be explicitly deallocated
struct VPackFunctionParametersWrapper {
  arangodb::aql::VPackFunctionParameters instance;
  VPackFunctionParametersWrapper() = default;
  ~VPackFunctionParametersWrapper() {
    for (auto& entry : instance) {
      entry.destroy();
    }
  }
  arangodb::aql::VPackFunctionParameters* operator->() { return &instance; }
  arangodb::aql::VPackFunctionParameters& operator*() { return instance; }
};

// AqlValue entrys must be explicitly deallocated
struct AqlValueWrapper {
  arangodb::aql::AqlValue instance;
  AqlValueWrapper(arangodb::aql::AqlValue&& other)
      : instance(std::move(other)) {}
  ~AqlValueWrapper() { instance.destroy(); }
  arangodb::aql::AqlValue* operator->() { return &instance; }
  arangodb::aql::AqlValue& operator*() { return instance; }
};

static const VPackBuilder systemDatabaseBuilder = dbArgsBuilder();
static const VPackSlice systemDatabaseArgs = systemDatabaseBuilder.slice();
}  // namespace

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

class IResearchAnalyzerFeatureTest
    : public ::testing::Test,
      public arangodb::tests::LogSuppressor<arangodb::Logger::AUTHENTICATION,
                                            arangodb::LogLevel::ERR>,
      public arangodb::tests::LogSuppressor<arangodb::Logger::CLUSTER,
                                            arangodb::LogLevel::FATAL> {
 protected:
  arangodb::tests::mocks::MockV8Server server;
  arangodb::SystemDatabaseFeature* sysDatabaseFeature{};

  IResearchAnalyzerFeatureTest() : server(false) {
    arangodb::tests::init();

    server.addFeature<arangodb::QueryRegistryFeature>(false);
    server.addFeature<arangodb::AqlFeature>(true);
    server.addFeature<arangodb::aql::OptimizerRulesFeature>(true);

    server.startFeatures();

    auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();

    auto vocbase =
        dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
    std::shared_ptr<arangodb::LogicalCollection> unused;
    arangodb::OperationOptions options(arangodb::ExecContext::current());
    arangodb::methods::Collections::createSystem(
        *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
        unused);
  }

  ~IResearchAnalyzerFeatureTest() {
    // Clear the authentication user:
    auto& authFeature = server.getFeature<arangodb::AuthenticationFeature>();
    auto* userManager = authFeature.userManager();
    if (userManager != nullptr) {
      userManager->removeAllUsers();
    }
  }

  void userSetAccessLevel(arangodb::auth::Level db, arangodb::auth::Level col) {
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    ASSERT_NE(authFeature, nullptr);
    auto* userManager = authFeature->userManager();
    ASSERT_NE(userManager, nullptr);
    arangodb::auth::UserMap userMap;
    auto user = arangodb::auth::User::newUser("testUser", "testPW",
                                              arangodb::auth::Source::LDAP);
    user.grantDatabase("testVocbase", db);
    user.grantCollection("testVocbase", "*", col);
    userMap.emplace("testUser", std::move(user));
    userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                        // configuration from system database
  }

  std::unique_ptr<arangodb::ExecContext> getLoggedInContext() const {
    return arangodb::ExecContext::create("testUser", "testVocbase");
  }

  std::string analyzerName() const {
    return arangodb::StaticStrings::SystemDatabase + "::test_analyzer";
  }
};

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                         authentication test suite
// -----------------------------------------------------------------------------

TEST_F(IResearchAnalyzerFeatureTest, test_auth_no_auth) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  EXPECT_TRUE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RW));
}
TEST_F(IResearchAnalyzerFeatureTest, test_auth_no_vocbase_read) {
  // no vocbase read access
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  userSetAccessLevel(arangodb::auth::Level::NONE, arangodb::auth::Level::NONE);
  auto ctxt = getLoggedInContext();
  arangodb::ExecContextScope execContextScope(ctxt.get());
  EXPECT_FALSE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RO));
}

// no collection read access (vocbase read access, no user)
TEST_F(IResearchAnalyzerFeatureTest,
       test_auth_vocbase_none_collection_read_no_user) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  userSetAccessLevel(arangodb::auth::Level::NONE, arangodb::auth::Level::RO);
  auto ctxt = getLoggedInContext();
  arangodb::ExecContextScope execContextScope(ctxt.get());
  EXPECT_FALSE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RO));
}

// no collection read access (vocbase read access)
TEST_F(IResearchAnalyzerFeatureTest, test_auth_vocbase_ro_collection_none) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  userSetAccessLevel(arangodb::auth::Level::RO, arangodb::auth::Level::NONE);
  auto ctxt = getLoggedInContext();
  arangodb::ExecContextScope execContextScope(ctxt.get());
  // implicit RO access to collection _analyzers collection granted due to RO
  // access to db
  EXPECT_TRUE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RO));

  EXPECT_FALSE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RW));
}

TEST_F(IResearchAnalyzerFeatureTest, test_auth_vocbase_ro_collection_ro) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  userSetAccessLevel(arangodb::auth::Level::RO, arangodb::auth::Level::RO);
  auto ctxt = getLoggedInContext();
  arangodb::ExecContextScope execContextScope(ctxt.get());
  EXPECT_TRUE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RO));
  EXPECT_FALSE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RW));
}

TEST_F(IResearchAnalyzerFeatureTest, test_auth_vocbase_ro_collection_rw) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  userSetAccessLevel(arangodb::auth::Level::RO, arangodb::auth::Level::RW);
  auto ctxt = getLoggedInContext();
  arangodb::ExecContextScope execContextScope(ctxt.get());
  EXPECT_TRUE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RO));
  EXPECT_FALSE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RW));
}

TEST_F(IResearchAnalyzerFeatureTest, test_auth_vocbase_rw_collection_ro) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  userSetAccessLevel(arangodb::auth::Level::RW, arangodb::auth::Level::RO);
  auto ctxt = getLoggedInContext();
  arangodb::ExecContextScope execContextScope(ctxt.get());
  EXPECT_TRUE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RO));
  // implicit access for system analyzers collection granted due to RW access to
  // database
  EXPECT_TRUE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RW));
}

TEST_F(IResearchAnalyzerFeatureTest, test_auth_vocbase_rw_collection_rw) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  userSetAccessLevel(arangodb::auth::Level::RW, arangodb::auth::Level::RW);
  auto ctxt = getLoggedInContext();
  arangodb::ExecContextScope execContextScope(ctxt.get());
  EXPECT_TRUE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RO));
  EXPECT_TRUE(arangodb::iresearch::IResearchAnalyzerFeature::canUse(
      vocbase, arangodb::auth::Level::RW));
}

// -----------------------------------------------------------------------------
// --SECTION--                                                emplace test suite
// -----------------------------------------------------------------------------

TEST_F(IResearchAnalyzerFeatureTest, test_emplace_valid) {
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_TRUE(feature
                    .emplace(result, analyzerName(), "TestAnalyzer",
                             VPackParser::fromJson("\"abcd\"")->slice())
                    .ok());
    EXPECT_NE(result.first, nullptr);
  }
  auto pool = feature.get(analyzerName(),
                          arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
}

TEST_F(IResearchAnalyzerFeatureTest, test_emplace_duplicate_valid) {
  // add duplicate valid (same name+type+properties)
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto res = feature.emplace(
        result, analyzerName(), "TestAnalyzer",
        VPackParser::fromJson("\"abcd\"")->slice(),
        arangodb::iresearch::Features(irs::IndexFeatures::FREQ));
    EXPECT_TRUE(res.ok());
    EXPECT_NE(result.first, nullptr);
  }
  auto pool = feature.get(analyzerName(),
                          arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features(irs::IndexFeatures::FREQ),
            pool->features());
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_TRUE(
        feature
            .emplace(result, analyzerName(), "TestAnalyzer",
                     VPackParser::fromJson("\"abcd\"")->slice(),
                     arangodb::iresearch::Features(irs::IndexFeatures::FREQ))
            .ok());
    EXPECT_NE(result.first, nullptr);
  }
  auto poolOther = feature.get(analyzerName(),
                               arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(poolOther, nullptr);
  EXPECT_EQ(pool, poolOther);
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_emplace_duplicate_invalid_properties) {
  // add duplicate invalid (same name+type different properties)
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_TRUE(feature
                    .emplace(result, analyzerName(), "TestAnalyzer",
                             VPackParser::fromJson("\"abc\"")->slice())
                    .ok());
    EXPECT_NE(result.first, nullptr);
  }
  auto pool = feature.get(analyzerName(),
                          arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
  // Emplace should fail
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_FALSE(feature
                     .emplace(result, analyzerName(), "TestAnalyzer",
                              VPackParser::fromJson("\"abcd\"")->slice())
                     .ok());
    EXPECT_EQ(result.first, nullptr);
  }
  // The formerly stored feature should still be available
  auto poolOther = feature.get(analyzerName(),
                               arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(poolOther, nullptr);
  EXPECT_EQ(pool, poolOther);
}

TEST_F(IResearchAnalyzerFeatureTest, test_emplace_duplicate_invalid_features) {
  // add duplicate invalid (same name+type different properties)
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_TRUE(feature
                    .emplace(result, analyzerName(), "TestAnalyzer",
                             VPackParser::fromJson("\"abc\"")->slice())
                    .ok());
    EXPECT_NE(result.first, nullptr);
  }
  auto pool = feature.get(analyzerName(),
                          arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
  {
    // Emplace should fail
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_FALSE(
        feature
            .emplace(result, analyzerName(), "TestAnalyzer",
                     VPackParser::fromJson("\"abc\"")->slice(),
                     arangodb::iresearch::Features(irs::IndexFeatures::FREQ))
            .ok());
    EXPECT_EQ(result.first, nullptr);
  }
  // The formerly stored feature should still be available
  auto poolOther = feature.get(analyzerName(),
                               arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(poolOther, nullptr);
  EXPECT_EQ(pool, poolOther);
}

TEST_F(IResearchAnalyzerFeatureTest, test_emplace_duplicate_invalid_type) {
  // add duplicate invalid (same name+type different properties)
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_TRUE(feature
                    .emplace(result, analyzerName(), "TestAnalyzer",
                             VPackParser::fromJson("\"abc\"")->slice())
                    .ok());
    EXPECT_NE(result.first, nullptr);
  }
  auto pool = feature.get(analyzerName(),
                          arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
  {
    // Emplace should fail
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_FALSE(
        feature
            .emplace(result, analyzerName(), "invalid",
                     VPackParser::fromJson("\"abc\"")->slice(),
                     arangodb::iresearch::Features(irs::IndexFeatures::FREQ))
            .ok());
    EXPECT_EQ(result.first, nullptr);
  }
  // The formerly stored feature should still be available
  auto poolOther = feature.get(analyzerName(),
                               arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(poolOther, nullptr);
  EXPECT_EQ(pool, poolOther);
}

TEST_F(IResearchAnalyzerFeatureTest, test_emplace_creation_failure_properties) {
  // add invalid (instance creation failure)
  arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  auto res = feature.emplace(result, analyzerName(), "TestAnalyzer",
                             VPackSlice::noneSlice());
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, res.errorNumber());
  EXPECT_EQ(feature.get(analyzerName(),
                        arangodb::QueryAnalyzerRevisions::QUERY_LATEST),
            nullptr);
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_emplace_creation_failure__properties_nil) {
  // add invalid (instance creation exception)
  arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  auto res = feature.emplace(result, analyzerName(), "TestAnalyzer",
                             VPackSlice::nullSlice());
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, res.errorNumber());
  EXPECT_EQ(feature.get(analyzerName(),
                        arangodb::QueryAnalyzerRevisions::QUERY_LATEST),
            nullptr);
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_emplace_creation_failure_invalid_type) {
  // add invalid (not registred)
  arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  auto res = feature.emplace(result, analyzerName(), "invalid",
                             VPackParser::fromJson("\"abc\"")->slice());
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_NOT_IMPLEMENTED, res.errorNumber());
  EXPECT_EQ(feature.get(analyzerName(),
                        arangodb::QueryAnalyzerRevisions::QUERY_LATEST),
            nullptr);
}

TEST_F(IResearchAnalyzerFeatureTest, test_emplace_creation_during_recovery) {
  // add valid inRecovery (failure)
  arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  auto before = StorageEngineMock::recoveryStateResult;
  StorageEngineMock::recoveryStateResult = arangodb::RecoveryState::IN_PROGRESS;
  irs::Finally restore = [&before]() noexcept {
    StorageEngineMock::recoveryStateResult = before;
  };
  auto res = feature.emplace(result, analyzerName(), "TestAnalyzer",
                             VPackParser::fromJson("\"abc\"")->slice());
  // emplace should return OK for the sake of recovery
  EXPECT_TRUE(res.ok());
  auto ptr = feature.get(analyzerName(),
                         arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  // but nothing should be stored
  EXPECT_EQ(nullptr, ptr);
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_emplace_creation_position_without_frequency) {
  // add invalid ('position' without 'frequency')
  arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  auto res = feature.emplace(
      result, analyzerName(), "TestAnalyzer",
      VPackParser::fromJson("\"abc\"")->slice(),
      arangodb::iresearch::Features({}, irs::IndexFeatures::POS));
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, res.errorNumber());
  EXPECT_EQ(feature.get(analyzerName(),
                        arangodb::QueryAnalyzerRevisions::QUERY_LATEST),
            nullptr);
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_emplace_creation_properties_too_large) {
  arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  std::string properties(1024 * 1024 + 1, 'x');  // +1 char longer then limit
  VPackBuilder prop;
  prop.openObject();
  prop.add("value", VPackValue(properties));
  prop.close();
  auto res = feature.emplace(
      result, analyzerName(), "TestAnalyzer", prop.slice(),
      arangodb::iresearch::Features({}, irs::IndexFeatures::FREQ));
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, res.errorNumber());
  EXPECT_EQ(feature.get(analyzerName(),
                        arangodb::QueryAnalyzerRevisions::QUERY_LATEST),
            nullptr);
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_emplace_creation_name_invalid_character) {
  arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  std::string invalidName = analyzerName() + "+";  // '+' is invalid
  auto res = feature.emplace(result, invalidName, "TestAnalyzer",
                             VPackParser::fromJson("\"abc\"")->slice());
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, res.errorNumber());
  EXPECT_EQ(
      feature.get(invalidName, arangodb::QueryAnalyzerRevisions::QUERY_LATEST),
      nullptr);
}

TEST_F(IResearchAnalyzerFeatureTest, test_emplace_add_static_analyzer) {
  arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  feature.prepare();  // add static analyzers
  auto res = feature.emplace(
      result, "identity", "identity", VPackSlice::noneSlice(),
      arangodb::iresearch::Features(arangodb::iresearch::FieldFeatures::NORM,
                                    irs::IndexFeatures::FREQ));
  EXPECT_TRUE(res.ok());
  EXPECT_NE(result.first, nullptr);
  auto pool =
      feature.get("identity", arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(
      arangodb::iresearch::Features(arangodb::iresearch::FieldFeatures::NORM,
                                    irs::IndexFeatures::FREQ),
      pool->features());
  auto analyzer = pool->get();
  ASSERT_NE(analyzer.get(), nullptr);
  feature.unprepare();
}

TEST_F(IResearchAnalyzerFeatureTest, test_renormalize_for_equal) {
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_TRUE(
        feature
            .emplace(
                result, analyzerName(), "ReNormalizingAnalyzer",
                VPackParser::fromJson("\"123\"")
                    ->slice())  // 123 will be stored as is (old-normalized)
            .ok());
    EXPECT_NE(result.first, nullptr);
  }
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_TRUE(feature
                    .emplace(result, analyzerName(), "ReNormalizingAnalyzer",
                             VPackParser::fromJson("{ \"args\":\"123\"}")
                                 ->slice())  // 123 will be normalized to 321
                    .ok());
    EXPECT_NE(result.first, nullptr);
  }
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_FALSE(
        feature
            .emplace(result, analyzerName(), "ReNormalizingAnalyzer",
                     VPackParser::fromJson("{ \"args\":\"1231\"}")
                         ->slice())  // Re-normalization should not help
            .ok());
    EXPECT_EQ(result.first, nullptr);
  }
}

TEST_F(IResearchAnalyzerFeatureTest, test_bulk_emplace_valid) {
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
  auto vocbase = dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
  EXPECT_TRUE(
      feature
          .bulkEmplace(*vocbase,
                       VPackParser::fromJson(
                           "[{\"name\":\"b_abcd\", \"type\":\"identity\"}]")
                           ->slice())
          .ok());
  auto pool = feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd",
                          arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
  EXPECT_EQ("identity", pool->type());
}

TEST_F(IResearchAnalyzerFeatureTest, test_bulk_emplace_multiple_valid) {
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
  auto vocbase = dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
  EXPECT_TRUE(
      feature
          .bulkEmplace(*vocbase, VPackParser::fromJson(
                                     R"([{"name":"b_abcd", "type":"identity"},
          {"name":"b_abcd2", "type":"TestAnalyzer",
                             "properties":{"args":"abc"},
                             "features":["frequency", "position", "norm"]}])")
                                     ->slice())
          .ok());
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
    EXPECT_EQ("identity", pool->type());
  }
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd2",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(arangodb::iresearch::Features(
                  arangodb::iresearch::FieldFeatures::NORM,
                  irs::IndexFeatures::FREQ | irs::IndexFeatures::POS),
              pool->features());
    EXPECT_EQ("TestAnalyzer", pool->type());
    EXPECT_EQUAL_SLICES(VPackParser::fromJson("{\"args\":\"abc\"}")->slice(),
                        pool->properties());
  }
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_bulk_emplace_multiple_skip_invalid_features) {
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
  auto vocbase = dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
  EXPECT_TRUE(
      feature
          .bulkEmplace(
              *vocbase,
              VPackParser::fromJson(
                  "[{\"name\":\"b_abcd\", \"type\":\"identity\"},"
                  "{\"name\":\"b_abcd2\", \"type\":\"TestAnalyzer\","
                  "\"properties\":{\"args\":\"abc\"},"
                  "\"features\":[\"frequency\", \"posAAAAition\", \"norm\"]},"
                  "{\"name\":\"b_abcd3\", \"type\":\"identity\"}"
                  "]")
                  ->slice())
          .ok());
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
    EXPECT_EQ("identity", pool->type());
  }
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd2",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_EQ(pool, nullptr);
  }
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd3",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
    EXPECT_EQ("identity", pool->type());
  }
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_bulk_emplace_multiple_skip_invalid_name) {
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
  auto vocbase = dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
  EXPECT_TRUE(
      feature
          .bulkEmplace(*vocbase,
                       VPackParser::fromJson(
                           "[{\"name\":\"b_abcd\", \"type\":\"identity\"},"
                           "{\"no_name\":\"b_abcd2\", \"type\":\"identity\"},"
                           "{\"name\":\"b_abcd3\", \"type\":\"identity\"}"
                           "]")
                           ->slice())
          .ok());
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
    EXPECT_EQ("identity", pool->type());
  }
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd2",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_EQ(pool, nullptr);
  }
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd3",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
    EXPECT_EQ("identity", pool->type());
  }
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_bulk_emplace_multiple_skip_invalid_type) {
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
  auto vocbase = dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
  EXPECT_TRUE(
      feature
          .bulkEmplace(*vocbase,
                       VPackParser::fromJson(
                           "[{\"name\":\"b_abcd\", \"type\":\"identity\"},"
                           "{\"name\":\"b_abcd2\", \"no_type\":\"identity\"},"
                           "{\"name\":\"b_abcd3\", \"type\":\"identity\"}"
                           "]")
                           ->slice())
          .ok());
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
    EXPECT_EQ("identity", pool->type());
  }
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd2",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_EQ(pool, nullptr);
  }
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd3",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
    EXPECT_EQ("identity", pool->type());
  }
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_bulk_emplace_multiple_skip_invalid_properties) {
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
  auto vocbase = dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
  EXPECT_TRUE(
      feature
          .bulkEmplace(
              *vocbase,
              VPackParser::fromJson(
                  "[{\"name\":\"b_abcd\", \"type\":\"identity\"},"
                  "{\"name\":\"b_abcd2\", \"type\":\"TestAnalyzer\","
                  "\"properties\":{\"invalid_args\":\"abc\"},"
                  "\"features\":[\"frequency\", \"position\", \"norm\"]},"
                  "{\"name\":\"b_abcd3\", \"type\":\"identity\"}"
                  "]")
                  ->slice())
          .ok());
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
    EXPECT_EQ("identity", pool->type());
  }
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd2",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_EQ(pool, nullptr);
  }
  {
    auto pool =
        feature.get(arangodb::StaticStrings::SystemDatabase + "::b_abcd3",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
    EXPECT_EQ("identity", pool->type());
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    get test suite
// -----------------------------------------------------------------------------

class IResearchAnalyzerFeatureGetTest : public IResearchAnalyzerFeatureTest {
 protected:
  arangodb::iresearch::IResearchAnalyzerFeature& analyzerFeature;
  std::string dbName;

 private:
  arangodb::SystemDatabaseFeature::ptr _sysVocbase;
  TRI_vocbase_t* _vocbase;

 protected:
  IResearchAnalyzerFeatureGetTest()
      : IResearchAnalyzerFeatureTest(),
        analyzerFeature(server.addFeatureUntracked<
                        arangodb::iresearch::IResearchAnalyzerFeature>()),
        dbName("testVocbase") {}

  ~IResearchAnalyzerFeatureGetTest() = default;

  // Need Setup inorder to alow ASSERTs
  void SetUp() override {
    // Prepare a database
    _sysVocbase = server.getFeature<arangodb::SystemDatabaseFeature>().use();
    ASSERT_NE(_sysVocbase, nullptr);

    _vocbase = nullptr;
    ASSERT_TRUE(
        server.getFeature<arangodb::DatabaseFeature>()
            .createDatabase(createInfo(server.server(), dbName, 1), _vocbase)
            .ok());
    ASSERT_NE(_vocbase, nullptr);
    std::shared_ptr<arangodb::LogicalCollection> unused;
    arangodb::OperationOptions options(arangodb::ExecContext::current());
    arangodb::methods::Collections::createSystem(
        *_vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
        unused);
    // Prepare analyzers
    analyzerFeature.prepare();  // add static analyzers

    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    ASSERT_TRUE(feature()
                    .emplace(result, sysName(), "TestAnalyzer",
                             VPackParser::fromJson("\"abc\"")->slice())
                    .ok());
    ASSERT_TRUE(feature()
                    .emplace(result, specificName(), "TestAnalyzer",
                             VPackParser::fromJson("\"def\"")->slice())
                    .ok());
  }

  void TearDown() override {
    // Not allowed to assert here
    if (server.server().hasFeature<arangodb::DatabaseFeature>()) {
      server.getFeature<arangodb::DatabaseFeature>().dropDatabase(dbName);
      _vocbase = nullptr;
    }
    analyzerFeature.unprepare();
  }

  arangodb::iresearch::IResearchAnalyzerFeature& feature() {
    return analyzerFeature;
  }

  std::string sysName() const {
    return arangodb::StaticStrings::SystemDatabase + shortName();
  }

  std::string specificName() const { return dbName + shortName(); }
  std::string shortName() const { return "::test_analyzer"; }

  TRI_vocbase_t* system() const { return _sysVocbase.get(); }

  TRI_vocbase_t* specificBase() const { return _vocbase; }
};

TEST_F(IResearchAnalyzerFeatureGetTest, test_get_valid) {
  auto pool = feature().get(analyzerName(),
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
  EXPECT_EQUAL_SLICES(VPackParser::fromJson("{\"args\":\"abc\"}")->slice(),
                      pool->properties());
  auto analyzer = pool.get();
  EXPECT_NE(analyzer, nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest, test_get_global_system) {
  auto sysVocbase = system();
  ASSERT_NE(sysVocbase, nullptr);
  auto pool = feature().get(analyzerName(), *sysVocbase,
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
  EXPECT_EQUAL_SLICES(VPackParser::fromJson("{\"args\":\"abc\"}")->slice(),
                      pool->properties());
  auto analyzer = pool.get();
  EXPECT_NE(analyzer, nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest, test_get_global_specific) {
  auto vocbase = specificBase();
  ASSERT_NE(vocbase, nullptr);
  auto pool = feature().get(analyzerName(), *vocbase,
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
  EXPECT_EQUAL_SLICES(VPackParser::fromJson("{\"args\":\"abc\"}")->slice(),
                      pool->properties());
  auto analyzer = pool.get();
  EXPECT_NE(analyzer, nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest,
       test_get_global_specific_analyzer_name_only) {
  auto vocbase = specificBase();
  ASSERT_NE(vocbase, nullptr);
  auto pool = feature().get(shortName(), *vocbase,
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
  EXPECT_EQUAL_SLICES(VPackParser::fromJson("{\"args\":\"abc\"}")->slice(),
                      pool->properties());
  auto analyzer = pool.get();
  EXPECT_NE(analyzer, nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest,
       test_get_local_system_analyzer_no_colons) {
  auto vocbase = specificBase();
  ASSERT_NE(vocbase, nullptr);
  auto pool = feature().get("test_analyzer", *vocbase,
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
  EXPECT_EQUAL_SLICES(VPackParser::fromJson("{\"args\":\"def\"}")->slice(),
                      pool->properties());
  auto analyzer = pool.get();
  EXPECT_NE(analyzer, nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest,
       test_get_local_including_collection_name) {
  auto vocbase = specificBase();
  ASSERT_NE(vocbase, nullptr);
  auto pool = feature().get(specificName(), *vocbase,
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arangodb::iresearch::Features{}, pool->features());
  EXPECT_EQUAL_SLICES(VPackParser::fromJson("{\"args\":\"def\"}")->slice(),
                      pool->properties());
  auto analyzer = pool.get();
  EXPECT_NE(analyzer, nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest, test_get_failure_invalid_name) {
  auto pool =
      feature().get(arangodb::StaticStrings::SystemDatabase + "::invalid",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  EXPECT_EQ(pool, nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest,
       test_get_failure_invalid_name_adding_vocbases) {
  auto sysVocbase = system();
  ASSERT_NE(sysVocbase, nullptr);
  auto pool = feature().get(
      arangodb::StaticStrings::SystemDatabase + "::invalid", *sysVocbase,
      arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  EXPECT_EQ(pool, nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest,
       test_get_failure_invalid_short_name_adding_vocbases) {
  auto sysVocbase = system();
  ASSERT_NE(sysVocbase, nullptr);
  auto pool = feature().get("::invalid", *sysVocbase,
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  EXPECT_EQ(pool, nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest,
       test_get_failure_invalid_short_name_no_colons_adding_vocbases) {
  auto sysVocbase = system();
  ASSERT_NE(sysVocbase, nullptr);
  auto pool = feature().get("invalid", *sysVocbase,
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  EXPECT_EQ(pool, nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest,
       test_get_failure_invalid_type_adding_vocbases) {
  auto sysVocbase = system();
  ASSERT_NE(sysVocbase, nullptr);
  auto pool = feature().get("testAnalyzer", *sysVocbase,
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  EXPECT_EQ(pool, nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest, test_get_static_analyzer) {
  auto pool =
      feature().get("identity", arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(
      arangodb::iresearch::Features(arangodb::iresearch::FieldFeatures::NORM,
                                    irs::IndexFeatures::FREQ),
      pool->features());
  auto analyzer = pool->get();
  ASSERT_NE(analyzer.get(), nullptr);
}

TEST_F(IResearchAnalyzerFeatureGetTest,
       test_get_static_analyzer_adding_vocbases) {
  auto sysVocbase = system();
  ASSERT_NE(sysVocbase, nullptr);
  auto pool = feature().get("identity", *sysVocbase,
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(
      arangodb::iresearch::Features(arangodb::iresearch::FieldFeatures::NORM,
                                    irs::IndexFeatures::FREQ),
      pool->features());
  auto analyzer = pool->get();
  ASSERT_NE(analyzer.get(), nullptr);
}

// -----------------------------------------------------------------------------
// --SECTION--                                            coordinator test suite
// -----------------------------------------------------------------------------

class IResearchAnalyzerFeatureCoordinatorTest
    : public ::testing::Test,
      public arangodb::tests::LogSuppressor<arangodb::Logger::CLUSTER,
                                            arangodb::LogLevel::FATAL>,
      public arangodb::tests::LogSuppressor<arangodb::Logger::ENGINES,
                                            arangodb::LogLevel::FATAL>,
      public arangodb::tests::LogSuppressor<arangodb::Logger::FIXME,
                                            arangodb::LogLevel::ERR> {
 public:
  arangodb::tests::mocks::MockCoordinator server;
  std::string _dbName;
  arangodb::SystemDatabaseFeature::ptr _system;
  TRI_vocbase_t* _vocbase;
  arangodb::iresearch::IResearchAnalyzerFeature& _feature;

 protected:
  IResearchAnalyzerFeatureCoordinatorTest()
      : server("CRDN_0001"),
        _dbName("TestVocbase"),
        _system(server.getFeature<arangodb::SystemDatabaseFeature>().use()),
        _feature(
            server
                .getFeature<arangodb::iresearch::IResearchAnalyzerFeature>()) {
    arangodb::tests::init();

    // server.addFeature<arangodb::ViewTypesFeature>(true);

    TransactionStateMock::abortTransactionCount = 0;
    TransactionStateMock::beginTransactionCount = 0;
    TransactionStateMock::commitTransactionCount = 0;
  }

  void SetUp() override {
    auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();

    _vocbase = nullptr;
    ASSERT_TRUE(
        dbFeature
            .createDatabase(createInfo(server.server(), _dbName, 1), _vocbase)
            .ok());
    ASSERT_NE(_vocbase, nullptr);
  }

  void TearDown() override {
    // Not allowed to assert here
    if (server.server().hasFeature<arangodb::DatabaseFeature>()) {
      server.getFeature<arangodb::DatabaseFeature>().dropDatabase(_dbName);
      _vocbase = nullptr;
    }
  }

  arangodb::iresearch::IResearchAnalyzerFeature& feature() {
    // Cannot use TestAsserts here, only in void funtions
    return _feature;
  }

  std::string sysName() const {
    return arangodb::StaticStrings::SystemDatabase + shortName();
  }

  std::string specificName() const { return _dbName + shortName(); }
  std::string shortName() const { return "::test_analyzer"; }

  TRI_vocbase_t* system() const { return _system.get(); }

  TRI_vocbase_t* specificBase() const { return _vocbase; }
};

TEST_F(IResearchAnalyzerFeatureCoordinatorTest, test_ensure_index_add_factory) {
  // add index factory
  {
    struct IndexTypeFactory : public arangodb::IndexTypeFactory {
      IndexTypeFactory(arangodb::ArangodServer& server)
          : arangodb::IndexTypeFactory(server) {}

      virtual bool equal(arangodb::velocypack::Slice lhs,
                         arangodb::velocypack::Slice rhs,
                         std::string const&) const override {
        return false;
      }

      std::shared_ptr<arangodb::Index> instantiate(
          arangodb::LogicalCollection& collection,
          arangodb::velocypack::Slice definition, arangodb::IndexId id,
          bool isClusterConstructor) const override {
        EXPECT_TRUE(
            collection.vocbase()
                .server()
                .hasFeature<arangodb::iresearch::IResearchAnalyzerFeature>());
        return std::make_shared<TestIndex>(id, collection, definition);
      }

      virtual arangodb::Result normalize(
          arangodb::velocypack::Builder& normalized,
          arangodb::velocypack::Slice definition, bool isCreation,
          TRI_vocbase_t const& vocbase) const override {
        EXPECT_TRUE(arangodb::iresearch::mergeSlice(normalized, definition));
        return arangodb::Result();
      }
    };
    static const IndexTypeFactory indexTypeFactory(server.server());
    auto& indexFactory = const_cast<arangodb::IndexFactory&>(
        server.getFeature<arangodb::EngineSelectorFeature>()
            .engine()
            .indexFactory());
    indexFactory.emplace("testType", indexTypeFactory);
  }

  // get missing via link creation (coordinator) ensure no recursive
  // ClusterInfo::loadPlan() call
  {
    auto createCollectionJson =
        VPackParser::fromJson(std::string("{ \"id\": 42, \"name\": \"") +
                              arangodb::tests::AnalyzerCollectionName +
                              "\", \"isSystem\": true, \"shards\": { }, "
                              "\"type\": 2 }");  // 'id' and 'shards' required
                                                 // for coordinator tests
    auto collectionId = std::to_string(42);

    auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

    std::shared_ptr<arangodb::LogicalCollection> logicalCollection;
    auto res = arangodb::methods::Collections::lookup(
        *system(), arangodb::tests::AnalyzerCollectionName, logicalCollection);
    ASSERT_TRUE(res.ok());
    ASSERT_NE(nullptr, logicalCollection);

    // simulate heartbeat thread
    // We need this call BEFORE creation of collection if at all
    {
      auto const colPath = "/Current/Collections/_system/" +
                           std::to_string(logicalCollection->id().id());
      auto const colValue = VPackParser::fromJson(
          "{ \"same-as-dummy-shard-id\": { \"indexes\": [ { "
          "\"id\": \"43\" "
          "} ], \"servers\": [ \"same-as-dummy-shard-server\" ] } "
          "}");  // '1' must match 'idString' in
                 // ClusterInfo::ensureIndexCoordinatorInner(...)
      EXPECT_TRUE(arangodb::AgencyComm(server.server())
                      .setValue(colPath, colValue->slice(), 0.0)
                      .successful());
      auto const dummyPath = "/Plan/Collections";
      auto const dummyValue = VPackParser::fromJson(
          "{ \"_system\": { \"" + std::to_string(logicalCollection->id().id()) +
          "\": { \"name\": \"testCollection\", "
          "\"shards\": { \"same-as-dummy-shard-id\": [ "
          "\"same-as-dummy-shard-server\" ] } } } }");
      EXPECT_TRUE(arangodb::AgencyComm(server.server())
                      .setValue(dummyPath, dummyValue->slice(), 0.0)
                      .successful());
      auto const versionPath = "/Plan/Version";
      auto const versionValue =
          VPackParser::fromJson(std::to_string(ci.getPlanVersion() + 1));
      EXPECT_TRUE((arangodb::AgencyComm(server.server())
                       .setValue(versionPath, versionValue->slice(), 0.0)
                       .successful()));  // force loadPlan() update
    }

    arangodb::velocypack::Builder builder;
    arangodb::velocypack::Builder tmp;

    builder.openObject();
    builder.add(arangodb::StaticStrings::IndexType,
                arangodb::velocypack::Value("testType"));
    builder.add(arangodb::StaticStrings::IndexFields,
                arangodb::velocypack::Slice::emptyArraySlice());
    builder.add("id", VPackValue("43"));
    builder.close();
    res = arangodb::methods::Indexes::ensureIndex(*logicalCollection,
                                                  builder.slice(), true, tmp);
    EXPECT_TRUE(res.ok());
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                               identity test suite
// -----------------------------------------------------------------------------
TEST_F(IResearchAnalyzerFeatureTest, test_identity_static) {
  auto pool = arangodb::iresearch::IResearchAnalyzerFeature::identity();
  ASSERT_NE(nullptr, pool);
  EXPECT_EQ(
      arangodb::iresearch::Features(arangodb::iresearch::FieldFeatures::NORM,
                                    irs::IndexFeatures::FREQ),
      pool->features());
  EXPECT_EQ("identity", pool->name());
  auto analyzer = pool->get();
  ASSERT_NE(nullptr, analyzer.get());
  auto* term = irs::get<irs::term_attribute>(*analyzer);
  ASSERT_NE(nullptr, term);
  EXPECT_TRUE(analyzer->reset("abc def ghi"));
  EXPECT_TRUE(analyzer->next());
  EXPECT_EQ(irs::ViewCast<irs::byte_type>(std::string_view("abc def ghi")),
            term->value);
  EXPECT_FALSE(analyzer->next());
  EXPECT_TRUE(analyzer->reset("123 456"));
  EXPECT_TRUE(analyzer->next());
  EXPECT_EQ(irs::ViewCast<irs::byte_type>(std::string_view("123 456")),
            term->value);
  EXPECT_FALSE(analyzer->next());
}
TEST_F(IResearchAnalyzerFeatureTest, test_identity_registered) {
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  feature.prepare();  // add static analyzers
  EXPECT_FALSE(
      !feature.get("identity", arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
  auto pool =
      feature.get("identity", arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
  ASSERT_NE(nullptr, pool);
  EXPECT_EQ(
      arangodb::iresearch::Features(arangodb::iresearch::FieldFeatures::NORM,
                                    irs::IndexFeatures::FREQ),
      pool->features());
  EXPECT_EQ("identity", pool->name());
  auto analyzer = pool->get();
  ASSERT_NE(nullptr, analyzer.get());
  auto* term = irs::get<irs::term_attribute>(*analyzer);
  ASSERT_NE(nullptr, term);
  EXPECT_FALSE(analyzer->next());
  EXPECT_TRUE(analyzer->reset("abc def ghi"));
  EXPECT_TRUE(analyzer->next());
  EXPECT_EQ(irs::ViewCast<irs::byte_type>(std::string_view("abc def ghi")),
            term->value);
  EXPECT_FALSE(analyzer->next());
  EXPECT_TRUE(analyzer->reset("123 456"));
  EXPECT_TRUE(analyzer->next());
  EXPECT_EQ(irs::ViewCast<irs::byte_type>(std::string_view("123 456")),
            term->value);
  EXPECT_FALSE(analyzer->next());
  feature.unprepare();
}

// -----------------------------------------------------------------------------
// --SECTION--                                              normalize test suite
// -----------------------------------------------------------------------------

TEST_F(IResearchAnalyzerFeatureTest, test_normalize) {
  TRI_vocbase_t active(testDBInfo(server.server(), "active", 2));
  TRI_vocbase_t system(systemDBInfo(server.server()));

  // normalize 'identity' (with prefix)
  {
    std::string_view analyzer = "identity";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), true);
    EXPECT_EQ(std::string("identity"), normalized);
  }

  // normalize 'identity' (without prefix)
  {
    std::string_view analyzer = "identity";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), true);
    EXPECT_EQ(std::string("identity"), normalized);
  }

  // normalize NIL (with prefix)
  {
    std::string_view analyzer = std::string_view{};
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), true);
    EXPECT_EQ(std::string("active::"), normalized);
  }

  // normalize NIL (without prefix)
  {
    std::string_view analyzer = std::string_view{};
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), false);
    EXPECT_EQ(std::string(""), normalized);
  }

  // normalize EMPTY (with prefix)
  {
    std::string_view analyzer = irs::kEmptyStringView<char>;
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), true);
    EXPECT_EQ(std::string("active::"), normalized);
  }

  // normalize EMPTY (without prefix)
  {
    std::string_view analyzer = irs::kEmptyStringView<char>;
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), false);
    EXPECT_EQ(std::string(""), normalized);
  }

  // normalize delimiter (with prefix)
  {
    std::string_view analyzer = "::";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), true);
    EXPECT_EQ(std::string("_system::"), normalized);
  }

  // normalize delimiter (without prefix)
  {
    std::string_view analyzer = "::";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), false);
    EXPECT_EQ(std::string("::"), normalized);
  }

  // normalize delimiter + name (with prefix)
  {
    std::string_view analyzer = "::name";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), true);
    EXPECT_EQ(std::string("_system::name"), normalized);
  }

  // normalize delimiter + name (without prefix)
  {
    std::string_view analyzer = "::name";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), false);
    EXPECT_EQ(std::string("::name"), normalized);
  }

  // normalize no-delimiter + name (with prefix)
  {
    std::string_view analyzer = "name";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), true);
    EXPECT_EQ(std::string("active::name"), normalized);
  }

  // normalize no-delimiter + name (without prefix)
  {
    std::string_view analyzer = "name";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), false);
    EXPECT_EQ(std::string("name"), normalized);
  }

  // normalize system + delimiter (with prefix)
  {
    std::string_view analyzer = "_system::";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), true);
    EXPECT_EQ(std::string("_system::"), normalized);
  }

  // normalize system + delimiter (without prefix)
  {
    std::string_view analyzer = "_system::";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), false);
    EXPECT_EQ(std::string("::"), normalized);
  }

  // normalize vocbase + delimiter (with prefix)
  {
    std::string_view analyzer = "active::";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), true);
    EXPECT_EQ(std::string("active::"), normalized);
  }

  // normalize vocbase + delimiter (without prefix)
  {
    std::string_view analyzer = "active::";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), false);
    EXPECT_EQ(std::string(""), normalized);
  }

  // normalize system + delimiter + name (with prefix)
  {
    std::string_view analyzer = "_system::name";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), true);
    EXPECT_EQ(std::string("_system::name"), normalized);
  }

  // normalize system + delimiter + name (without prefix)
  {
    std::string_view analyzer = "_system::name";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), false);
    EXPECT_EQ(std::string("::name"), normalized);
  }

  // normalize system + delimiter + name (without prefix) in system
  {
    std::string_view analyzer = "_system::name";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, system.name(), false);
    EXPECT_EQ(std::string("name"), normalized);
  }

  // normalize vocbase + delimiter + name (with prefix)
  {
    std::string_view analyzer = "active::name";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), true);
    EXPECT_EQ(std::string("active::name"), normalized);
  }

  // normalize vocbase + delimiter + name (without prefix)
  {
    std::string_view analyzer = "active::name";
    auto normalized = arangodb::iresearch::IResearchAnalyzerFeature::normalize(
        analyzer, active.name(), false);
    EXPECT_EQ(std::string("name"), normalized);
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                        static_analyzer test suite
// -----------------------------------------------------------------------------

TEST_F(IResearchAnalyzerFeatureTest, test_static_analyzer_features) {
  // test registered 'identity'
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  feature.prepare();  // add static analyzers
  for (auto& analyzerEntry : staticAnalyzers()) {
    EXPECT_FALSE(!feature.get(analyzerEntry.first,
                              arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    auto pool = feature.get(analyzerEntry.first,
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST);
    ASSERT_FALSE(!pool);
    EXPECT_EQ(analyzerEntry.second.features, pool->features());
    EXPECT_EQ(analyzerEntry.first, pool->name());
    auto analyzer = pool->get();
    EXPECT_FALSE(!analyzer);
    auto* term = irs::get<irs::term_attribute>(*analyzer);
    EXPECT_FALSE(!term);
  }
  feature.unprepare();
}

// -----------------------------------------------------------------------------
// --SECTION--                                            persistence test suite
// -----------------------------------------------------------------------------

TEST_F(IResearchAnalyzerFeatureTest,
       test_persistence_invalid_missing_attributes) {
  static std::vector<std::string> const EMPTY;
  auto& database = server.getFeature<arangodb::SystemDatabaseFeature>();
  auto vocbase = database.use();

  // read invalid configuration (missing attributes)
  {
    {
      std::string collection(arangodb::tests::AnalyzerCollectionName);
      arangodb::OperationOptions options;
      arangodb::SingleCollectionTransaction trx(
          arangodb::transaction::StandaloneContext::Create(*vocbase),
          collection, arangodb::AccessMode::Type::WRITE);
      trx.begin();
      trx.truncate(collection, options);
      trx.insert(collection, VPackParser::fromJson("{}")->slice(), options);
      trx.insert(collection,
                 VPackParser::fromJson("{\"type\": \"identity\", "
                                       "\"properties\": null}")
                     ->slice(),
                 options);
      trx.insert(collection,
                 VPackParser::fromJson(
                     "{\"name\": 12345,        \"type\": \"identity\", "
                     "\"properties\": null}")
                     ->slice(),
                 options);
      trx.insert(collection,
                 VPackParser::fromJson(
                     "{\"name\": \"invalid1\",                         "
                     "\"properties\": null}")
                     ->slice(),
                 options);
      trx.insert(collection,
                 VPackParser::fromJson(
                     "{\"name\": \"invalid2\", \"type\": 12345,        "
                     "\"properties\": null}")
                     ->slice(),
                 options);
      auto res = trx.commit();
      EXPECT_TRUE(res.ok());
    }

    std::map<std::string, std::pair<std::string_view, std::string_view>>
        expected = {};
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());

    feature.start();  // load persisted analyzers

    feature.visit(
        [&expected](
            arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
          if (staticAnalyzers().find(analyzer->name()) !=
              staticAnalyzers().end()) {
            return true;  // skip static analyzers
          }

          auto itr = expected.find(analyzer->name());
          EXPECT_NE(itr, expected.end());
          EXPECT_EQ(itr->second.first, analyzer->type());
          EXPECT_EQ(itr->second.second, analyzer->properties().toString());
          expected.erase(itr);
          return true;
        });
    EXPECT_TRUE(expected.empty());
    feature.stop();
  }
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_persistence_invalid_duplicate_records) {
  static std::vector<std::string> const EMPTY;
  auto& database = server.getFeature<arangodb::SystemDatabaseFeature>();
  auto vocbase = database.use();

  // read invalid configuration (duplicate non-identical records)
  {
    {
      std::string collection(arangodb::tests::AnalyzerCollectionName);
      arangodb::OperationOptions options;
      arangodb::SingleCollectionTransaction trx(
          arangodb::transaction::StandaloneContext::Create(*vocbase),
          collection, arangodb::AccessMode::Type::WRITE);
      trx.begin();
      trx.truncate(collection, options);
      trx.insert(collection,
                 VPackParser::fromJson(
                     "{\"name\": \"valid\", \"type\": \"TestAnalyzer\", "
                     "\"properties\": {\"args\":\"abcd\"} }")
                     ->slice(),
                 options);
      trx.insert(collection,
                 VPackParser::fromJson(
                     "{\"name\": \"valid\", \"type\": \"TestAnalyzer\", "
                     "\"properties\": {\"args\":\"abc\"} }")
                     ->slice(),
                 options);
      auto res = trx.commit();
      EXPECT_TRUE(res.ok());
    }

    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    EXPECT_NO_THROW(feature.start());
  }
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_persistence_valid_different_parameters) {
  static std::vector<std::string> const EMPTY;
  auto& database = server.getFeature<arangodb::SystemDatabaseFeature>();
  auto vocbase = database.use();

  // read valid configuration (different parameter options)
  {
    {
      std::string collection(arangodb::tests::AnalyzerCollectionName);
      arangodb::OperationOptions options;
      arangodb::SingleCollectionTransaction trx(
          arangodb::transaction::StandaloneContext::Create(*vocbase),
          collection, arangodb::AccessMode::Type::WRITE);
      trx.begin();
      trx.truncate(collection, options);
      trx.insert(collection,
                 VPackParser::fromJson(
                     "{\"name\": \"valid0\", \"type\": \"identity\", "
                     "\"properties\": {}                      }")
                     ->slice(),
                 options);
      trx.insert(collection,
                 VPackParser::fromJson(
                     "{\"name\": \"valid1\", \"type\": \"identity\", "
                     "\"properties\": true                      }")
                     ->slice(),
                 options);
      trx.insert(collection,
                 VPackParser::fromJson(
                     "{\"name\": \"valid2\", \"type\": \"identity\", "
                     "\"properties\": {\"args\":\"abc\"}        }")
                     ->slice(),
                 options);
      trx.insert(collection,
                 VPackParser::fromJson(
                     "{\"name\": \"valid3\", \"type\": \"identity\", "
                     "\"properties\": 3.14                      }")
                     ->slice(),
                 options);
      trx.insert(collection,
                 VPackParser::fromJson(
                     "{\"name\": \"valid4\", \"type\": \"identity\", "
                     "\"properties\": [ 1, \"abc\" ]            }")
                     ->slice(),
                 options);
      trx.insert(collection,
                 VPackParser::fromJson(
                     "{\"name\": \"valid5\", \"type\": \"identity\", "
                     "\"properties\": { \"a\": 7, \"b\": \"c\" }}")
                     ->slice(),
                 options);
      auto res = trx.commit();
      EXPECT_TRUE(res.ok());
    }

    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    feature.start();  // feature doesn't load persisted analyzers

    EXPECT_TRUE(feature.visit(
        [](arangodb::iresearch::AnalyzerPool::ptr const&) { return false; }));

    feature.stop();
  }
}

TEST_F(IResearchAnalyzerFeatureTest, test_persistence_add_new_records) {
  static std::vector<std::string> const EMPTY;
  auto& database = server.getFeature<arangodb::SystemDatabaseFeature>();
  auto vocbase = database.use();

  // add new records
  {
    {
      arangodb::OperationOptions options;
      auto collection =
          vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);
      arangodb::transaction::Methods trx(
          arangodb::transaction::StandaloneContext::Create(*vocbase), EMPTY,
          EMPTY, EMPTY, arangodb::transaction::Options());
      bool usedRangeDelete;
      EXPECT_TRUE(collection->truncate(trx, options, usedRangeDelete).ok());
    }

    {
      arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
      arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());

      EXPECT_TRUE(
          feature
              .emplace(result,
                       arangodb::StaticStrings::SystemDatabase + "::valid",
                       "identity",
                       VPackParser::fromJson("{\"args\":\"abc\"}")->slice())
              .ok());
      EXPECT_TRUE(result.first);
      EXPECT_TRUE(result.second);
    }

    {
      arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());

      feature.start();  // feature doesn't load persisted analyzers

      EXPECT_TRUE(feature.visit(
          [](arangodb::iresearch::AnalyzerPool::ptr const&) { return false; }));

      feature.stop();
    }
  }
}

TEST_F(IResearchAnalyzerFeatureTest, test_persistence_remove_existing_records) {
  static std::vector<std::string> const EMPTY;
  auto& database = server.getFeature<arangodb::SystemDatabaseFeature>();
  auto vocbase = database.use();

  // remove existing records
  {
    {
      std::string collection(arangodb::tests::AnalyzerCollectionName);
      arangodb::OperationOptions options;
      arangodb::SingleCollectionTransaction trx(
          arangodb::transaction::StandaloneContext::Create(*vocbase),
          collection, arangodb::AccessMode::Type::WRITE);

      trx.begin();
      trx.truncate(collection, options);
      trx.insert(collection,
                 VPackParser::fromJson("{\"name\": \"valid\", \"type\": "
                                       "\"identity\", \"properties\": {}}")
                     ->slice(),
                 options);
      auto res = trx.commit();
      EXPECT_TRUE(res.ok());
    }

    {
      std::map<std::string, std::pair<std::string_view, std::string_view>>
          expected = {
              {"text_de",
               {"text",
                "{ \"locale\": \"de.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"noStrem\": false "
                "}"}},
              {"text_en",
               {"text",
                "{ \"locale\": \"en.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"noStrem\": false "
                "}"}},
              {"text_es",
               {"text",
                "{ \"locale\": \"es.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"noStrem\": false "
                "}"}},
              {"text_fi",
               {"text",
                "{ \"locale\": \"fi.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"noStrem\": false "
                "}"}},
              {"text_fr",
               {"text",
                "{ \"locale\": \"fr.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"noStrem\": false "
                "}"}},
              {"text_it",
               {"text",
                "{ \"locale\": \"it.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"noStrem\": false "
                "}"}},
              {"text_nl",
               {"text",
                "{ \"locale\": \"nl.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"noStrem\": false "
                "}"}},
              {"text_no",
               {"text",
                "{ \"locale\": \"no.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"noStrem\": false "
                "}"}},
              {"text_pt",
               {"text",
                "{ \"locale\": \"pt.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"noStrem\": false "
                "}"}},
              {"text_ru",
               {"text",
                "{ \"locale\": \"ru.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"noStrem\": false "
                "}"}},
              {"text_sv",
               {"text",
                "{ \"locale\": \"sv.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"noStrem\": false "
                "}"}},
              {"text_zh",
               {"text",
                "{ \"locale\": \"zh.UTF-8\", \"caseConvert\": \"lower\", "
                "\"stopwords\": [ ], \"noAccent\": true, \"stemming\": "
                "false}"}},
              {"identity", {"identity", "{\n}"}},
          };
      arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());

      feature.prepare();  // load static analyzers
      feature.start();    // doesn't load persisted analyzers

      feature.visit(
          [&expected](
              arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
            auto itr = expected.find(analyzer->name());
            EXPECT_NE(itr, expected.end());
            EXPECT_EQ(itr->second.first, analyzer->type());

            std::string expectedProperties;

            EXPECT_TRUE(irs::analysis::analyzers::normalize(
                expectedProperties, analyzer->type(),
                irs::type<irs::text_format::vpack>::get(),
                arangodb::iresearch::ref<char>(
                    VPackParser::fromJson(itr->second.second.data(),
                                          itr->second.second.size())
                        ->slice()),
                false));

            EXPECT_EQUAL_SLICES(arangodb::iresearch::slice(expectedProperties),
                                analyzer->properties());
            expected.erase(itr);
            return true;
          });

      EXPECT_TRUE(expected.empty());
      EXPECT_FALSE(
          feature.remove(arangodb::StaticStrings::SystemDatabase + "::valid")
              .ok());
      EXPECT_FALSE(feature.remove("identity").ok());

      feature.stop();
      feature.unprepare();
    }

    {
      std::map<std::string, std::pair<std::string_view, std::string_view>>
          expected = {};
      arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());

      feature.start();  // doesn't load persisted analyzers

      EXPECT_TRUE(feature.visit(
          [](arangodb::iresearch::AnalyzerPool::ptr const&) { return false; }));

      feature.stop();
    }
  }
}

TEST_F(IResearchAnalyzerFeatureTest,
       test_persistence_emplace_on_single_server) {
  static std::vector<std::string> const EMPTY;
  auto& database = server.getFeature<arangodb::SystemDatabaseFeature>();
  auto vocbase = database.use();

  // emplace on single-server (should persist)
  {
    // clear collection
    {
      std::string collection(arangodb::tests::AnalyzerCollectionName);
      arangodb::OperationOptions options;
      arangodb::SingleCollectionTransaction trx(
          arangodb::transaction::StandaloneContext::Create(*vocbase),
          collection, arangodb::AccessMode::Type::WRITE);
      trx.begin();
      trx.truncate(collection, options);
      auto res = trx.commit();
      EXPECT_TRUE(res.ok());
    }

    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_analyzerA",
                             "TestAnalyzer",
                             VPackParser::fromJson("\"abc\"")->slice(),
                             {{}, irs::IndexFeatures::FREQ})
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_TRUE(feature.get(
        arangodb::StaticStrings::SystemDatabase + "::test_analyzerA",
        arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    EXPECT_TRUE(
        vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName));
    arangodb::OperationOptions options;
    arangodb::SingleCollectionTransaction trx(
        arangodb::transaction::StandaloneContext::Create(*vocbase),
        arangodb::tests::AnalyzerCollectionName,
        arangodb::AccessMode::Type::WRITE);
    EXPECT_TRUE((trx.begin().ok()));
    auto queryResult =
        trx.all(arangodb::tests::AnalyzerCollectionName, 0, 2, options);
    EXPECT_TRUE((true == queryResult.ok()));
    auto slice = arangodb::velocypack::Slice(queryResult.buffer->data());
    EXPECT_TRUE(slice.isArray());
    ASSERT_EQ(1, slice.length());
    slice = slice.at(0);
    EXPECT_TRUE(slice.isObject());
    EXPECT_TRUE(slice.hasKey("_key") && slice.get("_key").isString() &&
                std::string("test_analyzerA") ==
                    slice.get("_key").copyString());
    EXPECT_TRUE(slice.hasKey("name") && slice.get("name").isString() &&
                std::string("test_analyzerA") ==
                    slice.get("name").copyString());
    EXPECT_TRUE(slice.hasKey("type") && slice.get("type").isString() &&
                std::string("TestAnalyzer") == slice.get("type").copyString());
    EXPECT_TRUE(
        slice.hasKey("properties") && slice.get("properties").isObject() &&
        VPackParser::fromJson("{\"args\":\"abc\"}")->slice().toString() ==
            slice.get("properties").toString());
    EXPECT_TRUE(slice.hasKey("features") && slice.get("features").isArray() &&
                1 == slice.get("features").length() &&
                slice.get("features").at(0).isString() &&
                std::string("frequency") ==
                    slice.get("features").at(0).copyString());
    EXPECT_TRUE(
        trx.truncate(arangodb::tests::AnalyzerCollectionName, options).ok());
    EXPECT_TRUE(trx.commit().ok());
  }
}

TEST_F(IResearchAnalyzerFeatureTest, test_analyzer_features) {
  {
    arangodb::iresearch::AnalyzerPool::ptr pool;
    ASSERT_TRUE(
        arangodb::iresearch::IResearchAnalyzerFeature::createAnalyzerPool(
            pool, "db::test", "TestAnalyzer",
            VPackParser::fromJson("\"abc\"")->slice(),
            arangodb::AnalyzersRevision::MIN, arangodb::iresearch::Features{},
            arangodb::iresearch::LinkVersion::MIN, false)
            .ok());
    ASSERT_NE(nullptr, pool);
    ASSERT_EQ(arangodb::iresearch::Features{}, pool->features());
    ASSERT_EQ(irs::IndexFeatures::NONE, pool->indexFeatures());
    ASSERT_TRUE(pool->fieldFeatures().empty());
  }

  {
    arangodb::iresearch::AnalyzerPool::ptr pool;
    ASSERT_TRUE(
        arangodb::iresearch::IResearchAnalyzerFeature::createAnalyzerPool(
            pool, "db::test", "TestAnalyzer",
            VPackParser::fromJson("\"abc\"")->slice(),
            arangodb::AnalyzersRevision::MIN,
            arangodb::iresearch::Features{irs::IndexFeatures::FREQ},
            arangodb::iresearch::LinkVersion::MIN, false)
            .ok());
    ASSERT_NE(nullptr, pool);
    ASSERT_EQ(arangodb::iresearch::Features{irs::IndexFeatures::FREQ},
              pool->features());
    ASSERT_EQ(irs::IndexFeatures::FREQ, pool->indexFeatures());
    ASSERT_TRUE(pool->fieldFeatures().empty());
  }

  // norm, version 0
  {
    arangodb::iresearch::AnalyzerPool::ptr pool;
    ASSERT_TRUE(
        arangodb::iresearch::IResearchAnalyzerFeature::createAnalyzerPool(
            pool, "db::test", "TestAnalyzer",
            VPackParser::fromJson("\"abc\"")->slice(),
            arangodb::AnalyzersRevision::MIN,
            arangodb::iresearch::Features{
                arangodb::iresearch::FieldFeatures::NORM,
                irs::IndexFeatures::FREQ},
            arangodb::iresearch::LinkVersion::MIN, false)
            .ok());
    ASSERT_NE(nullptr, pool);
    ASSERT_EQ(
        (arangodb::iresearch::Features{arangodb::iresearch::FieldFeatures::NORM,
                                       irs::IndexFeatures::FREQ}),
        pool->features());
    ASSERT_EQ(irs::IndexFeatures::FREQ, pool->indexFeatures());
    irs::type_info::type_id const expected[]{irs::type<irs::Norm>::id()};
    auto features = pool->fieldFeatures();
    ASSERT_TRUE(
        std::equal(expected, expected + 1, features.begin(), features.end()));
  }

  // norm, version 1
  {
    arangodb::iresearch::AnalyzerPool::ptr pool;
    ASSERT_TRUE(
        arangodb::iresearch::IResearchAnalyzerFeature::createAnalyzerPool(
            pool, "db::test", "TestAnalyzer",
            VPackParser::fromJson("\"abc\"")->slice(),
            arangodb::AnalyzersRevision::MIN,
            arangodb::iresearch::Features{
                arangodb::iresearch::FieldFeatures::NORM,
                irs::IndexFeatures::FREQ},
            arangodb::iresearch::LinkVersion::MAX, false)
            .ok());
    ASSERT_NE(nullptr, pool);
    ASSERT_EQ(
        (arangodb::iresearch::Features{arangodb::iresearch::FieldFeatures::NORM,
                                       irs::IndexFeatures::FREQ}),
        pool->features());
    ASSERT_EQ(irs::IndexFeatures::FREQ, pool->indexFeatures());
    irs::type_info::type_id const expected[]{irs::type<irs::Norm2>::id()};
    auto features = pool->fieldFeatures();
    ASSERT_TRUE(
        std::equal(expected, expected + 1, features.begin(), features.end()));
  }

  // frequency is not set
  {
    arangodb::iresearch::AnalyzerPool::ptr pool;
    ASSERT_FALSE(
        arangodb::iresearch::IResearchAnalyzerFeature::createAnalyzerPool(
            pool, "db::test", "TestAnalyzer",
            VPackParser::fromJson("\"abc\"")->slice(),
            arangodb::AnalyzersRevision::MIN,
            arangodb::iresearch::Features{irs::IndexFeatures::POS},
            arangodb::iresearch::LinkVersion::MIN, false)
            .ok());
    ASSERT_EQ(nullptr, pool);
  }
}

TEST_F(IResearchAnalyzerFeatureTest, test_analyzer_equality) {
  arangodb::iresearch::AnalyzerPool::ptr lhs;
  ASSERT_TRUE(arangodb::iresearch::IResearchAnalyzerFeature::createAnalyzerPool(
                  lhs, "db::test", "TestAnalyzer",
                  VPackParser::fromJson("\"abc\"")->slice(),
                  arangodb::AnalyzersRevision::MIN,
                  arangodb::iresearch::Features{},
                  arangodb::iresearch::LinkVersion::MIN, false)
                  .ok());
  ASSERT_NE(nullptr, lhs);
  ASSERT_EQ(*lhs, *lhs);

  // different name
  {
    arangodb::iresearch::AnalyzerPool::ptr rhs;
    ASSERT_TRUE(
        arangodb::iresearch::IResearchAnalyzerFeature::createAnalyzerPool(
            rhs, "db::test1", "TestAnalyzer",
            VPackParser::fromJson("\"abc\"")->slice(),
            arangodb::AnalyzersRevision::MIN, arangodb::iresearch::Features{},
            arangodb::iresearch::LinkVersion::MIN, false)
            .ok());
    ASSERT_NE(nullptr, rhs);
    ASSERT_NE(*lhs, *rhs);
  }

  // different type
  {
    arangodb::iresearch::AnalyzerPool::ptr rhs;
    ASSERT_TRUE(
        arangodb::iresearch::IResearchAnalyzerFeature::createAnalyzerPool(
            rhs, "db::test", "ReNormalizingAnalyzer",
            VPackParser::fromJson("\"abc\"")->slice(),
            arangodb::AnalyzersRevision::MIN, arangodb::iresearch::Features{},
            arangodb::iresearch::LinkVersion::MIN, false)
            .ok());
    ASSERT_NE(nullptr, rhs);
    ASSERT_NE(*lhs, *rhs);
  }

  // different properties
  {
    arangodb::iresearch::AnalyzerPool::ptr rhs;
    ASSERT_TRUE(
        arangodb::iresearch::IResearchAnalyzerFeature::createAnalyzerPool(
            rhs, "db::test", "TestAnalyzer",
            VPackParser::fromJson("\"abcd\"")->slice(),
            arangodb::AnalyzersRevision::MIN, arangodb::iresearch::Features{},
            arangodb::iresearch::LinkVersion::MIN, false)
            .ok());
    ASSERT_NE(nullptr, rhs);
    ASSERT_NE(*lhs, *rhs);
  }

  // different features
  {
    arangodb::iresearch::AnalyzerPool::ptr rhs;
    ASSERT_TRUE(
        arangodb::iresearch::IResearchAnalyzerFeature::createAnalyzerPool(
            rhs, "db::test", "TestAnalyzer",
            VPackParser::fromJson("\"abcd\"")->slice(),
            arangodb::AnalyzersRevision::MIN,
            arangodb::iresearch::Features(irs::IndexFeatures::FREQ),
            arangodb::iresearch::LinkVersion::MIN, false)
            .ok());
    ASSERT_NE(nullptr, rhs);
    ASSERT_NE(*lhs, *rhs);
  }

  // different revision - this is still the same analyzer!
  {
    arangodb::iresearch::AnalyzerPool::ptr rhs;
    ASSERT_TRUE(
        arangodb::iresearch::IResearchAnalyzerFeature::createAnalyzerPool(
            rhs, "db::test", "TestAnalyzer",
            VPackParser::fromJson("\"abc\"")->slice(),
            arangodb::AnalyzersRevision::MIN + 1,
            arangodb::iresearch::Features{},
            arangodb::iresearch::LinkVersion::MIN, false)
            .ok());
    ASSERT_NE(nullptr, rhs);
    ASSERT_EQ(*lhs, *rhs);
  }
}

TEST_F(IResearchAnalyzerFeatureTest, test_remove) {
  VPackBuilder bogus;
  {
    VPackArrayBuilder trxs(&bogus);
    {
      VPackArrayBuilder trx(&bogus);
      {
        VPackObjectBuilder op(&bogus);
        bogus.add("a", VPackValue(12));
      }
    }
  }
  server.server()
      .getFeature<arangodb::ClusterFeature>()
      .agencyCache()
      .applyTestTransaction(bogus.slice());

  arangodb::network::ConnectionPool::Config poolConfig(
      server.server().getFeature<arangodb::metrics::MetricsFeature>());
  poolConfig.clusterInfo =
      &server.getFeature<arangodb::ClusterFeature>().clusterInfo();
  poolConfig.numIOThreads = 1;
  poolConfig.maxOpenConnections = 3;
  poolConfig.verifyHosts = false;
  poolConfig.name = "IResearchAnalyzerFeatureTest";

  AsyncAgencyStorePoolMock pool(server.server(), poolConfig);
  arangodb::AgencyCommHelper::initialize("arango");
  arangodb::AsyncAgencyCommManager::initialize(server.server());
  arangodb::AsyncAgencyCommManager::INSTANCE->pool(&pool);
  arangodb::AsyncAgencyCommManager::INSTANCE->addEndpoint(
      "tcp://localhost:4000/");
  arangodb::AgencyComm(server.server()).ensureStructureInitialized();

  ASSERT_TRUE(server.server().hasFeature<arangodb::DatabaseFeature>());
  auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();

  // remove existing
  {
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    feature.prepare();  // add static analyzers

    // add analyzer
    {
      arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
      ASSERT_TRUE(feature
                      .emplace(result,
                               arangodb::StaticStrings::SystemDatabase +
                                   "::test_analyzer0",
                               "TestAnalyzer",
                               VPackParser::fromJson("\"abc\"")->slice())
                      .ok());
      ASSERT_NE(nullptr,
                feature.get(arangodb::StaticStrings::SystemDatabase +
                                "::test_analyzer0",
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    }

    EXPECT_TRUE(feature
                    .remove(arangodb::StaticStrings::SystemDatabase +
                            "::test_analyzer0")
                    .ok());
    EXPECT_EQ(nullptr,
              feature.get(
                  arangodb::StaticStrings::SystemDatabase + "::test_analyzer0",
                  arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    feature.unprepare();
  }

  // remove existing (inRecovery) single-server
  {
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());

    // add analyzer
    {
      arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
      ASSERT_TRUE(feature
                      .emplace(result,
                               arangodb::StaticStrings::SystemDatabase +
                                   "::test_analyzer0",
                               "TestAnalyzer",
                               VPackParser::fromJson("\"abc\"")->slice())
                      .ok());
      ASSERT_NE(nullptr,
                feature.get(arangodb::StaticStrings::SystemDatabase +
                                "::test_analyzer0",
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    }

    auto before = StorageEngineMock::recoveryStateResult;
    StorageEngineMock::recoveryStateResult =
        arangodb::RecoveryState::IN_PROGRESS;
    irs::Finally restore = [&before]() noexcept {
      StorageEngineMock::recoveryStateResult = before;
    };

    EXPECT_FALSE(feature
                     .remove(arangodb::StaticStrings::SystemDatabase +
                             "::test_analyzer0")
                     .ok());
    EXPECT_NE(nullptr,
              feature.get(
                  arangodb::StaticStrings::SystemDatabase + "::test_analyzer0",
                  arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
  }

  // remove existing (dbserver)
  {
    auto beforeRole = arangodb::ServerState::instance()->getRole();
    arangodb::ServerState::instance()->setRole(
        arangodb::ServerState::ROLE_DBSERVER);
    irs::Finally restoreRole = [&beforeRole]() noexcept {
      arangodb::ServerState::instance()->setRole(beforeRole);
    };

    // create a new instance of an ApplicationServer and fill it with the
    // required features cannot use the existing server since its features
    // already have some state

    arangodb::ArangodServer newServer(nullptr, nullptr);
    newServer.addFeature<arangodb::metrics::MetricsFeature>();
    auto& cluster = newServer.addFeature<arangodb::ClusterFeature>();
    auto& networkFeature = newServer.addFeature<arangodb::NetworkFeature>();
    auto& dbFeature = newServer.addFeature<arangodb::DatabaseFeature>();
    auto& selector = newServer.addFeature<arangodb::EngineSelectorFeature>();
    StorageEngineMock engine(newServer);
    selector.setEngineTesting(&engine);
    newServer.addFeature<arangodb::QueryRegistryFeature>();
    newServer.addFeature<arangodb::ShardingFeature>();
    auto& sysDatabase = newServer.addFeature<arangodb::SystemDatabaseFeature>();
    newServer.addFeature<arangodb::V8DealerFeature>();
    newServer.addFeature<
        arangodb::application_features::CommunicationFeaturePhase>();
    auto& feature =
        newServer.addFeature<arangodb::iresearch::IResearchAnalyzerFeature>();

    cluster.prepare();
    networkFeature.prepare();
    dbFeature.prepare();

    auto cleanup = arangodb::scopeGuard([&, this]() noexcept {
      dbFeature.unprepare();
      networkFeature.unprepare();
      server.getFeature<arangodb::DatabaseFeature>().prepare();
    });

    // create system vocbase (before feature start)
    {
      auto databases = VPackBuilder();
      databases.openArray();
      databases.add(systemDatabaseArgs);
      databases.close();
      EXPECT_EQ(TRI_ERROR_NO_ERROR, dbFeature.loadDatabases(databases.slice()));
      sysDatabase.start();  // get system database from DatabaseFeature
    }

    newServer.getFeature<arangodb::ClusterFeature>()
        .agencyCache()
        .applyTestTransaction(bogus.slice());

    // add analyzer
    {
      arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
      ASSERT_EQ(nullptr,
                feature.get(arangodb::StaticStrings::SystemDatabase +
                                "::test_analyzer2",
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
      ASSERT_TRUE(feature
                      .emplace(result,
                               arangodb::StaticStrings::SystemDatabase +
                                   "::test_analyzer2",
                               "TestAnalyzer",
                               VPackParser::fromJson("\"abc\"")->slice())
                      .ok());
      ASSERT_NE(nullptr,
                feature.get(arangodb::StaticStrings::SystemDatabase +
                                "::test_analyzer2",
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    }

    EXPECT_TRUE(feature
                    .remove(arangodb::StaticStrings::SystemDatabase +
                            "::test_analyzer2")
                    .ok());
    EXPECT_EQ(nullptr,
              feature.get(
                  arangodb::StaticStrings::SystemDatabase + "::test_analyzer2",
                  arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
  }

  // remove existing (inRecovery) dbserver
  {
    auto beforeRole = arangodb::ServerState::instance()->getRole();
    arangodb::ServerState::instance()->setRole(
        arangodb::ServerState::ROLE_DBSERVER);
    irs::Finally restoreRole = [&beforeRole]() noexcept {
      arangodb::ServerState::instance()->setRole(beforeRole);
    };

    arangodb::ArangodServer newServer(nullptr, nullptr);
    newServer.addFeature<arangodb::metrics::MetricsFeature>();
    auto& auth = newServer.addFeature<arangodb::AuthenticationFeature>();
    auto& cluster = newServer.addFeature<arangodb::ClusterFeature>();
    auto& networkFeature = newServer.addFeature<arangodb::NetworkFeature>();
    auto& dbFeature = newServer.addFeature<arangodb::DatabaseFeature>();
    auto& selector = newServer.addFeature<arangodb::EngineSelectorFeature>();
    StorageEngineMock engine(newServer);
    selector.setEngineTesting(&engine);
    newServer.addFeature<arangodb::QueryRegistryFeature>();
    newServer.addFeature<arangodb::ShardingFeature>();
    auto& sysDatabase = newServer.addFeature<arangodb::SystemDatabaseFeature>();
    newServer.addFeature<arangodb::V8DealerFeature>();
    newServer.addFeature<
        arangodb::application_features::CommunicationFeaturePhase>();
    auto& feature =
        newServer.addFeature<arangodb::iresearch::IResearchAnalyzerFeature>();

    auth.prepare();
    cluster.prepare();
    networkFeature.prepare();
    dbFeature.prepare();

    auto cleanup = arangodb::scopeGuard([&, this]() noexcept {
      dbFeature.unprepare();
      networkFeature.unprepare();
      cluster.unprepare();
      auth.unprepare();
      server.getFeature<arangodb::DatabaseFeature>().prepare();
    });

    // create system vocbase (before feature start)
    {
      auto databases = VPackBuilder();
      databases.openArray();
      databases.add(systemDatabaseArgs);
      databases.close();
      EXPECT_EQ(TRI_ERROR_NO_ERROR, dbFeature.loadDatabases(databases.slice()));
      sysDatabase.start();  // get system database from DatabaseFeature
    }

    newServer.getFeature<arangodb::ClusterFeature>()
        .agencyCache()
        .applyTestTransaction(bogus.slice());
    // add analyzer
    {
      arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
      ASSERT_EQ(nullptr,
                feature.get(arangodb::StaticStrings::SystemDatabase +
                                "::test_analyzer2",
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
      ASSERT_TRUE(feature
                      .emplace(result,
                               arangodb::StaticStrings::SystemDatabase +
                                   "::test_analyzer2",
                               "TestAnalyzer",
                               VPackParser::fromJson("\"abc\"")->slice())
                      .ok());
      ASSERT_NE(nullptr,
                feature.get(arangodb::StaticStrings::SystemDatabase +
                                "::test_analyzer2",
                            arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    }

    auto before = StorageEngineMock::recoveryStateResult;
    StorageEngineMock::recoveryStateResult =
        arangodb::RecoveryState::IN_PROGRESS;
    irs::Finally restore = [&before]() noexcept {
      StorageEngineMock::recoveryStateResult = before;
    };

    EXPECT_TRUE(feature
                    .remove(arangodb::StaticStrings::SystemDatabase +
                            "::test_analyzer2")
                    .ok());
    EXPECT_EQ(nullptr,
              feature.get(
                  arangodb::StaticStrings::SystemDatabase + "::test_analyzer2",
                  arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
  }

  // remove existing (in-use)
  {
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult
        result;  // will keep reference
    ASSERT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_analyzer3",
                             "TestAnalyzer",
                             VPackParser::fromJson("\"abc\"")->slice())
                    .ok());
    ASSERT_NE(nullptr,
              feature.get(
                  arangodb::StaticStrings::SystemDatabase + "::test_analyzer3",
                  arangodb::QueryAnalyzerRevisions::QUERY_LATEST));

    EXPECT_FALSE(feature
                     .remove(arangodb::StaticStrings::SystemDatabase +
                                 "::test_analyzer3",
                             false)
                     .ok());
    EXPECT_NE(nullptr,
              feature.get(
                  arangodb::StaticStrings::SystemDatabase + "::test_analyzer3",
                  arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    EXPECT_TRUE(feature
                    .remove(arangodb::StaticStrings::SystemDatabase +
                                "::test_analyzer3",
                            true)
                    .ok());
    EXPECT_EQ(nullptr,
              feature.get(
                  arangodb::StaticStrings::SystemDatabase + "::test_analyzer3",
                  arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
  }

  // remove missing (no vocbase)
  {
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    ASSERT_EQ(nullptr, dbFeature.lookupDatabase("testVocbase"));

    EXPECT_EQ(nullptr,
              feature.get("testVocbase::test_analyzer",
                          arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    EXPECT_FALSE(feature.remove("testVocbase::test_analyzer").ok());
  }

  // remove missing (no collection)
  {
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    TRI_vocbase_t* vocbase;
    ASSERT_TRUE(
        dbFeature.createDatabase(testDBInfo(server.server()), vocbase).ok());
    ASSERT_NE(nullptr, dbFeature.lookupDatabase("testVocbase"));
    EXPECT_EQ(nullptr,
              feature.get("testVocbase::test_analyzer",
                          arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    EXPECT_FALSE(feature.remove("testVocbase::test_analyzer").ok());
  }

  // remove invalid
  {
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    EXPECT_EQ(
        nullptr,
        feature.get(arangodb::StaticStrings::SystemDatabase + "::test_analyzer",
                    arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    EXPECT_FALSE(
        feature
            .remove(arangodb::StaticStrings::SystemDatabase + "::test_analyzer")
            .ok());
  }

  // remove static analyzer
  {
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    feature.prepare();  // add static analyzers
    EXPECT_NE(nullptr,
              feature.get("identity",
                          arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
    EXPECT_FALSE(feature.remove("identity").ok());
    EXPECT_NE(nullptr,
              feature.get("identity",
                          arangodb::QueryAnalyzerRevisions::QUERY_LATEST));
  }
}

TEST_F(IResearchAnalyzerFeatureTest, test_prepare) {
  auto before = StorageEngineMock::recoveryStateResult;
  StorageEngineMock::recoveryStateResult = arangodb::RecoveryState::IN_PROGRESS;
  irs::Finally restore = [&before]() noexcept {
    StorageEngineMock::recoveryStateResult = before;
  };
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  EXPECT_TRUE(feature.visit(
      [](auto) { return false; }));  // ensure feature is empty after creation
  feature.prepare();                 // add static analyzers

  // check static analyzers
  auto expected = staticAnalyzers();
  feature.visit(
      [&expected, &feature](
          arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
        auto itr = expected.find(analyzer->name());
        EXPECT_NE(itr, expected.end());
        EXPECT_EQ(itr->second.type, analyzer->type());

        std::string expectedProperties;
        EXPECT_TRUE(irs::analysis::analyzers::normalize(
            expectedProperties, analyzer->type(),
            irs::type<irs::text_format::vpack>::get(),
            arangodb::iresearch::ref<char>(itr->second.properties), false));

        EXPECT_EQUAL_SLICES(arangodb::iresearch::slice(expectedProperties),
                            analyzer->properties());
        EXPECT_EQ(itr->second.features,
                  feature
                      .get(analyzer->name(),
                           arangodb::QueryAnalyzerRevisions::QUERY_LATEST)
                      ->features());
        expected.erase(itr);
        return true;
      });
  EXPECT_TRUE(expected.empty());
  feature.unprepare();
}

TEST_F(IResearchAnalyzerFeatureTest, test_start) {
  auto& database = server.getFeature<arangodb::SystemDatabaseFeature>();
  auto vocbase = database.use();

  // test feature start load configuration (inRecovery, no configuration
  // collection)
  {
    // ensure no configuration collection
    {
      auto collection =
          vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);

      if (collection) {
        auto res = vocbase->dropCollection(collection->id(), true);
        EXPECT_TRUE(res.ok());
      }

      collection =
          vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);
      EXPECT_EQ(nullptr, collection);
    }

    auto before = StorageEngineMock::recoveryStateResult;
    StorageEngineMock::recoveryStateResult =
        arangodb::RecoveryState::IN_PROGRESS;
    irs::Finally restore = [&before]() noexcept {
      StorageEngineMock::recoveryStateResult = before;
    };
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    feature.prepare();  // add static analyzers
    feature.start();    // load persisted analyzers
    EXPECT_EQ(nullptr, vocbase->lookupCollection(
                           arangodb::tests::AnalyzerCollectionName));

    auto expected = staticAnalyzers();

    feature.visit(
        [&expected, &feature](
            arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
          auto itr = expected.find(analyzer->name());
          EXPECT_NE(itr, expected.end());
          EXPECT_EQ(itr->second.type, analyzer->type());

          std::string expectedProperties;
          EXPECT_TRUE(irs::analysis::analyzers::normalize(
              expectedProperties, analyzer->type(),
              irs::type<irs::text_format::vpack>::get(),
              arangodb::iresearch::ref<char>(itr->second.properties), false));

          EXPECT_EQUAL_SLICES(arangodb::iresearch::slice(expectedProperties),
                              analyzer->properties());
          EXPECT_EQ(itr->second.features,
                    feature
                        .get(analyzer->name(),
                             arangodb::QueryAnalyzerRevisions::QUERY_LATEST)
                        ->features());
          expected.erase(itr);
          return true;
        });
    EXPECT_TRUE(expected.empty());
    feature.stop();
    feature.unprepare();
  }

  // test feature start load configuration (inRecovery, with configuration
  // collection)
  {
    // ensure there is an empty configuration collection
    {
      auto collection =
          vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);

      if (collection) {
        vocbase->dropCollection(collection->id(), true);
      }

      collection =
          vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);
      EXPECT_EQ(nullptr, collection);
      arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
      arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
      std::shared_ptr<arangodb::LogicalCollection> unused;
      arangodb::OperationOptions options(arangodb::ExecContext::current());
      arangodb::methods::Collections::createSystem(
          *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
          unused);
      EXPECT_TRUE(feature
                      .emplace(result,
                               arangodb::StaticStrings::SystemDatabase +
                                   "::test_analyzer",
                               "identity",
                               VPackParser::fromJson("\"abc\"")->slice())
                      .ok());
      EXPECT_FALSE(!result.first);
      collection =
          vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);
      EXPECT_NE(nullptr, collection);
    }

    auto before = StorageEngineMock::recoveryStateResult;
    StorageEngineMock::recoveryStateResult =
        arangodb::RecoveryState::IN_PROGRESS;
    irs::Finally restore = [&before]() noexcept {
      StorageEngineMock::recoveryStateResult = before;
    };
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    feature.prepare();  // add static analyzers
    feature.start();    // doesn't load persisted analyzers
    EXPECT_NE(nullptr, vocbase->lookupCollection(
                           arangodb::tests::AnalyzerCollectionName));

    auto expected = staticAnalyzers();

    feature.visit(
        [&expected, &feature](
            arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
          auto itr = expected.find(analyzer->name());
          EXPECT_NE(itr, expected.end());
          EXPECT_EQ(itr->second.type, analyzer->type());

          std::string expectedProperties;
          EXPECT_TRUE(irs::analysis::analyzers::normalize(
              expectedProperties, analyzer->type(),
              irs::type<irs::text_format::vpack>::get(),
              arangodb::iresearch::ref<char>(itr->second.properties), false));

          EXPECT_EQUAL_SLICES(arangodb::iresearch::slice(expectedProperties),
                              analyzer->properties());
          EXPECT_EQ(itr->second.features,
                    feature
                        .get(analyzer->name(),
                             arangodb::QueryAnalyzerRevisions::QUERY_LATEST)
                        ->features());
          expected.erase(itr);
          return true;
        });
    EXPECT_TRUE(expected.empty());
    feature.stop();
    feature.unprepare();
  }

  // test feature start load configuration (no configuration collection)
  {
    // ensure no configuration collection
    {
      auto collection =
          vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);

      if (collection) {
        vocbase->dropCollection(collection->id(), true);
      }

      collection =
          vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);
      EXPECT_EQ(nullptr, collection);
    }
    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    feature.prepare();  // add static analyzers
    feature.start();    // doesn't load persisted analyzers
    EXPECT_EQ(nullptr, vocbase->lookupCollection(
                           arangodb::tests::AnalyzerCollectionName));

    auto expected = staticAnalyzers();

    feature.visit(
        [&expected, &feature](
            arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
          auto itr = expected.find(analyzer->name());
          EXPECT_NE(itr, expected.end());
          EXPECT_EQ(itr->second.type, analyzer->type());

          std::string expectedProperties;
          EXPECT_TRUE(irs::analysis::analyzers::normalize(
              expectedProperties, analyzer->type(),
              irs::type<irs::text_format::vpack>::get(),
              arangodb::iresearch::ref<char>(itr->second.properties), false));

          EXPECT_EQUAL_SLICES(arangodb::iresearch::slice(expectedProperties),
                              analyzer->properties());
          EXPECT_EQ(itr->second.features,
                    feature
                        .get(analyzer->name(),
                             arangodb::QueryAnalyzerRevisions::QUERY_LATEST)
                        ->features());
          expected.erase(itr);
          return true;
        });
    EXPECT_TRUE(expected.empty());
    feature.stop();
    feature.unprepare();
  }

  // test feature start load configuration (with configuration collection)
  {
    // ensure there is an empty configuration collection
    {
      auto collection =
          vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);

      if (collection) {
        vocbase->dropCollection(collection->id(), true);
      }

      collection =
          vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);
      EXPECT_EQ(nullptr, collection);
      arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
      arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
      std::shared_ptr<arangodb::LogicalCollection> unused;
      arangodb::OperationOptions options(arangodb::ExecContext::current());
      arangodb::methods::Collections::createSystem(
          *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
          unused);
      EXPECT_TRUE(
          (true ==
           feature
               .emplace(
                   result,
                   arangodb::StaticStrings::SystemDatabase + "::test_analyzer",
                   "identity", VPackParser::fromJson("\"abc\"")->slice())
               .ok()));
      EXPECT_FALSE(!result.first);
      collection =
          vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);
      EXPECT_NE(nullptr, collection);
    }

    arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
    feature.prepare();  // add static analyzers
    feature.start();    // doesn't load persisted analyzers
    EXPECT_NE(nullptr, vocbase->lookupCollection(
                           arangodb::tests::AnalyzerCollectionName));

    auto expected = staticAnalyzers();

    feature.visit(
        [&expected, &feature](
            arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
          auto itr = expected.find(analyzer->name());
          EXPECT_NE(itr, expected.end());
          EXPECT_EQ(itr->second.type, analyzer->type());

          std::string expectedproperties;
          EXPECT_TRUE(irs::analysis::analyzers::normalize(
              expectedproperties, analyzer->type(),
              irs::type<irs::text_format::vpack>::get(),
              arangodb::iresearch::ref<char>(itr->second.properties), false));

          EXPECT_EQUAL_SLICES(arangodb::iresearch::slice(expectedproperties),
                              analyzer->properties());
          EXPECT_EQ(itr->second.features,
                    feature
                        .get(analyzer->name(),
                             arangodb::QueryAnalyzerRevisions::QUERY_LATEST)
                        ->features());
          expected.erase(itr);
          return true;
        });
    EXPECT_TRUE(expected.empty());
    feature.stop();
    feature.unprepare();
  }
}

TEST_F(IResearchAnalyzerFeatureTest, test_tokens) {
  // create a new instance of an ApplicationServer and fill it with the required
  // features cannot use the existing server since its features already have
  // some state
  arangodb::ArangodServer newServer(nullptr, nullptr);
  auto& analyzers =
      newServer.addFeature<arangodb::iresearch::IResearchAnalyzerFeature>();
  auto& dbfeature = newServer.addFeature<arangodb::DatabaseFeature>();
  auto& selector = newServer.addFeature<arangodb::EngineSelectorFeature>();
  StorageEngineMock engine(newServer);
  selector.setEngineTesting(&engine);
  auto& functions = newServer.addFeature<arangodb::aql::AqlFunctionFeature>();
  newServer.addFeature<arangodb::metrics::MetricsFeature>();
  newServer.addFeature<arangodb::ClusterFeature>();
  newServer.addFeature<arangodb::QueryRegistryFeature>();
  auto& sharding = newServer.addFeature<arangodb::ShardingFeature>();
  auto& systemdb = newServer.addFeature<arangodb::SystemDatabaseFeature>();
  newServer.addFeature<arangodb::V8DealerFeature>();

  auto cleanup = arangodb::scopeGuard([&dbfeature, this]() noexcept {
    dbfeature.unprepare();
    server.getFeature<arangodb::DatabaseFeature>().prepare();
  });

  sharding.prepare();

  // create system vocbase (before feature start)
  {
    auto databases = VPackBuilder();
    databases.openArray();
    databases.add(systemDatabaseArgs);
    databases.close();
    EXPECT_EQ(TRI_ERROR_NO_ERROR, dbfeature.loadDatabases(databases.slice()));

    systemdb.start();  // get system database from DatabaseFeature
  }

  auto vocbase = systemdb.use();
  // ensure there is no configuration collection
  {
    auto collection =
        vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);

    if (collection) {
      vocbase->dropCollection(collection->id(), true);
    }

    collection =
        vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName);
    EXPECT_EQ(nullptr, collection);
  }

  std::shared_ptr<arangodb::LogicalCollection> unused;
  arangodb::OperationOptions options(arangodb::ExecContext::current());
  arangodb::methods::Collections::createSystem(
      *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
      unused);
  // test function registration

  // AqlFunctionFeature::byName(..) throws exception instead of returning a
  // nullptr
  EXPECT_ANY_THROW((functions.byName("TOKENS")));
  analyzers.prepare();
  analyzers.start();  // load AQL functions
  // if failed to register - other tests makes no sense
  auto* function = functions.byName("TOKENS");
  ASSERT_NE(nullptr, function);
  auto& impl = function->implementation;
  ASSERT_NE(nullptr, impl);

  arangodb::aql::Function tkns("TOKENS", impl);
  arangodb::aql::AstNode node(arangodb::aql::NODE_TYPE_FCALL);
  node.setData(static_cast<void const*>(&tkns));

  arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
  analyzers.start();  // load AQL functions
  ASSERT_TRUE(
      (true ==
       analyzers
           .emplace(result,
                    arangodb::StaticStrings::SystemDatabase + "::test_analyzer",
                    "TestAnalyzer", VPackParser::fromJson("\"abc\"")->slice())
           .ok()));
  ASSERT_TRUE(
      (true ==
       analyzers
           .emplace(result,
                    arangodb::StaticStrings::SystemDatabase +
                        "::test_number_analyzer",
                    "iresearch-tokens-typed",
                    VPackParser::fromJson("{\"type\":\"number\"}")->slice())
           .ok()));
  ASSERT_TRUE(
      (true ==
       analyzers
           .emplace(
               result,
               arangodb::StaticStrings::SystemDatabase + "::test_bool_analyzer",
               "iresearch-tokens-typed",
               VPackParser::fromJson("{\"type\":\"bool\"}")->slice())
           .ok()));
  ASSERT_FALSE(!result.first);

  arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(*vocbase),
      arangodb::tests::AnalyzerCollectionName,
      arangodb::AccessMode::Type::WRITE);
  ExpressionContextMock exprCtx;
  exprCtx.setTrx(&trx);

  // test tokenization
  {
    std::string analyzer(arangodb::StaticStrings::SystemDatabase +
                         "::test_analyzer");
    std::string_view data("abcdefghijklmnopqrstuvwxyz");
    VPackFunctionParametersWrapper args;
    args->emplace_back(data.data(), data.size());
    args->emplace_back(analyzer.c_str(), analyzer.size());
    AqlValueWrapper result(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(26, result->length());

    for (int64_t i = 0; i < 26; ++i) {
      bool mustDestroy;
      auto entry = result->at(i, mustDestroy, false);
      EXPECT_TRUE(entry.isString());
      auto value = arangodb::iresearch::getStringRef(entry.slice());
      EXPECT_EQ(1, value.size());
      EXPECT_EQ('a' + i, value.data()[0]);
    }
  }
  // test default analyzer
  {
    std::string_view data("abcdefghijklmnopqrstuvwxyz");
    VPackFunctionParametersWrapper args;
    args->emplace_back(data.data(), data.size());
    AqlValueWrapper result(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(1, result->length());
    bool mustDestroy;
    auto entry = result->at(0, mustDestroy, false);
    EXPECT_TRUE(entry.isString());
    auto value = entry.slice().copyString();
    EXPECT_EQ(data, value);
  }

  // test typed analyzer tokenization BTS-357
  {
    std::string analyzer(arangodb::StaticStrings::SystemDatabase +
                         "::test_number_analyzer");
    std::string_view data("123");
    VPackFunctionParametersWrapper args;
    args->emplace_back(data.data(), data.size());
    args->emplace_back(analyzer.c_str(), analyzer.size());
    AqlValueWrapper result(impl(&exprCtx, node, *args));
    ASSERT_TRUE(result->isArray());
    ASSERT_EQ(3, result->length());
    std::string expected123[] = {
        "oL/wAAAAAAAA", "sL/wAAAAAA==", "wL/wAAA=", "0L/w",
        "oIAAAAAAAAAA", "sIAAAAAAAA==", "wIAAAAA=", "0IAA",
        "oL/wAAAAAAAA", "sL/wAAAAAA==", "wL/wAAA=", "0L/w"};
    for (size_t i = 0; i < result->length(); ++i) {
      bool mustDestroy;
      auto entry = result->at(i, mustDestroy, false).slice();
      ASSERT_TRUE(entry.isArray());
      ASSERT_EQ(4, entry.length());
      for (size_t j = 0; j < entry.length(); ++j) {
        auto actual = entry.at(j);
        ASSERT_TRUE(actual.isString());
        ASSERT_EQ(expected123[i * 4 + j], actual.copyString());
      }
    }
  }

  // test typed analyzer tokenization BTS-357
  {
    std::string analyzer(arangodb::StaticStrings::SystemDatabase +
                         "::test_bool_analyzer");
    std::string_view data("123");
    VPackFunctionParametersWrapper args;
    args->emplace_back(data.data(), data.size());
    args->emplace_back(analyzer.c_str(), analyzer.size());
    AqlValueWrapper result(impl(&exprCtx, node, *args));
    ASSERT_TRUE(result->isArray());
    ASSERT_EQ(3, result->length());
    std::string expected1[] = {"AA==", "/w==", "AA=="};
    for (size_t i = 0; i < result->length(); ++i) {
      bool mustDestroy;
      auto entry = result->at(i, mustDestroy, false).slice();
      ASSERT_TRUE(entry.isArray());
      ASSERT_TRUE(entry.at(0).isString());
      ASSERT_EQ(expected1[i], arangodb::iresearch::getStringRef(entry.at(0)));
    }
  }

  // test invalid arg count
  // Zero count (less than expected)
  {
    arangodb::aql::VPackFunctionParameters args;
    EXPECT_THROW(AqlValueWrapper(impl(&exprCtx, node, args)),
                 arangodb::basics::Exception);
  }
  // test invalid arg count
  // 3 parameters. More than expected
  {
    std::string_view data("abcdefghijklmnopqrstuvwxyz");
    std::string_view analyzer("identity");
    std::string_view unexpectedParameter("something");
    VPackFunctionParametersWrapper args;
    args->emplace_back(data.data(), data.size());
    args->emplace_back(analyzer.data(), analyzer.size());
    args->emplace_back(unexpectedParameter.data(), unexpectedParameter.size());
    EXPECT_THROW(AqlValueWrapper(impl(&exprCtx, node, *args)),
                 arangodb::basics::Exception);
  }

  // test values
  // 123.4
  std::string expected123P4[] = {"oMBe2ZmZmZma",
                                 "sMBe2ZmZmQ==", "wMBe2Zk=", "0MBe"};

  // 123
  std::string expected123[] = {"oMBewAAAAAAA",
                               "sMBewAAAAA==", "wMBewAA=", "0MBe"};

  // boolean true
  std::string expectedTrue("/w==");
  // boolean false
  std::string expectedFalse("AA==");

  // test double data type
  {
    VPackFunctionParametersWrapper args;
    args->emplace_back(arangodb::aql::AqlValueHintDouble(123.4));
    auto result = AqlValueWrapper(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(std::size(expected123P4), result->length());

    for (size_t i = 0; i < result->length(); ++i) {
      bool mustDestroy;
      auto entry = result->at(i, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isString());
      EXPECT_EQ(expected123P4[i], arangodb::iresearch::getStringRef(entry));
    }
  }
  // test integer data type
  {
    auto expected = 123;
    VPackFunctionParametersWrapper args;
    args->emplace_back(arangodb::aql::AqlValueHintInt(expected));
    auto result = AqlValueWrapper(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(std::size(expected123), result->length());

    for (size_t i = 0; i < result->length(); ++i) {
      bool mustDestroy;
      auto entry = result->at(i, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isString());
      EXPECT_EQ(expected123[i], arangodb::iresearch::getStringRef(entry));
    }
  }
  // test true bool
  {
    VPackFunctionParametersWrapper args;
    args->emplace_back(arangodb::aql::AqlValueHintBool(true));
    auto result = AqlValueWrapper(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(1, result->length());
    bool mustDestroy;
    auto entry = result->at(0, mustDestroy, false).slice();
    EXPECT_TRUE(entry.isString());
    EXPECT_EQ(expectedTrue, arangodb::iresearch::getStringRef(entry));
  }
  // test false bool
  {
    VPackFunctionParametersWrapper args;
    args->emplace_back(arangodb::aql::AqlValueHintBool(false));
    auto result = AqlValueWrapper(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(1, result->length());
    bool mustDestroy;
    auto entry = result->at(0, mustDestroy, false).slice();
    EXPECT_TRUE(entry.isString());
    EXPECT_EQ(expectedFalse, arangodb::iresearch::getStringRef(entry));
  }
  // test null data type
  {
    VPackFunctionParametersWrapper args;
    args->emplace_back(arangodb::aql::AqlValueHintNull());
    auto result = AqlValueWrapper(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(1, result->length());
    bool mustDestroy;
    auto entry = result->at(0, mustDestroy, false).slice();
    EXPECT_TRUE(entry.isString());
    EXPECT_EQ("", arangodb::iresearch::getStringRef(entry));
  }

  // test double type with not needed analyzer
  {
    std::string analyzer(arangodb::StaticStrings::SystemDatabase +
                         "::test_analyzer");
    VPackFunctionParametersWrapper args;
    args->emplace_back(arangodb::aql::AqlValueHintDouble(123.4));
    args->emplace_back(analyzer.c_str(), analyzer.size());
    auto result = AqlValueWrapper(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(std::size(expected123P4), result->length());

    for (size_t i = 0; i < result->length(); ++i) {
      bool mustDestroy;
      auto entry = result->at(i, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isString());
      EXPECT_EQ(expected123P4[i], arangodb::iresearch::getStringRef(entry));
    }
  }
  // test double type with not needed analyzer (invalid analyzer type)
  {
    std::string_view analyzer("invalid_analyzer");
    VPackFunctionParametersWrapper args;
    args->emplace_back(arangodb::aql::AqlValueHintDouble(123.4));
    args->emplace_back(analyzer.data(), analyzer.size());
    EXPECT_THROW(AqlValueWrapper(impl(&exprCtx, node, *args)),
                 arangodb::basics::Exception);
  }
  // test invalid analyzer (when analyzer needed for text)
  {
    std::string_view analyzer("invalid");
    std::string_view data("abcdefghijklmnopqrstuvwxyz");
    VPackFunctionParametersWrapper args;
    args->emplace_back(data.data(), data.size());
    args->emplace_back(analyzer.data(), analyzer.size());
    EXPECT_THROW(AqlValueWrapper(impl(&exprCtx, node, *args)),
                 arangodb::basics::Exception);
  }

  // empty array
  {
    VPackFunctionParametersWrapper args;
    args->emplace_back(arangodb::aql::AqlValueHintEmptyArray());
    auto result = AqlValueWrapper(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(1, result->length());
    bool mustDestroy;
    auto entry = result->at(0, mustDestroy, false).slice();
    EXPECT_TRUE(entry.isEmptyArray());
  }
  // empty nested array
  {
    VPackFunctionParametersWrapper args;
    arangodb::velocypack::Buffer<uint8_t> buffer;
    VPackBuilder builder(buffer);
    builder.openArray();
    builder.openArray();
    builder.close();
    builder.close();
    auto aqlValue = arangodb::aql::AqlValue(std::move(buffer));
    args->push_back(std::move(aqlValue));
    auto result = AqlValueWrapper(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(1, result->length());
    bool mustDestroy;
    auto entry = result->at(0, mustDestroy, false).slice();
    EXPECT_TRUE(entry.isArray());
    EXPECT_EQ(1, entry.length());
    auto entryNested = entry.at(0);
    EXPECT_TRUE(entryNested.isEmptyArray());
  }

  // non-empty nested array
  {
    VPackFunctionParametersWrapper args;
    arangodb::velocypack::Buffer<uint8_t> buffer;
    VPackBuilder builder(buffer);
    builder.openArray();
    builder.openArray();
    builder.openArray();
    builder.add(arangodb::velocypack::Value(true));
    builder.close();
    builder.close();
    builder.close();
    auto aqlValue = arangodb::aql::AqlValue(std::move(buffer));
    args->push_back(std::move(aqlValue));
    auto result = AqlValueWrapper(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(1, result->length());
    bool mustDestroy;
    auto entry = result->at(0, mustDestroy, false).slice();
    EXPECT_TRUE(entry.isArray());
    EXPECT_EQ(1, entry.length());
    auto nested = entry.at(0);
    EXPECT_TRUE(nested.isArray());
    EXPECT_EQ(1, nested.length());
    auto nested2 = nested.at(0);
    EXPECT_TRUE(nested2.isArray());
    EXPECT_EQ(1, nested2.length());
    auto booleanValue = nested2.at(0);
    EXPECT_TRUE(booleanValue.isString());
    EXPECT_EQ(expectedTrue, arangodb::iresearch::getStringRef(booleanValue));
  }

  // array of bools
  {
    arangodb::velocypack::Buffer<uint8_t> buffer;
    VPackBuilder builder(buffer);
    builder.openArray();
    builder.add(arangodb::velocypack::Value(true));
    builder.add(arangodb::velocypack::Value(false));
    builder.add(arangodb::velocypack::Value(true));
    builder.close();
    auto aqlValue = arangodb::aql::AqlValue(std::move(buffer));
    VPackFunctionParametersWrapper args;
    args->push_back(std::move(aqlValue));
    std::string_view analyzer("text_en");
    args->emplace_back(analyzer.data(), analyzer.size());
    auto result = AqlValueWrapper(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(3, result->length());
    {
      bool mustDestroy;
      auto entry = result->at(0, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(1, entry.length());
      auto booleanValue = entry.at(0);
      EXPECT_TRUE(booleanValue.isString());
      EXPECT_EQ(expectedTrue, arangodb::iresearch::getStringRef(booleanValue));
    }
    {
      bool mustDestroy;
      auto entry = result->at(1, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(1, entry.length());
      auto booleanValue = entry.at(0);
      EXPECT_TRUE(booleanValue.isString());
      EXPECT_EQ(expectedFalse, arangodb::iresearch::getStringRef(booleanValue));
    }
    {
      bool mustDestroy;
      auto entry = result->at(2, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(1, entry.length());
      auto booleanValue = entry.at(0);
      EXPECT_TRUE(booleanValue.isString());
      EXPECT_EQ(expectedTrue, arangodb::iresearch::getStringRef(booleanValue));
    }
  }

  // mixed values array
  // [ [[]], [['test', 123.4, true]], 123, 123.4, true, null, false, 'jumps',
  // ['quick', 'dog'] ]
  {
    VPackFunctionParametersWrapper args;
    arangodb::velocypack::Buffer<uint8_t> buffer;
    VPackBuilder builder(buffer);
    builder.openArray();
    // [[]]
    builder.openArray();
    builder.openArray();
    builder.close();
    builder.close();

    //[['test', 123.4, true]]
    builder.openArray();
    builder.openArray();
    builder.add(arangodb::velocypack::Value("test"));
    builder.add(arangodb::velocypack::Value(123.4));
    builder.add(arangodb::velocypack::Value(true));
    builder.close();
    builder.close();

    builder.add(arangodb::velocypack::Value(123));
    builder.add(arangodb::velocypack::Value(123.4));
    builder.add(arangodb::velocypack::Value(true));
    builder.add(VPackSlice::nullSlice());
    builder.add(arangodb::velocypack::Value(false));
    builder.add(arangodb::velocypack::Value("jumps"));

    //[ 'quick', 'dog' ]
    builder.openArray();
    builder.add(arangodb::velocypack::Value("quick"));
    builder.add(arangodb::velocypack::Value("dog"));
    builder.close();

    builder.close();

    auto aqlValue = arangodb::aql::AqlValue(std::move(buffer));
    args->push_back(std::move(aqlValue));
    std::string_view analyzer("text_en");
    args->emplace_back(analyzer.data(), analyzer.size());
    auto result = AqlValueWrapper(impl(&exprCtx, node, *args));
    EXPECT_TRUE(result->isArray());
    EXPECT_EQ(9, result->length());
    {
      bool mustDestroy;
      auto entry = result->at(0, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(1, entry.length());
      auto nested = entry.at(0);
      EXPECT_TRUE(nested.isArray());
      EXPECT_EQ(1, nested.length());
      auto nested2 = nested.at(0);
      EXPECT_TRUE(nested2.isEmptyArray());
    }
    {
      bool mustDestroy;
      auto entry = result->at(1, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(1, entry.length());
      auto nested = entry.at(0);
      EXPECT_TRUE(nested.isArray());
      EXPECT_EQ(3, nested.length());

      {
        auto textTokens = nested.at(0);
        EXPECT_TRUE(textTokens.isArray());
        EXPECT_EQ(1, textTokens.length());
        auto value = textTokens.at(0).copyString();
        EXPECT_STREQ("test", value.c_str());
      }
      {
        auto numberTokens = nested.at(1);
        EXPECT_TRUE(numberTokens.isArray());
        EXPECT_EQ(std::size(expected123P4), numberTokens.length());
        for (size_t i = 0; i < numberTokens.length(); ++i) {
          auto entry = numberTokens.at(i);
          EXPECT_TRUE(entry.isString());
          EXPECT_EQ(expected123P4[i], arangodb::iresearch::getStringRef(entry));
        }
      }
      {
        auto booleanTokens = nested.at(2);
        EXPECT_TRUE(booleanTokens.isArray());
        EXPECT_EQ(1, booleanTokens.length());
        auto booleanValue = booleanTokens.at(0);
        EXPECT_TRUE(booleanValue.isString());
        EXPECT_EQ(expectedTrue,
                  arangodb::iresearch::getStringRef(booleanValue));
      }
    }
    {
      bool mustDestroy;
      auto entry = result->at(2, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(std::size(expected123), entry.length());
      for (size_t i = 0; i < entry.length(); ++i) {
        auto numberSlice = entry.at(i);
        EXPECT_TRUE(numberSlice.isString());
        EXPECT_EQ(expected123[i],
                  arangodb::iresearch::getStringRef(numberSlice));
      }
    }
    {
      bool mustDestroy;
      auto entry = result->at(3, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(std::size(expected123P4), entry.length());
      for (size_t i = 0; i < entry.length(); ++i) {
        auto numberSlice = entry.at(i);
        EXPECT_TRUE(numberSlice.isString());
        EXPECT_EQ(expected123P4[i],
                  arangodb::iresearch::getStringRef(numberSlice));
      }
    }
    {
      bool mustDestroy;
      auto entry = result->at(4, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(1, entry.length());
      auto booleanValue = entry.at(0);
      EXPECT_TRUE(booleanValue.isString());
      EXPECT_EQ(expectedTrue, arangodb::iresearch::getStringRef(booleanValue));
    }
    {
      bool mustDestroy;
      auto entry = result->at(5, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(1, entry.length());
      auto nullSlice = entry.at(0);
      EXPECT_TRUE(nullSlice.isString());
      EXPECT_EQ("", arangodb::iresearch::getStringRef(nullSlice));
    }
    {
      bool mustDestroy;
      auto entry = result->at(6, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(1, entry.length());
      auto booleanValue = entry.at(0);
      EXPECT_TRUE(booleanValue.isString());
      EXPECT_EQ(expectedFalse, arangodb::iresearch::getStringRef(booleanValue));
    }
    {
      bool mustDestroy;
      auto entry = result->at(7, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(1, entry.length());
      auto textSlice = entry.at(0);
      EXPECT_TRUE(textSlice.isString());
      auto value = textSlice.copyString();
      EXPECT_EQ("jump", value);
    }
    {
      bool mustDestroy;
      auto entry = result->at(8, mustDestroy, false).slice();
      EXPECT_TRUE(entry.isArray());
      EXPECT_EQ(2, entry.length());
      {
        auto subArray = entry.at(0);
        EXPECT_TRUE(subArray.isArray());
        EXPECT_EQ(1, subArray.length());
        auto textSlice = subArray.at(0);
        EXPECT_TRUE(textSlice.isString());
        auto value = textSlice.copyString();
        EXPECT_EQ("quick", value);
      }
      {
        auto subArray = entry.at(1);
        EXPECT_TRUE(subArray.isArray());
        EXPECT_EQ(1, subArray.length());
        auto textSlice = subArray.at(0);
        EXPECT_TRUE(textSlice.isString());
        auto value = textSlice.copyString();
        EXPECT_EQ("dog", value);
      }
    }
  }
}

class IResearchAnalyzerFeatureUpgradeStaticLegacyTest
    : public IResearchAnalyzerFeatureTest {
 protected:
  arangodb::DatabaseFeature& dbFeature;
  arangodb::SystemDatabaseFeature& sysDatabase;

  std::string const LEGACY_ANALYZER_COLLECTION_NAME = "_iresearch_analyzers";
  std::string const ANALYZER_COLLECTION_QUERY =
      std::string("FOR d IN ") + arangodb::tests::AnalyzerCollectionName +
      " RETURN d";
  std::unordered_set<std::string> const EXPECTED_LEGACY_ANALYZERS = {
      "text_de", "text_en", "text_es", "text_fi", "text_fr", "text_it",
      "text_nl", "text_no", "text_pt", "text_ru", "text_sv", "text_zh",
  };
  std::shared_ptr<VPackBuilder> createCollectionJson = VPackParser::fromJson(
      std::string("{ \"id\": 42, \"name\": \"") +
      arangodb::tests::AnalyzerCollectionName +
      "\", \"isSystem\": true, \"shards\": { \"same-as-dummy-shard-id\": [ "
      "\"shard-server-does-not-matter\" ] }, \"type\": 2 }");  // 'id' and
                                                               // 'shards'
                                                               // required for
                                                               // coordinator
                                                               // tests
  std::shared_ptr<VPackBuilder> createLegacyCollectionJson =
      VPackParser::fromJson(std::string("{ \"id\": 43, \"name\": \"") +
                            LEGACY_ANALYZER_COLLECTION_NAME +
                            "\", \"isSystem\": true, \"shards\": { "
                            "\"shard-id-does-not-matter\": [ "
                            "\"shard-server-does-not-matter\" ] }, \"type\": 2 "
                            "}");  // 'id' and 'shards' required for coordinator
                                   // tests
  std::string collectionId = std::to_string(42);
  std::string legacyCollectionId = std::to_string(43);
  std::shared_ptr<VPackBuilder> versionJson =
      VPackParser::fromJson("{ \"version\": 0, \"tasks\": {} }");

 private:
  arangodb::DatabasePathFeature& dbPathFeature;

 protected:
  IResearchAnalyzerFeatureUpgradeStaticLegacyTest()
      : IResearchAnalyzerFeatureTest(),
        dbFeature(server.getFeature<arangodb::DatabaseFeature>()),
        sysDatabase(server.getFeature<arangodb::SystemDatabaseFeature>()),
        dbPathFeature(server.getFeature<arangodb::DatabasePathFeature>()) {}

  ~IResearchAnalyzerFeatureUpgradeStaticLegacyTest() {}
};

TEST_F(IResearchAnalyzerFeatureUpgradeStaticLegacyTest, no_system_no_analyzer) {
  // test no system, no analyzer collection (single-server)
  arangodb::iresearch::IResearchAnalyzerFeature feature(
      server.server());  // required for running upgrade task
  feature.start();       // register upgrade tasks

  TRI_vocbase_t* vocbase;
  EXPECT_TRUE(
      dbFeature.createDatabase(testDBInfo(server.server()), vocbase).ok());
  sysDatabase.unprepare();  // unset system vocbase
  // EXPECT_TRUE(arangodb::methods::Upgrade::startup(*vocbase, true,
  // false).ok()); // run upgrade collections are not created in upgrade tasks
  // within iresearch anymore. For that reason, we have to create the collection
  // here manually.
  // TODO: We should use global system creation here instead of all the
  // exissting manual stuff ...
  std::shared_ptr<arangodb::LogicalCollection> unused;
  arangodb::OperationOptions options(arangodb::ExecContext::current());
  arangodb::methods::Collections::createSystem(
      *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
      unused);

  EXPECT_FALSE(
      !vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName));
  auto result =
      arangodb::tests::executeQuery(*vocbase, ANALYZER_COLLECTION_QUERY);
  EXPECT_TRUE(result.result.ok());
  auto slice = result.data->slice();
  EXPECT_TRUE(slice.isArray());
  EXPECT_EQ(0, slice.length());
}

TEST_F(IResearchAnalyzerFeatureUpgradeStaticLegacyTest,
       no_system_with_analyzer) {
  // test no system, with analyzer collection (single-server)
  arangodb::iresearch::IResearchAnalyzerFeature feature(server.server());
  feature.start();

  std::unordered_set<std::string> expected{"abc"};
  TRI_vocbase_t* vocbase;
  EXPECT_TRUE(
      dbFeature.createDatabase(testDBInfo(server.server()), vocbase).ok());
  EXPECT_FALSE(!vocbase->createCollection(createCollectionJson->slice()));

  // add document to collection
  {
    arangodb::OperationOptions options;
    arangodb::SingleCollectionTransaction trx(
        arangodb::transaction::StandaloneContext::Create(*vocbase),
        arangodb::tests::AnalyzerCollectionName,
        arangodb::AccessMode::Type::WRITE);
    EXPECT_TRUE(trx.begin().ok());
    EXPECT_TRUE(
        (true ==
         trx.insert(arangodb::tests::AnalyzerCollectionName,
                    VPackParser::fromJson("{\"name\": \"abc\"}")->slice(),
                    options)
             .ok()));
    EXPECT_TRUE(trx.commit().ok());
  }

  sysDatabase.unprepare();  // unset system vocbase
  // EXPECT_TRUE(arangodb::methods::Upgrade::startup(*vocbase, true,
  // false).ok()); // run upgrade
  // TODO: We should use global system creation here instead of all the
  // exissting manual stuff ...
  std::shared_ptr<arangodb::LogicalCollection> unused;
  arangodb::OperationOptions options(arangodb::ExecContext::current());
  arangodb::methods::Collections::createSystem(
      *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
      unused);
  EXPECT_FALSE(
      !vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName));
  auto result =
      arangodb::tests::executeQuery(*vocbase, ANALYZER_COLLECTION_QUERY);
  EXPECT_TRUE(result.result.ok());
  auto slice = result.data->slice();
  EXPECT_TRUE(slice.isArray());

  for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
    auto resolved = itr.value().resolveExternals();
    EXPECT_TRUE(resolved.isObject());
    EXPECT_TRUE(resolved.get("name").isString());
    EXPECT_EQ(1, expected.erase(resolved.get("name").copyString()));
  }

  EXPECT_TRUE(expected.empty());
}

TEST_F(IResearchAnalyzerFeatureUpgradeStaticLegacyTest,
       system_no_legacy_no_analyzer) {
  // test system, no legacy collection, no analyzer collection (single-server)
  arangodb::iresearch::IResearchAnalyzerFeature feature(
      server.server());  // required for running upgrade task
  feature.start();       // register upgrade tasks

  // ensure no legacy collection after feature start
  {
    auto system = sysDatabase.use();
    auto collection = system->lookupCollection(LEGACY_ANALYZER_COLLECTION_NAME);
    ASSERT_FALSE(collection);
  }

  TRI_vocbase_t* vocbase;
  EXPECT_TRUE(
      dbFeature.createDatabase(testDBInfo(server.server()), vocbase).ok());
  // EXPECT_TRUE(arangodb::methods::Upgrade::startup(*vocbase, true,
  // false).ok()); // run upgrade
  // TODO: We should use global system creation here instead of all the
  // exissting manual stuff ...
  std::shared_ptr<arangodb::LogicalCollection> unused;
  arangodb::OperationOptions options(arangodb::ExecContext::current());
  arangodb::methods::Collections::createSystem(
      *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
      unused);
  EXPECT_FALSE(
      !vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName));
  auto result =
      arangodb::tests::executeQuery(*vocbase, ANALYZER_COLLECTION_QUERY);
  EXPECT_TRUE(result.result.ok());
  auto slice = result.data->slice();
  EXPECT_TRUE(slice.isArray());
  EXPECT_EQ(0, slice.length());
}

TEST_F(IResearchAnalyzerFeatureUpgradeStaticLegacyTest,
       system_no_legacy_with_analyzer) {
  // test system, no legacy collection, with analyzer collection (single-server)
  arangodb::iresearch::IResearchAnalyzerFeature feature(
      server.server());  // required for running upgrade task
  feature.start();       // register upgrade tasks

  // ensure no legacy collection after feature start
  {
    auto system = sysDatabase.use();
    auto collection = system->lookupCollection(LEGACY_ANALYZER_COLLECTION_NAME);
    ASSERT_FALSE(collection);
  }

  std::unordered_set<std::string> expected{"abc"};
  TRI_vocbase_t* vocbase;
  EXPECT_TRUE(
      dbFeature.createDatabase(testDBInfo(server.server()), vocbase).ok());
  EXPECT_FALSE(!vocbase->createCollection(createCollectionJson->slice()));

  // add document to collection
  {
    arangodb::OperationOptions options;
    arangodb::SingleCollectionTransaction trx(
        arangodb::transaction::StandaloneContext::Create(*vocbase),
        arangodb::tests::AnalyzerCollectionName,
        arangodb::AccessMode::Type::WRITE);
    EXPECT_TRUE(trx.begin().ok());
    EXPECT_TRUE(
        (true ==
         trx.insert(arangodb::tests::AnalyzerCollectionName,
                    VPackParser::fromJson("{\"name\": \"abc\"}")->slice(),
                    options)
             .ok()));
    EXPECT_TRUE(trx.commit().ok());
  }

  // EXPECT_TRUE(arangodb::methods::Upgrade::startup(*vocbase, true,
  // false).ok()); // run upgrade
  // TODO: We should use global system creation here instead of all the
  // exissting manual stuff ...
  std::shared_ptr<arangodb::LogicalCollection> unused;
  arangodb::OperationOptions options(arangodb::ExecContext::current());
  arangodb::methods::Collections::createSystem(
      *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
      unused);
  EXPECT_FALSE(
      !vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName));
  auto result =
      arangodb::tests::executeQuery(*vocbase, ANALYZER_COLLECTION_QUERY);
  EXPECT_TRUE(result.result.ok());
  auto slice = result.data->slice();
  EXPECT_TRUE(slice.isArray());

  for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
    auto resolved = itr.value().resolveExternals();
    EXPECT_TRUE(resolved.isObject());
    EXPECT_TRUE(resolved.get("name").isString());
    EXPECT_EQ(1, expected.erase(resolved.get("name").copyString()));
  }

  EXPECT_TRUE(expected.empty());
}

TEST_F(IResearchAnalyzerFeatureUpgradeStaticLegacyTest,
       system_with_legacy_no_analyzer) {
  // test system, with legacy collection, no analyzer collection (single-server)
  arangodb::iresearch::IResearchAnalyzerFeature feature(
      server.server());  // required for running upgrade task
  feature.start();       // register upgrade tasks

  // ensure legacy collection after feature start
  {
    auto system = sysDatabase.use();
    auto collection = system->lookupCollection(LEGACY_ANALYZER_COLLECTION_NAME);
    ASSERT_FALSE(collection);
    ASSERT_FALSE(
        !system->createCollection(createLegacyCollectionJson->slice()));
  }

  // add document to legacy collection after feature start
  {
    arangodb::OperationOptions options;
    auto system = sysDatabase.use();
    arangodb::SingleCollectionTransaction trx(
        arangodb::transaction::StandaloneContext::Create(*system),
        LEGACY_ANALYZER_COLLECTION_NAME, arangodb::AccessMode::Type::WRITE);
    EXPECT_TRUE(trx.begin().ok());
    EXPECT_TRUE(
        (true ==
         trx.insert(LEGACY_ANALYZER_COLLECTION_NAME,
                    VPackParser::fromJson("{\"name\": \"legacy\"}")->slice(),
                    options)
             .ok()));
    EXPECT_TRUE(trx.commit().ok());
  }

  TRI_vocbase_t* vocbase;
  EXPECT_TRUE(
      dbFeature.createDatabase(testDBInfo(server.server()), vocbase).ok());
  // EXPECT_TRUE(arangodb::methods::Upgrade::startup(*vocbase, true,
  // false).ok()); // run upgrade
  // TODO: We should use global system creation here instead of all the
  // exissting manual stuff ...
  std::shared_ptr<arangodb::LogicalCollection> unused;
  arangodb::OperationOptions options(arangodb::ExecContext::current());
  arangodb::methods::Collections::createSystem(
      *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
      unused);
  EXPECT_FALSE(
      !vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName));
  auto result =
      arangodb::tests::executeQuery(*vocbase, ANALYZER_COLLECTION_QUERY);
  EXPECT_TRUE(result.result.ok());
  auto slice = result.data->slice();
  EXPECT_TRUE(slice.isArray());
  EXPECT_EQ(0, slice.length());
}

TEST_F(IResearchAnalyzerFeatureUpgradeStaticLegacyTest,
       system_no_legacy_with_analyzer_2) {
  // test system, no legacy collection, with analyzer collection (single-server)
  arangodb::iresearch::IResearchAnalyzerFeature feature(
      server.server());  // required for running upgrade task
  feature.start();       // register upgrade tasks

  // ensure no legacy collection after feature start
  {
    auto system = sysDatabase.use();
    auto collection = system->lookupCollection(LEGACY_ANALYZER_COLLECTION_NAME);
    ASSERT_FALSE(collection);
  }

  std::set<std::string> expected{"abc"};
  TRI_vocbase_t* vocbase;
  EXPECT_TRUE(
      dbFeature.createDatabase(testDBInfo(server.server()), vocbase).ok());
  EXPECT_FALSE(!vocbase->createCollection(createCollectionJson->slice()));

  // add document to collection
  {
    arangodb::OperationOptions options;
    arangodb::SingleCollectionTransaction trx(
        arangodb::transaction::StandaloneContext::Create(*vocbase),
        arangodb::tests::AnalyzerCollectionName,
        arangodb::AccessMode::Type::WRITE);
    EXPECT_TRUE(trx.begin().ok());
    EXPECT_TRUE(
        (true ==
         trx.insert(arangodb::tests::AnalyzerCollectionName,
                    VPackParser::fromJson("{\"name\": \"abc\"}")->slice(),
                    options)
             .ok()));
    EXPECT_TRUE(trx.commit().ok());
  }

  // EXPECT_TRUE(arangodb::methods::Upgrade::startup(*vocbase, true,
  // false).ok()); // run upgrade
  // TODO: We should use global system creation here instead of all the
  // exissting manual stuff ...
  std::shared_ptr<arangodb::LogicalCollection> unused;
  arangodb::OperationOptions options(arangodb::ExecContext::current());
  arangodb::methods::Collections::createSystem(
      *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
      unused);
  EXPECT_FALSE(
      !vocbase->lookupCollection(arangodb::tests::AnalyzerCollectionName));
  auto result =
      arangodb::tests::executeQuery(*vocbase, ANALYZER_COLLECTION_QUERY);
  EXPECT_TRUE(result.result.ok());
  auto slice = result.data->slice();
  EXPECT_TRUE(slice.isArray());

  for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
    auto resolved = itr.value().resolveExternals();
    EXPECT_TRUE(resolved.isObject());
    EXPECT_TRUE(resolved.get("name").isString());
    EXPECT_EQ(1, expected.erase(resolved.get("name").copyString()));
  }

  EXPECT_TRUE(expected.empty());
}

namespace {
// helper function for string->vpack properties represenation conversion
template<class Container>
std::set<typename Container::value_type> makeVPackPropExpectedSet(
    const Container& stringPropContainer) {
  std::set<typename Container::value_type> expectedSet;
  for (auto& expectedEntry : stringPropContainer) {
    std::string normalizedProperties;
    auto vpack = VPackParser::fromJson(expectedEntry._properties);
    EXPECT_TRUE(irs::analysis::analyzers::normalize(
        normalizedProperties, expectedEntry._type,
        irs::type<irs::text_format::vpack>::get(),
        arangodb::iresearch::ref<char>(vpack->slice()), false));
    expectedSet.emplace(expectedEntry._name, normalizedProperties,
                        expectedEntry._features, expectedEntry._type);
  }
  return expectedSet;
}
}  // namespace

TEST_F(IResearchAnalyzerFeatureTest, test_visit) {
  struct ExpectedType {
    arangodb::iresearch::Features _features;
    std::string _name;
    std::string _properties;
    std::string _type;
    ExpectedType(std::string_view name, std::string_view properties,
                 arangodb::iresearch::Features const& features,
                 std::string_view type)
        : _features(features),
          _name(name),
          _properties(properties),
          _type(type) {}

    bool operator<(ExpectedType const& other) const {
      if (_name < other._name) {
        return true;
      }

      if (_name > other._name) {
        return false;
      }

      if (_properties < other._properties) {
        return true;
      }

      if (_properties > other._properties) {
        return false;
      }

      if (_features.indexFeatures() < other._features.indexFeatures()) {
        return true;
      }

      if (_features.indexFeatures() > other._features.indexFeatures()) {
        return false;
      }

      const auto fieldFeatures =
          _features.fieldFeatures(arangodb::iresearch::LinkVersion::MIN);
      const auto otherFieldFeatures =
          other._features.fieldFeatures(arangodb::iresearch::LinkVersion::MIN);

      if (fieldFeatures.size() < otherFieldFeatures.size()) {
        return true;
      }

      if (fieldFeatures.size() > otherFieldFeatures.size()) {
        return false;
      }

      if (_type < other._type) {
        return true;
      }
      if (_type > other._type) {
        return false;
      }

      return false;  // assume equal
    }
  };

  arangodb::ArangodServer newServer(nullptr, nullptr);
  arangodb::iresearch::IResearchAnalyzerFeature feature(newServer);
  auto& dbFeature = newServer.addFeature<arangodb::DatabaseFeature>();
  auto& selector = newServer.addFeature<arangodb::EngineSelectorFeature>();
  StorageEngineMock engine(newServer);
  selector.setEngineTesting(&engine);
  newServer.addFeature<arangodb::metrics::MetricsFeature>();
  newServer.addFeature<arangodb::ClusterFeature>();
  newServer.addFeature<arangodb::QueryRegistryFeature>();
  auto& sysDatabase = newServer.addFeature<arangodb::SystemDatabaseFeature>();
  newServer.addFeature<arangodb::V8DealerFeature>();

  // create system vocbase (before feature start)
  {
    auto databases = VPackBuilder();
    databases.openArray();
    databases.add(systemDatabaseArgs);
    databases.close();
    EXPECT_EQ(TRI_ERROR_NO_ERROR, dbFeature.loadDatabases(databases.slice()));
    sysDatabase.start();  // get system database from DatabaseFeature
    auto system = sysDatabase.use();
    std::shared_ptr<arangodb::LogicalCollection> unused;
    arangodb::OperationOptions options(arangodb::ExecContext::current());
    arangodb::methods::Collections::createSystem(
        *system, options, arangodb::tests::AnalyzerCollectionName, false,
        unused);
  }

  auto cleanup = arangodb::scopeGuard([&dbFeature, this]() noexcept {
    dbFeature.unprepare();
    server.getFeature<arangodb::DatabaseFeature>()
        .prepare();  // restore calculation vocbase
  });

  arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
  EXPECT_TRUE((
      true ==
      feature
          .emplace(result,
                   arangodb::StaticStrings::SystemDatabase + "::test_analyzer0",
                   "TestAnalyzer", VPackParser::fromJson("\"abc0\"")->slice())
          .ok()));
  EXPECT_FALSE(!result.first);
  EXPECT_TRUE((
      true ==
      feature
          .emplace(result,
                   arangodb::StaticStrings::SystemDatabase + "::test_analyzer1",
                   "TestAnalyzer", VPackParser::fromJson("\"abc1\"")->slice())
          .ok()));
  EXPECT_FALSE(!result.first);
  EXPECT_TRUE((
      true ==
      feature
          .emplace(result,
                   arangodb::StaticStrings::SystemDatabase + "::test_analyzer2",
                   "TestAnalyzer", VPackParser::fromJson("\"abc2\"")->slice())
          .ok()));
  EXPECT_FALSE(!result.first);

  // full visitation
  {
    std::set<ExpectedType> expected = {
        {arangodb::StaticStrings::SystemDatabase + "::test_analyzer0",
         "\"abc0\"",
         {},
         "TestAnalyzer"},
        {arangodb::StaticStrings::SystemDatabase + "::test_analyzer1",
         "\"abc1\"",
         {},
         "TestAnalyzer"},
        {arangodb::StaticStrings::SystemDatabase + "::test_analyzer2",
         "\"abc2\"",
         {},
         "TestAnalyzer"},
    };
    auto expectedSet = makeVPackPropExpectedSet(expected);
    auto result = feature.visit(
        [&expectedSet](
            arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
          if (staticAnalyzers().find(analyzer->name()) !=
              staticAnalyzers().end()) {
            return true;  // skip static analyzers
          }

          EXPECT_EQ(analyzer->type(), "TestAnalyzer");
          EXPECT_EQ(1,
                    expectedSet.erase(ExpectedType(
                        analyzer->name(),
                        arangodb::iresearch::ref<char>(analyzer->properties()),
                        analyzer->features(), analyzer->type())));
          return true;
        });
    EXPECT_TRUE(result);
    EXPECT_TRUE(expectedSet.empty());
  }

  // partial visitation
  {
    std::set<ExpectedType> expected = {
        {arangodb::StaticStrings::SystemDatabase + "::test_analyzer0",
         "\"abc0\"",
         {},
         "TestAnalyzer"},
        {arangodb::StaticStrings::SystemDatabase + "::test_analyzer1",
         "\"abc1\"",
         {},
         "TestAnalyzer"},
        {arangodb::StaticStrings::SystemDatabase + "::test_analyzer2",
         "\"abc2\"",
         {},
         "TestAnalyzer"},
    };
    auto expectedSet = makeVPackPropExpectedSet(expected);
    auto result = feature.visit(
        [&expectedSet](
            arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
          if (staticAnalyzers().find(analyzer->name()) !=
              staticAnalyzers().end()) {
            return true;  // skip static analyzers
          }

          EXPECT_EQ(analyzer->type(), "TestAnalyzer");
          EXPECT_EQ(1,
                    expectedSet.erase(ExpectedType(
                        analyzer->name(),
                        arangodb::iresearch::ref<char>(analyzer->properties()),
                        analyzer->features(), analyzer->type())));
          return false;
        });
    EXPECT_FALSE(result);
    EXPECT_EQ(2, expectedSet.size());
  }

  TRI_vocbase_t* vocbase0;
  TRI_vocbase_t* vocbase1;
  TRI_vocbase_t* vocbase2;
  EXPECT_TRUE(
      dbFeature
          .createDatabase(createInfo(server.server(), "vocbase0", 1), vocbase0)
          .ok());
  EXPECT_TRUE(
      dbFeature
          .createDatabase(createInfo(server.server(), "vocbase1", 1), vocbase1)
          .ok());
  EXPECT_TRUE(
      dbFeature
          .createDatabase(createInfo(server.server(), "vocbase2", 1), vocbase2)
          .ok());
  std::shared_ptr<arangodb::LogicalCollection> unused;
  arangodb::OperationOptions options(arangodb::ExecContext::current());
  arangodb::methods::Collections::createSystem(
      *vocbase0, options, arangodb::tests::AnalyzerCollectionName, false,
      unused);
  arangodb::methods::Collections::createSystem(
      *vocbase1, options, arangodb::tests::AnalyzerCollectionName, false,
      unused);
  arangodb::methods::Collections::createSystem(
      *vocbase2, options, arangodb::tests::AnalyzerCollectionName, false,
      unused);
  // add database-prefixed analyzers
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    EXPECT_TRUE(feature
                    .emplace(result, "vocbase2::test_analyzer3", "TestAnalyzer",
                             VPackParser::fromJson("\"abc3\"")->slice())
                    .ok());
    EXPECT_FALSE(!result.first);
    EXPECT_TRUE(feature
                    .emplace(result, "vocbase2::test_analyzer4", "TestAnalyzer",
                             VPackParser::fromJson("\"abc4\"")->slice())
                    .ok());
    EXPECT_FALSE(!result.first);
    EXPECT_TRUE(feature
                    .emplace(result, "vocbase1::test_analyzer5", "TestAnalyzer",
                             VPackParser::fromJson("\"abc5\"")->slice())
                    .ok());
    EXPECT_FALSE(!result.first);
  }

  // full visitation limited to a vocbase (empty)
  {
    std::set<ExpectedType> expected = {};
    auto result = feature.visit(
        [&expected](
            arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
          EXPECT_EQ(analyzer->type(), "TestAnalyzer");
          EXPECT_EQ(1,
                    expected.erase(ExpectedType(
                        analyzer->name(),
                        arangodb::iresearch::ref<char>(analyzer->properties()),
                        analyzer->features(), analyzer->type())));
          return true;
        },
        vocbase0);
    EXPECT_TRUE(result);
    EXPECT_TRUE(expected.empty());
  }

  // full visitation limited to a vocbase (non-empty)
  {
    std::set<ExpectedType> expected = {
        {"vocbase2::test_analyzer3", "\"abc3\"", {}, "TestAnalyzer"},
        {"vocbase2::test_analyzer4", "\"abc4\"", {}, "TestAnalyzer"},
    };
    auto expectedSet = makeVPackPropExpectedSet(expected);
    auto result = feature.visit(
        [&expectedSet](
            arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
          EXPECT_EQ(analyzer->type(), "TestAnalyzer");
          EXPECT_EQ(1,
                    expectedSet.erase(ExpectedType(
                        analyzer->name(),
                        arangodb::iresearch::ref<char>(analyzer->properties()),
                        analyzer->features(), analyzer->type())));
          return true;
        },
        vocbase2);
    EXPECT_TRUE(result);
    EXPECT_TRUE(expectedSet.empty());
  }

  // static analyzer visitation
  {
    std::vector<ExpectedType> expected = {
        {"identity",
         "{}",
         {arangodb::iresearch::FieldFeatures::NORM, irs::IndexFeatures::FREQ},
         "identity"},
        {"text_de",
         "{ \"locale\": \"de.UTF-8\", \"stopwords\": [ ] "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
        {"text_en",
         "{ \"locale\": \"en.UTF-8\", \"stopwords\": [ ] "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
        {"text_es",
         "{ \"locale\": \"es.UTF-8\", \"stopwords\": [ ] "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
        {"text_fi",
         "{ \"locale\": \"fi.UTF-8\", \"stopwords\": [ ] "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
        {"text_fr",
         "{ \"locale\": \"fr.UTF-8\", \"stopwords\": [ ] "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
        {"text_it",
         "{ \"locale\": \"it.UTF-8\", \"stopwords\": [ ] "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
        {"text_nl",
         "{ \"locale\": \"nl.UTF-8\", \"stopwords\": [ ] "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
        {"text_no",
         "{ \"locale\": \"no.UTF-8\", \"stopwords\": [ ] "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
        {"text_pt",
         "{ \"locale\": \"pt.UTF-8\", \"stopwords\": [ ] "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
        {"text_ru",
         "{ \"locale\": \"ru.UTF-8\", \"stopwords\": [ ] "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
        {"text_sv",
         "{ \"locale\": \"sv.UTF-8\", \"stopwords\": [ ] "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
        {"text_zh",
         "{ \"locale\": \"zh.UTF-8\", \"stopwords\": [ ], \"stemming\":false "
         "}",
         {arangodb::iresearch::FieldFeatures::NORM,
          irs::IndexFeatures::FREQ | irs::IndexFeatures::POS},
         "text"},
    };

    auto expectedSet = makeVPackPropExpectedSet(expected);
    ASSERT_EQ(expected.size(), expectedSet.size());

    auto result = feature.visit(
        [&expectedSet](
            arangodb::iresearch::AnalyzerPool::ptr const& analyzer) -> bool {
          EXPECT_EQ(1,
                    expectedSet.erase(ExpectedType(
                        analyzer->name(),
                        arangodb::iresearch::ref<char>(analyzer->properties()),
                        analyzer->features(), analyzer->type())));
          return true;
        },
        nullptr);
    EXPECT_TRUE(result);
    EXPECT_TRUE(expectedSet.empty());
  }
}

TEST_F(IResearchAnalyzerFeatureTest, custom_analyzers_toVelocyPack) {
  // create a new instance of an ApplicationServer and fill it with the required
  // features cannot use the existing server since its features already have
  // some state
  arangodb::ArangodServer newServer(nullptr, nullptr);
  arangodb::iresearch::IResearchAnalyzerFeature feature(newServer);
  auto& dbFeature = newServer.addFeature<arangodb::DatabaseFeature>();
  auto& selector = newServer.addFeature<arangodb::EngineSelectorFeature>();
  StorageEngineMock engine(newServer);
  selector.setEngineTesting(&engine);
  newServer.addFeature<arangodb::metrics::MetricsFeature>();
  newServer.addFeature<arangodb::ClusterFeature>();
  newServer.addFeature<arangodb::QueryRegistryFeature>();
  auto& sysDatabase = newServer.addFeature<arangodb::SystemDatabaseFeature>();
  newServer.addFeature<arangodb::V8DealerFeature>();
  auto cleanup = arangodb::scopeGuard([&dbFeature, this]() noexcept {
    dbFeature.unprepare();
    server.getFeature<arangodb::DatabaseFeature>().prepare();
  });

  // create system vocbase (before feature start)
  {
    auto databases = VPackBuilder();
    databases.openArray();
    databases.add(systemDatabaseArgs);
    databases.close();
    EXPECT_EQ(TRI_ERROR_NO_ERROR, dbFeature.loadDatabases(databases.slice()));
    sysDatabase.start();
    auto vocbase =
        dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
    std::shared_ptr<arangodb::LogicalCollection> unused;
    arangodb::OperationOptions options(arangodb::ExecContext::current());
    arangodb::methods::Collections::createSystem(
        *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
        unused);
    EXPECT_NE(nullptr, sysDatabase.use());
  }

  arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
  auto vpack = VPackParser::fromJson(
      "{\"locale\":\"ru_RU.utf-8\",\"case\":\"upper\",\"accent\":true}");
  EXPECT_TRUE(feature
                  .emplace(result,
                           arangodb::StaticStrings::SystemDatabase +
                               "::test_norm_analyzer4",
                           "norm", vpack->slice())
                  .ok());
  EXPECT_TRUE(result.first);

  EXPECT_EQUAL_SLICES(VPackParser::fromJson(
                          R"({"locale":"ru_RU","case":"upper","accent":true})")
                          ->slice(),
                      result.first->properties());

  // for persistence
  {
    auto expectedVpack = VPackParser::fromJson(
        "{ \"_key\": \"test_norm_analyzer4\", \"name\": "
        "\"test_norm_analyzer4\", \"type\": \"norm\", "
        "\"properties\":{\"locale\":\"ru_RU\",\"case\":\"upper\","
        "\"accent\":true}, "
        "\"features\": [], "
        "\"revision\": 0 } ");

    VPackBuilder builder;
    result.first->toVelocyPack(builder, true);
    EXPECT_EQUAL_SLICES(expectedVpack->slice(), builder.slice());
  }

  // not for persistence
  {
    auto expectedVpack = VPackParser::fromJson(
        "{ \"name\": \"" + arangodb::StaticStrings::SystemDatabase +
        "::test_norm_analyzer4\", "
        "\"type\": \"norm\", "
        "\"properties\":{\"locale\":\"ru_RU\","
        "\"case\":\"upper\",\"accent\":true}, "
        "\"features\": [] } ");

    VPackBuilder builder;
    result.first->toVelocyPack(builder, false);
    EXPECT_EQUAL_SLICES(expectedVpack->slice(), builder.slice());
  }

  // for definition (same database)
  {
    auto expectedVpack = VPackParser::fromJson(
        "{ \"name\": \"test_norm_analyzer4\", "
        "\"type\": \"norm\", "
        "\"properties\":{\"locale\":\"ru_RU\",\"case\":\"upper\","
        "\"accent\":true}, "
        "\"features\": [] } ");

    VPackBuilder builder;
    result.first->toVelocyPack(builder, sysDatabase.use().get());
    EXPECT_EQUAL_SLICES(expectedVpack->slice(), builder.slice());
  }

  // for definition (different database)
  {
    TRI_vocbase_t* vocbase;
    EXPECT_TRUE(
        dbFeature
            .createDatabase(createInfo(server.server(), "vocbase0", 1), vocbase)
            .ok());

    auto expectedVpack = VPackParser::fromJson(
        "{ \"name\": \"::test_norm_analyzer4\", "
        "\"type\": \"norm\", "
        "\"properties\":{\"locale\":\"ru_RU\",\"case\":\"upper\","
        "\"accent\":true}, "
        "\"features\": []} ");

    VPackBuilder builder;
    result.first->toVelocyPack(builder, vocbase);
    EXPECT_EQUAL_SLICES(expectedVpack->slice(), builder.slice());
  }

  // for definition (without database)
  {
    auto expectedVpack = VPackParser::fromJson(
        "{ \"name\": \"" + arangodb::StaticStrings::SystemDatabase +
        "::test_norm_analyzer4\", "
        "\"type\": \"norm\", "
        "\"properties\":{\"locale\":\"ru_RU\","
        "\"case\":\"upper\",\"accent\":true}, "
        "\"features\": []} ");

    VPackBuilder builder;
    result.first->toVelocyPack(builder, nullptr);
    EXPECT_EQUAL_SLICES(expectedVpack->slice(), builder.slice());
  }
}

TEST_F(IResearchAnalyzerFeatureTest, custom_analyzers_vpack_create) {
  // create a new instance of an ApplicationServer and fill it with the required
  // features cannot use the existing server since its features already have
  // some state
  arangodb::ArangodServer newServer(nullptr, nullptr);
  arangodb::iresearch::IResearchAnalyzerFeature feature(newServer);
  auto& dbFeature = newServer.addFeature<arangodb::DatabaseFeature>();
  auto& selector = newServer.addFeature<arangodb::EngineSelectorFeature>();
  StorageEngineMock engine(newServer);
  selector.setEngineTesting(&engine);
  newServer.addFeature<arangodb::metrics::MetricsFeature>();
  newServer.addFeature<arangodb::ClusterFeature>();
  newServer.addFeature<arangodb::QueryRegistryFeature>();
  auto& sysDatabase = newServer.addFeature<arangodb::SystemDatabaseFeature>();
  newServer.addFeature<arangodb::V8DealerFeature>();
  auto cleanup = arangodb::scopeGuard([&dbFeature, this]() noexcept {
    dbFeature.unprepare();
    server.getFeature<arangodb::DatabaseFeature>()
        .prepare();  // restore calculation vocbase
  });

  // create system vocbase (before feature start)
  {
    auto databases = VPackBuilder();
    databases.openArray();
    databases.add(systemDatabaseArgs);
    databases.close();
    EXPECT_EQ(TRI_ERROR_NO_ERROR, dbFeature.loadDatabases(databases.slice()));
    sysDatabase.start();  // get system database from DatabaseFeature
    auto vocbase =
        dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
    std::shared_ptr<arangodb::LogicalCollection> unused;
    arangodb::OperationOptions options(arangodb::ExecContext::current());
    arangodb::methods::Collections::createSystem(
        *vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
        unused);
  }

  // NGRAM ////////////////////////////////////////////////////////////////////
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    // with unknown parameter
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_ngram_analyzer1",
                             "ngram",
                             VPackParser::fromJson(
                                 "{\"min\":1,\"max\":5,\"preserveOriginal\":"
                                 "false,\"invalid_parameter\":true}")
                                 ->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(VPackParser::fromJson(
                            "{\"min\":1,\"max\":5,\"preserveOriginal\":false, "
                            "\"startMarker\":\"\",\"endMarker\":\"\", "
                            "\"streamType\":\"binary\"}")
                            ->slice(),
                        result.first->properties());
  }
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    // with changed parameters
    auto vpack = VPackParser::fromJson(
        "{\"min\":11,\"max\":22,\"preserveOriginal\":true, "
        "\"startMarker\":\"\",\"endMarker\":\"\", \"streamType\":\"binary\"}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_ngram_analyzer2",
                             "ngram", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(vpack->slice(), result.first->properties());
  }
  // DELIMITER ////////////////////////////////////////////////////////////////
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    // with unknown parameter
    EXPECT_TRUE(
        feature
            .emplace(result,
                     arangodb::StaticStrings::SystemDatabase +
                         "::test_delimiter_analyzer1",
                     "delimiter",
                     VPackParser::fromJson(
                         "{\"delimiter\":\",\",\"invalid_parameter\":true}")
                         ->slice())
            .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(VPackParser::fromJson("{\"delimiter\":\",\"}")->slice(),
                        result.first->properties());
  }
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    // with unknown parameter
    auto vpack = VPackParser::fromJson("{\"delimiter\":\"|\"}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_delimiter_analyzer2",
                             "delimiter", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(vpack->slice(), result.first->properties());
  }
  // TEXT /////////////////////////////////////////////////////////////////////
  // with unknown parameter
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson(
        "{\"locale\":\"ru_RU.UTF-8\",\"case\":\"lower\",\"invalid_parameter\":"
        "true,\"stopwords\":[],\"accent\":true,\"stemming\":false}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_text_analyzer1",
                             "text", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(
        VPackParser::fromJson(
            "{ "
            "\"locale\":\"ru_RU\",\"case\":\"lower\",\"stopwords\":[],"
            "\"accent\":true,\"stemming\":false}")
            ->slice(),
        result.first->properties());
  }

  // no case convert in creation. Default value shown
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson(
        "{\"locale\":\"ru_RU.UTF-8\",\"stopwords\":[],\"accent\":true,"
        "\"stemming\":false}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_text_analyzer2",
                             "text", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(
        VPackParser::fromJson(
            "{\"locale\":\"ru_RU\",\"case\":\"lower\",\"stopwords\":[],"
            "\"accent\":true,\"stemming\":false}")
            ->slice(),
        result.first->properties());
  }

  // no accent in creation. Default value shown
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson(
        "{\"locale\":\"ru_RU.UTF-8\",\"case\":\"lower\",\"stopwords\":[],"
        "\"stemming\":false}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_text_analyzer3",
                             "text", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(
        VPackParser::fromJson(
            "{\"locale\":\"ru_RU\",\"case\":\"lower\",\"stopwords\":[],"
            "\"accent\":false,\"stemming\":false}")
            ->slice(),
        result.first->properties());
  }

  // no stem in creation. Default value shown
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson(
        "{\"locale\":\"ru_RU.UTF-8\",\"case\":\"lower\",\"stopwords\":[],"
        "\"accent\":true}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_text_analyzer4",
                             "text", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(
        VPackParser::fromJson(
            "{\"locale\":\"ru_RU\",\"case\":\"lower\",\"stopwords\":[],"
            "\"accent\":true,\"stemming\":true}")
            ->slice(),
        result.first->properties());
  }

  // non default values for stem, accent and case
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson(
        "{\"locale\":\"ru_RU.utf-8\",\"case\":\"upper\",\"stopwords\":[],"
        "\"accent\":true,\"stemming\":false}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_text_analyzer5",
                             "text", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(
        VPackParser::fromJson(
            R"({"locale":"ru_RU","case":"upper","stopwords":[],"accent":true,"stemming":false})")
            ->slice(),
        result.first->properties());
  }

  // non-empty stopwords with duplicates
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson(
        "{\"locale\":\"en_US.utf-8\",\"case\":\"upper\",\"stopwords\":[\"z\","
        "\"a\",\"b\",\"a\"],\"accent\":false,\"stemming\":true}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_text_analyzer6",
                             "text", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);

    // stopwords order is not guaranteed. Need to deep check json
    auto propSlice = result.first->properties();
    ASSERT_TRUE(propSlice.hasKey("stopwords"));
    auto stopwords = propSlice.get("stopwords");
    ASSERT_TRUE(stopwords.isArray());

    std::unordered_set<std::string> expected_stopwords = {"z", "a", "b"};
    for (auto const& it : arangodb::velocypack::ArrayIterator(stopwords)) {
      ASSERT_TRUE(it.isString());
      expected_stopwords.erase(it.copyString());
    }
    ASSERT_TRUE(expected_stopwords.empty());
  }
  // with invalid locale
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson("{\"locale\":\"invalid12345.UTF-8\"}");
    EXPECT_FALSE(feature
                     .emplace(result,
                              arangodb::StaticStrings::SystemDatabase +
                                  "::test_text_analyzer7",
                              "text", vpack->slice())
                     .ok());
  }
  // STEM /////////////////////////////////////////////////////////////////////
  // with unknown parameter
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson(
        "{\"locale\":\"ru_RU.UTF-8\",\"invalid_parameter\":true}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_stem_analyzer1",
                             "stem", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(VPackParser::fromJson("{\"locale\":\"ru\"}")->slice(),
                        result.first->properties());
  }
  // with invalid locale
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson("{\"locale\":\"invalid12345.UTF-8\"}");
    EXPECT_FALSE(feature
                     .emplace(result,
                              arangodb::StaticStrings::SystemDatabase +
                                  "::test_stem_analyzer2",
                              "stem", vpack->slice())
                     .ok());
  }
  // NORM /////////////////////////////////////////////////////////////////////
  // with unknown parameter
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson(
        "{\"locale\":\"ru_RU.UTF-8\",\"case\":\"lower\",\"invalid_parameter\":"
        "true,\"accent\":true}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_norm_analyzer1",
                             "norm", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(
        VPackParser::fromJson(
            "{\"locale\":\"ru_RU\",\"case\":\"lower\",\"accent\":true}")
            ->slice(),
        result.first->properties());
  }

  // no case convert in creation. Default value shown
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack =
        VPackParser::fromJson("{\"locale\":\"ru_RU.UTF-8\",\"accent\":true}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_norm_analyzer2",
                             "norm", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(
        VPackParser::fromJson(
            "{\"locale\":\"ru_RU\",\"case\":\"none\",\"accent\":true}")
            ->slice(),
        result.first->properties());
  }

  // no accent in creation. Default value shown
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson(
        "{\"locale\":\"ru_RU.UTF-8\",\"case\":\"lower\"}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_norm_analyzer3",
                             "norm", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);
    EXPECT_EQUAL_SLICES(
        VPackParser::fromJson(
            "{\"locale\":\"ru_RU\",\"case\":\"lower\",\"accent\":true}")
            ->slice(),
        result.first->properties());
  }
  // non default values for accent and case
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson(
        "{\"locale\":\"ru_RU.utf-8\",\"case\":\"upper\",\"accent\":true}");
    EXPECT_TRUE(feature
                    .emplace(result,
                             arangodb::StaticStrings::SystemDatabase +
                                 "::test_norm_analyzer4",
                             "norm", vpack->slice())
                    .ok());
    EXPECT_TRUE(result.first);

    EXPECT_EQUAL_SLICES(
        VPackParser::fromJson(
            R"({"locale":"ru_RU","case":"upper","accent":true})")
            ->slice(),
        result.first->properties());
  }
  // with invalid locale
  {
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    auto vpack = VPackParser::fromJson("{\"locale\":\"invalid12345.UTF-8\"}");
    EXPECT_FALSE(feature
                     .emplace(result,
                              arangodb::StaticStrings::SystemDatabase +
                                  "::test_norm_analyzer5",
                              "norm", vpack->slice())
                     .ok());
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                     Features test
// -----------------------------------------------------------------------------

TEST(FeaturesTest, construct) {
  arangodb::iresearch::Features f;
  ASSERT_TRUE(f.validate().ok());
  ASSERT_EQ(irs::IndexFeatures::NONE, f.indexFeatures());
  ASSERT_TRUE(f.fieldFeatures(arangodb::iresearch::LinkVersion::MIN).empty());
  ASSERT_TRUE(f.fieldFeatures(arangodb::iresearch::LinkVersion::MAX).empty());
  ASSERT_EQ(arangodb::iresearch::Features{}, f);
  ASSERT_FALSE(arangodb::iresearch::Features{} != f);
}

TEST(FeaturesTest, add_invalid) {
  arangodb::iresearch::Features f;
  ASSERT_TRUE(f.validate().ok());

  {
    ASSERT_TRUE(f.add(irs::type<irs::position>::name()));
    ASSERT_EQ(irs::IndexFeatures::POS, f.indexFeatures());
    ASSERT_TRUE(f.fieldFeatures(arangodb::iresearch::LinkVersion::MIN).empty());
    ASSERT_TRUE(f.fieldFeatures(arangodb::iresearch::LinkVersion::MAX).empty());
    ASSERT_TRUE(f.validate().fail());
  }
}

TEST(FeaturesTest, add_validate) {
  arangodb::iresearch::Features f;
  ASSERT_TRUE(f.validate().ok());

  {
    ASSERT_FALSE(f.add("invalidFeature"));
    ASSERT_EQ(irs::IndexFeatures::NONE, f.indexFeatures());
    ASSERT_TRUE(f.fieldFeatures(arangodb::iresearch::LinkVersion::MIN).empty());
    ASSERT_TRUE(f.fieldFeatures(arangodb::iresearch::LinkVersion::MAX).empty());
    ASSERT_TRUE(f.validate().ok());
  }

  {
    ASSERT_TRUE(f.add(irs::type<irs::frequency>::name()));
    ASSERT_EQ(irs::IndexFeatures::FREQ, f.indexFeatures());
    ASSERT_TRUE(f.fieldFeatures(arangodb::iresearch::LinkVersion::MIN).empty());
    ASSERT_TRUE(f.fieldFeatures(arangodb::iresearch::LinkVersion::MAX).empty());
    ASSERT_TRUE(f.validate().ok());
  }

  {
    ASSERT_TRUE(f.add(irs::type<irs::frequency>::name()));
    ASSERT_EQ(irs::IndexFeatures::FREQ, f.indexFeatures());
    ASSERT_TRUE(f.fieldFeatures(arangodb::iresearch::LinkVersion::MIN).empty());
    ASSERT_TRUE(f.fieldFeatures(arangodb::iresearch::LinkVersion::MAX).empty());
    ASSERT_TRUE(f.validate().ok());
  }

  {
    ASSERT_TRUE(f.add(irs::type<irs::Norm>::name()));
    ASSERT_EQ(irs::IndexFeatures::FREQ, f.indexFeatures());
    ASSERT_EQ(std::vector<irs::type_info::type_id>{irs::type<irs::Norm>::id()},
              f.fieldFeatures(arangodb::iresearch::LinkVersion::MIN));
    ASSERT_EQ(std::vector<irs::type_info::type_id>{irs::type<irs::Norm2>::id()},
              f.fieldFeatures(arangodb::iresearch::LinkVersion::MAX));
    ASSERT_TRUE(f.validate().ok());
  }

  {
    ASSERT_TRUE(f.add(irs::type<irs::Norm>::name()));
    ASSERT_EQ(irs::IndexFeatures::FREQ, f.indexFeatures());
    ASSERT_EQ(std::vector<irs::type_info::type_id>{irs::type<irs::Norm>::id()},
              f.fieldFeatures(arangodb::iresearch::LinkVersion::MIN));
    ASSERT_EQ(std::vector<irs::type_info::type_id>{irs::type<irs::Norm2>::id()},
              f.fieldFeatures(arangodb::iresearch::LinkVersion::MAX));
    ASSERT_TRUE(f.validate().ok());
  }

  {
    ASSERT_TRUE(f.add(irs::type<irs::position>::name()));
    ASSERT_EQ(irs::IndexFeatures::FREQ | irs::IndexFeatures::POS,
              f.indexFeatures());
    ASSERT_EQ(std::vector<irs::type_info::type_id>{irs::type<irs::Norm>::id()},
              f.fieldFeatures(arangodb::iresearch::LinkVersion::MIN));
    ASSERT_EQ(std::vector<irs::type_info::type_id>{irs::type<irs::Norm2>::id()},
              f.fieldFeatures(arangodb::iresearch::LinkVersion::MAX));
    ASSERT_TRUE(f.validate().ok());
  }

  {
    ASSERT_FALSE(f.add(irs::type<irs::Norm2>::name()));
    ASSERT_EQ(irs::IndexFeatures::FREQ | irs::IndexFeatures::POS,
              f.indexFeatures());
    ASSERT_EQ(std::vector<irs::type_info::type_id>{irs::type<irs::Norm>::id()},
              f.fieldFeatures(arangodb::iresearch::LinkVersion::MIN));
    ASSERT_EQ(std::vector<irs::type_info::type_id>{irs::type<irs::Norm2>::id()},
              f.fieldFeatures(arangodb::iresearch::LinkVersion::MAX));
    ASSERT_TRUE(f.validate().ok());
  }
}
