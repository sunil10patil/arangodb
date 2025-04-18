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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include "Aql/RowFetcherHelper.h"
#include "Mocks/LogLevels.h"
#include "Mocks/Servers.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/AqlItemBlockHelper.h"
#include "Aql/AqlItemBlockManager.h"
#include "Aql/AqlValue.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/Query.h"
#include "Aql/RegisterInfos.h"
#include "Aql/ShortestPathExecutor.h"
#include "Aql/Stats.h"
#include "Basics/GlobalResourceMonitor.h"
#include "Basics/ResourceUsage.h"
#include "Graph/EdgeDocumentToken.h"
#include "Graph/ShortestPathOptions.h"
#include "Graph/TraverserCache.h"
#include "Graph/TraverserOptions.h"
#include "Graph/PathManagement/PathResult.h"

#include "../Mocks/MockGraphProvider.h"
#include "../Mocks/Servers.h"

#include <string_view>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::graph;
using namespace arangodb::tests::mocks;

namespace arangodb {
namespace tests {
namespace aql {

class TokenTranslator : public TraverserCache {
 public:
  TokenTranslator(Query* query, BaseOptions* opts)
      : TraverserCache(*query, opts),
        _edges(11, arangodb::basics::VelocyPackHelper::VPackHash(),
               arangodb::basics::VelocyPackHelper::VPackEqual()) {}
  ~TokenTranslator() = default;

  std::string_view makeVertex(std::string const& id) {
    VPackBuilder vertex;
    vertex.openObject();
    vertex.add(StaticStrings::IdString, VPackValue(id));
    vertex.add(
        StaticStrings::KeyString,
        VPackValue(
            id));  // This is not corect but nevermind we fake it anyways.
    vertex.add(StaticStrings::RevString,
               VPackValue("123"));  // just to have it there
    vertex.close();
    auto vslice = vertex.slice();
    std::string_view ref(vslice.get(StaticStrings::IdString).stringView());
    _dataLake.emplace_back(vertex.steal());
    _vertices.emplace(ref, vslice);
    return ref;
  }

  EdgeDocumentToken makeEdge(std::string const& s, std::string const& t) {
    VPackBuilder edge;
    std::string fromVal = s;
    std::string toVal = t;
    edge.openObject();
    edge.add(StaticStrings::RevString,
             VPackValue("123"));  // just to have it there
    edge.add(StaticStrings::FromString, VPackValue(fromVal));
    edge.add(StaticStrings::ToString, VPackValue(toVal));
    edge.close();
    auto eslice = edge.slice();
    _dataLake.emplace_back(edge.steal());
    _edges.emplace(eslice);
    return EdgeDocumentToken{eslice};
  }

  VPackSlice translateVertex(std::string_view idString) {
    auto it = _vertices.find(idString);
    TRI_ASSERT(it != _vertices.end());
    return it->second;
  }

  std::string_view translateVertexID(std::string_view idString) {
    auto it = _vertices.find(idString);
    TRI_ASSERT(it != _vertices.end());
    return it->second.get(StaticStrings::IdString).stringView();
  }

  bool appendVertex(std::string_view idString, VPackBuilder& builder) override {
    builder.add(translateVertex(idString));
    return true;
  }

  bool appendVertex(std::string_view idString, AqlValue& result) override {
    result = AqlValue(translateVertex(idString));
    return true;
  }

  AqlValue fetchEdgeAqlResult(EdgeDocumentToken const& edgeTkn) override {
    auto it = _edges.find(VPackSlice(edgeTkn.vpack()));
    TRI_ASSERT(it != _edges.end());
    return AqlValue{*it};
  }

 private:
  std::vector<std::shared_ptr<VPackBuffer<uint8_t>>> _dataLake;
  std::unordered_map<std::string_view, VPackSlice> _vertices;
  std::unordered_set<VPackSlice, arangodb::basics::VelocyPackHelper::VPackHash,
                     arangodb::basics::VelocyPackHelper::VPackEqual>
      _edges;
};

// FakePathFinder only stores a lump of pairs (source and targets) by which
// sequences of outputs can be found. It also stores which paths it has been
// asked for to verify later whether the outputs produced by the
// ShortestPathExecutor are the ones we expected.
class FakePathFinder {
 public:
  using VertexRef = arangodb::velocypack::HashedStringRef;

  FakePathFinder(ShortestPathOptions& opts, TokenTranslator& translator)
      : _paths(), _translator(translator) {}

  ~FakePathFinder() = default;

  void clear(){};

  aql::TraversalStats stealStats() { return {}; }

  bool isDone() { return true; }

  void reset(VertexRef source, VertexRef target) {
    _tmpPathBuilder.clear();
    _source = source;
    _target = target;
  }

  bool getNextPath(VPackBuilder& result) {
    VPackBuilder s;
    s.openArray();
    s.add(VPackValue(_source.toString()));
    s.add(VPackValue(_target.toString()));
    s.close();

    VPackSlice sourceSlice = s.slice().at(0);
    VPackSlice targetSlice = s.slice().at(1);

    bool foundPath = shortestPath(sourceSlice, targetSlice);
    result = _tmpPathBuilder;

    return foundPath;
  }

  void addPath(std::vector<std::string>&& path) {
    _paths.emplace_back(std::move(path));
  }

  bool shortestPath(VPackSlice source, VPackSlice target) {
    TRI_ASSERT(source.isString());
    TRI_ASSERT(target.isString());
    _calledWith.emplace_back(
        std::make_pair(source.copyString(), target.copyString()));

    std::string const s = source.copyString();
    std::string const t = target.copyString();

    for (auto const& p : _paths) {
      if (p.front() == s && p.back() == t) {
        // Found a path

        // new refactored output
        _tmpPathBuilder.clear();
        _tmpPathBuilder.openObject();
        _tmpPathBuilder.add(VPackValue(StaticStrings::GraphQueryVertices));
        _tmpPathBuilder.openArray();
        for (size_t i = 0; i < p.size() - 1; ++i) {
          _tmpPathBuilder.add(VPackValue(_translator.makeVertex(p[i])));
        }
        _tmpPathBuilder.add(VPackValue(_translator.makeVertex(p.back())));
        _tmpPathBuilder.close();  // vertices

        _tmpPathBuilder.add(VPackValue(StaticStrings::GraphQueryEdges));
        _tmpPathBuilder.openArray();
        for (size_t i = 0; i < p.size() - 1; ++i) {
          _tmpPathBuilder.add(
              VPackValue(_translator.makeEdge(p[i], p[i + 1]).vpack()));
        }
        _tmpPathBuilder.close();  // edges

        _tmpPathBuilder.close();  // outer object

        return true;
      }
    }
    return false;
  }

  std::vector<std::string> const& findPath(
      std::pair<std::string, std::string> const& src) {
    for (auto const& p : _paths) {
      if (p.front() == src.first && p.back() == src.second) {
        return p;
      }
    }
    return _theEmptyPath;
  }

  [[nodiscard]] auto getCalledWith()
      -> std::vector<std::pair<std::string, std::string>> const& {
    return _calledWith;
  };

  // Needs to provide lookupFunctionality for Cache
 private:
  std::vector<std::vector<std::string>> _paths;
  std::vector<std::pair<std::string, std::string>> _calledWith;
  std::vector<std::string> const _theEmptyPath{};
  TokenTranslator& _translator;

  VertexRef _source;
  VertexRef _target;
  VPackBuilder _tmpPathBuilder;
};

struct TestShortestPathOptions : public ShortestPathOptions {
  TestShortestPathOptions(Query* query) : ShortestPathOptions(*query) {
    std::unique_ptr<TraverserCache> cache =
        std::make_unique<TokenTranslator>(query, this);
  }
};

using Vertex = GraphNode::InputVertex;
using Path = std::vector<std::string>;
using PathSequence = std::vector<Path>;

enum class ShortestPathOutput { VERTEX_ONLY, VERTEX_AND_EDGE };

// Namespace conflict with the other shortest path executor
namespace {
Vertex const constSource("vertex/source"), constTarget("vertex/target"),
    regSource(RegisterId(0)),
    regTarget(RegisterId(1)), brokenSource{"IwillBreakYourSearch"},
    brokenTarget{"I will also break your search"};
MatrixBuilder<2> const noneRow{{{{}}}};
MatrixBuilder<2> const oneRow{
    {{{R"("vertex/source")"}, {R"("vertex/target")"}}}};
MatrixBuilder<2> const twoRows{
    {{{R"("vertex/source")"}, {R"("vertex/target")"}}},
    {{{R"("vertex/a")"}, {R"("vertex/b")"}}}};
MatrixBuilder<2> const threeRows{
    {{{R"("vertex/source")"}, {R"("vertex/target")"}}},
    {{{R"("vertex/a")"}, {R"("vertex/b")"}}},
    {{{R"("vertex/a")"}, {R"("vertex/target")"}}}};
MatrixBuilder<2> const someRows{{{{R"("vertex/c")"}, {R"("vertex/target")"}}},
                                {{{R"("vertex/b")"}, {R"("vertex/target")"}}},
                                {{{R"("vertex/e")"}, {R"("vertex/target")"}}},
                                {{{R"("vertex/a")"}, {R"("vertex/target")"}}}};

auto pathBetween(std::string const& start, std::string const& end, size_t n)
    -> Path {
  auto path = std::vector<std::string>{};
  path.reserve(2 + n);
  path.push_back(start);
  for (size_t i = 0; i < n; ++i) {
    path.emplace_back(std::to_string(i));
  }
  path.push_back(end);
  return {path};
}

PathSequence const noPath = {};
PathSequence const onePath = {
    pathBetween("vertex/source", "vertex/target", 10)};
PathSequence const threePaths = {
    pathBetween("vertex/source", "vertex/target", 10),
    pathBetween("vertex/source", "vertex/b", 100),
    pathBetween("vertex/a", "vertex/b", 1000)};
PathSequence const somePaths = {
    pathBetween("vertex/source", "vertex/target", 10),
    pathBetween("vertex/source", "vertex/b", 100),
    pathBetween("vertex/a", "vertex/b", 1000),
    pathBetween("vertex/c", "vertex/d", 2001)};
PathSequence const someOtherPaths = {
    pathBetween("vertex/a", "vertex/target", 10),
    pathBetween("vertex/b", "vertex/target", 999),
    pathBetween("vertex/c", "vertex/target", 1001),
    pathBetween("vertex/d", "vertex/target", 2000),
    pathBetween("vertex/e", "vertex/target", 200),
    pathBetween("vertex/f", "vertex/target", 15),
    pathBetween("vertex/g", "vertex/target", 10)};

auto sources = testing::Values(constSource, regSource, brokenSource);
auto targets = testing::Values(constTarget, regTarget, brokenTarget);
static auto inputs =
    testing::Values(noneRow, oneRow, twoRows, threeRows, someRows);
auto paths = testing::Values(noPath, onePath, threePaths, somePaths);
auto calls = testing::Values(
    AqlCall{}, AqlCall{0, 0u, 0u, false}, AqlCall{0, 1u, 0u, false},
    AqlCall{0, 0u, 1u, false}, AqlCall{0, 1u, 1u, false}, AqlCall{1, 1u, 1u},
    AqlCall{100, 1u, 1u}, AqlCall{1000}, AqlCall{0, 0u, 0u, true},
    AqlCall{0, AqlCall::Infinity{}, AqlCall::Infinity{}, true});

auto variants = testing::Values(ShortestPathOutput::VERTEX_ONLY,
                                ShortestPathOutput::VERTEX_AND_EDGE);
auto blockSizes = testing::Values(size_t{5}, 1000);
}  // namespace

// TODO: this needs a << operator
struct ShortestPathTestParameters {
  static RegIdSet _makeOutputRegisters(ShortestPathOutput in) {
    switch (in) {
      case ShortestPathOutput::VERTEX_ONLY:
        return RegIdSet{std::initializer_list<RegisterId>{2}};
      case ShortestPathOutput::VERTEX_AND_EDGE:
        return RegIdSet{std::initializer_list<RegisterId>{2, 3}};
    }
    return RegIdSet{};
  }
  static ShortestPathExecutorInfos<FakePathFinder>::RegisterMapping
  _makeRegisterMapping(ShortestPathOutput in) {
    switch (in) {
      case ShortestPathOutput::VERTEX_ONLY:
        return ShortestPathExecutorInfos<FakePathFinder>::RegisterMapping{
            {ShortestPathExecutorInfos<FakePathFinder>::OutputName::VERTEX, 2}};
        break;
      case ShortestPathOutput::VERTEX_AND_EDGE:
        return ShortestPathExecutorInfos<FakePathFinder>::RegisterMapping{
            {ShortestPathExecutorInfos<FakePathFinder>::OutputName::VERTEX, 2},
            {ShortestPathExecutorInfos<FakePathFinder>::OutputName::EDGE, 3}};
    }
    return ShortestPathExecutorInfos<FakePathFinder>::RegisterMapping{};
  }

  ShortestPathTestParameters(
      std::tuple<Vertex, Vertex, ShortestPathOutput> params)
      : _source(std::get<0>(params)),
        _target(std::get<1>(params)),
        _outputRegisters(_makeOutputRegisters(std::get<2>(params))),
        _registerMapping(_makeRegisterMapping(std::get<2>(params))),
        _inputMatrix{oneRow},
        _inputMatrixCopy{oneRow},
        _paths(threePaths),
        _call(AqlCall{0, AqlCall::Infinity{}, AqlCall::Infinity{}, true}),
        _blockSize(1000) {}

  ShortestPathTestParameters(
      std::tuple<MatrixBuilder<2>, PathSequence, AqlCall, size_t> params)
      : _source(constSource),
        _target(constTarget),
        _outputRegisters(_makeOutputRegisters(ShortestPathOutput::VERTEX_ONLY)),
        _registerMapping(_makeRegisterMapping(ShortestPathOutput::VERTEX_ONLY)),
        _inputMatrix{std::get<0>(params)},
        _inputMatrixCopy{std::get<0>(params)},
        _paths(std::get<1>(params)),
        _call(std::get<2>(params)),
        _blockSize(std::get<3>(params)) {}

  Vertex _source;
  Vertex _target;
  RegIdSet _inputRegisters;
  RegIdSet _outputRegisters;
  ShortestPathExecutorInfos<FakePathFinder>::RegisterMapping _registerMapping;
  MatrixBuilder<2> _inputMatrix;
  MatrixBuilder<2> _inputMatrixCopy;
  PathSequence _paths;
  AqlCall _call;
  size_t _blockSize{1000};
};

class ShortestPathExecutorTest : public ::testing::Test {
 protected:
  ShortestPathTestParameters parameters;

  MockAqlServer server;
  ExecutionState state;
  arangodb::GlobalResourceMonitor global{};
  arangodb::ResourceMonitor monitor{global};
  AqlItemBlockManager itemBlockManager;
  std::shared_ptr<arangodb::aql::Query> fakedQuery;
  TestShortestPathOptions options;
  ShortestPathOptions defaultOptions;
  std::unique_ptr<BaseOptions> dummy;
  std::unique_ptr<TraverserCache> cache;
  TokenTranslator translator;

  RegisterInfos registerInfos;
  // parameters are copied because they are const otherwise
  // and that doesn't mix with std::move
  ShortestPathExecutorInfos<FakePathFinder> executorInfos;

  FakePathFinder& finder;

  SharedAqlItemBlockPtr inputBlock;
  AqlItemBlockInputRange input;

  std::shared_ptr<arangodb::velocypack::Builder> fakeUnusedBlock;
  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher;

  ShortestPathExecutor<FakePathFinder> testee;

  ShortestPathExecutorTest(ShortestPathTestParameters parameters_)
      : parameters(std::move(parameters_)),
        server{},
        itemBlockManager(monitor),
        fakedQuery(server.createFakeQuery()),
        options(fakedQuery.get()),
        defaultOptions(*fakedQuery.get()),
        dummy(std::make_unique<ShortestPathOptions>(defaultOptions)),
        cache(std::make_unique<TraverserCache>(
            *fakedQuery.get(),
            dynamic_cast<ShortestPathOptions*>(&defaultOptions))),
        translator(fakedQuery.get(), dummy.get()),
        registerInfos(parameters._inputRegisters, parameters._outputRegisters,
                      2, 4, {}, {RegIdSet{0, 1}}),
        executorInfos(*fakedQuery.get(),
                      std::make_unique<FakePathFinder>(options, translator),
                      std::move(parameters._registerMapping),
                      std::move(parameters._source),
                      std::move(parameters._target)),
        finder(static_cast<FakePathFinder&>(executorInfos.finder())),
        inputBlock(buildBlock<2>(itemBlockManager,
                                 std::move(parameters._inputMatrix))),
        input(AqlItemBlockInputRange(MainQueryState::DONE, 0, inputBlock, 0)),
        fakeUnusedBlock(VPackParser::fromJson("[]")),
        fetcher(itemBlockManager, fakeUnusedBlock->steal(), false),
        testee(fetcher, executorInfos) {
    for (auto&& p : parameters._paths) {
      finder.addPath(std::move(p));
    }
  }

  size_t ExpectedNumberOfRowsProduced(size_t expectedFound) {
    if (parameters._call.getOffset() >= expectedFound) {
      return 0;
    } else {
      expectedFound -= parameters._call.getOffset();
    }
    return parameters._call.clampToLimit(expectedFound);
  }

  // We only verify that the shortest path executor was called with
  // correct inputs
  void ValidateCalledWith() {
    auto const& pathsQueriedBetween = finder.getCalledWith();
    auto block =
        buildBlock<2>(itemBlockManager, std::move(parameters._inputMatrix));

    // We should always only call the finder at most for all input rows
    ASSERT_LE(pathsQueriedBetween.size(), block->numRows());

    auto source = std::string{};
    auto target = std::string{};
    auto blockIndex = size_t{0};
    for (auto const& input : pathsQueriedBetween) {
      if (executorInfos.useRegisterForSourceInput()) {
        AqlValue value =
            block->getValue(blockIndex, executorInfos.getSourceInputRegister());
        ASSERT_TRUE(value.isString());
        ASSERT_TRUE(value.slice().isEqualString(input.first));
      } else {
        ASSERT_EQ(executorInfos.getSourceInputValue(), input.first);
      }

      if (executorInfos.useRegisterForTargetInput()) {
        AqlValue value =
            block->getValue(blockIndex, executorInfos.getTargetInputRegister());
        ASSERT_TRUE(value.isString());
        ASSERT_TRUE(value.slice().isEqualString(input.second));
      } else {
        ASSERT_EQ(executorInfos.getTargetInputValue(), input.second);
      }
      blockIndex++;
    }
  }

  // TODO: check fullcount correctness.
  void ValidateResult(std::vector<SharedAqlItemBlockPtr>& results,
                      size_t skippedInitial, size_t skippedFullCount) {
    auto const& pathsQueriedBetween = finder.getCalledWith();

    FakePathFinder& finder =
        static_cast<FakePathFinder&>(executorInfos.finder());

    auto expectedRowsFound = std::vector<std::string>{};
    auto expectedPathStarts = std::set<size_t>{};
    for (auto const& p : pathsQueriedBetween) {
      auto& f = finder.findPath(p);
      expectedPathStarts.insert(expectedRowsFound.size());
      expectedRowsFound.insert(expectedRowsFound.end(), f.begin(), f.end());
    }

    auto expectedNrRowsSkippedInitial =
        std::min(parameters._call.getOffset(), expectedRowsFound.size());
    EXPECT_EQ(skippedInitial, expectedNrRowsSkippedInitial);

    // TODO: Really we're relying on the fact here that the executor
    //       calls the path finder with the correct inputs, where we should
    //       assert/compute the paths that could be produced if the
    //       finder is called with the input parameters given in the test
    auto expectedNrRowsProduced =
        ExpectedNumberOfRowsProduced(expectedRowsFound.size());

    auto expectedRowsIndex = size_t{skippedInitial};
    for (auto const& block : results) {
      if (block != nullptr) {
        ASSERT_NE(block, nullptr);
        for (size_t blockIndex = 0; blockIndex < block->numRows();
             ++blockIndex, ++expectedRowsIndex) {
          if (executorInfos.usesOutputRegister(
                  ShortestPathExecutorInfos<
                      FakePathFinder>::OutputName::VERTEX)) {
            AqlValue value = block->getValue(
                blockIndex, executorInfos.getOutputRegister(
                                ShortestPathExecutorInfos<
                                    FakePathFinder>::OutputName::VERTEX));
            EXPECT_TRUE(value.slice().stringView() ==
                        translator.translateVertexID(std::string_view(
                            expectedRowsFound[expectedRowsIndex])));
          }
          if (executorInfos.usesOutputRegister(
                  ShortestPathExecutorInfos<
                      FakePathFinder>::OutputName::EDGE)) {
            AqlValue value = block->getValue(
                blockIndex, executorInfos.getOutputRegister(
                                ShortestPathExecutorInfos<
                                    FakePathFinder>::OutputName::EDGE));

            if (expectedPathStarts.find(expectedRowsIndex) !=
                expectedPathStarts.end()) {
              EXPECT_TRUE(value.isNull(false));
            } else {
              EXPECT_TRUE(value.isObject());
              VPackSlice edge = value.slice();
              // FROM and TO checks are enough here.
              EXPECT_TRUE(
                  edge.get(StaticStrings::FromString)
                      .stringView()
                      .compare(expectedRowsFound[expectedRowsIndex - 1]) == 0);
              EXPECT_TRUE(edge.get(StaticStrings::ToString)
                              .stringView()
                              .compare(expectedRowsFound[expectedRowsIndex]) ==
                          0);
            }
          }
        }
      }
    }
    ASSERT_EQ(expectedRowsIndex - skippedInitial, expectedNrRowsProduced);

    // If a fullCount was requested, the sum (skippedInitial + produced +
    // skippedFullCount) should be exactly the number of rows we produced.
    if (parameters._call.fullCount) {
      ASSERT_EQ(skippedInitial + (expectedRowsIndex - skippedInitial) +
                    skippedFullCount,
                expectedRowsFound.size());
    }
  }

  void TestExecutor() {
    // We use a copy here because we modify the call and want to keep track
    // of whether things happen the correct way.
    auto ourCall = AqlCall{parameters._call};
    auto skippedInitial = size_t{0};
    auto skippedFullCount = size_t{0};
    auto state = ExecutorState{ExecutorState::HASMORE};
    auto outputs = std::vector<SharedAqlItemBlockPtr>{};

    // TODO: Do we have to emulate pauses because
    //       upstream needs to produce more?
    //       that would require breaking up the input
    //       matrix into chunks and feeding those into
    //       the executor.

    // If an offset is requested, skip
    if (ourCall.getOffset() > 0) {
      std::tie(state, std::ignore /* stats */, skippedInitial, std::ignore) =
          testee.skipRowsRange(input, ourCall);
    }
    ourCall.resetSkipCount();

    // Produce rows
    while (state == ExecutorState::HASMORE && ourCall.getLimit() > 0) {
      SharedAqlItemBlockPtr block =
          itemBlockManager.requestBlock(parameters._blockSize, 4);

      OutputAqlItemRow output(
          std::move(block), registerInfos.getOutputRegisters(),
          registerInfos.registersToKeep(), registerInfos.registersToClear());
      output.setCall(std::move(ourCall));

      std::tie(state, std::ignore, std::ignore) =
          testee.produceRows(input, output);

      outputs.emplace_back(output.stealBlock());
      ourCall = output.stealClientCall();
    }

    // FullCount
    if (ourCall.needsFullCount()) {
      // Emulate being called with a full count
      ourCall.hardLimit = 0u;
      ourCall.softLimit = 0u;
      std::tie(state, std::ignore /* stats */, skippedFullCount, std::ignore) =
          testee.skipRowsRange(input, ourCall);
    }
    ourCall.resetSkipCount();

    ValidateCalledWith();
    ValidateResult(outputs, skippedInitial, skippedFullCount);
  }
};  // namespace aql

/*
 * We currently only have one test, but it's heavily parameterised.
 * We emulate the call sequence of ExecutionBlockImpl, so, we skip, produce, and
 * fullcount (depending on what the AqlCall parameter prescribes).
 *
 * the test with all combinations of parameters defined below, and compare the
 * produced output of the executor with the expected output (which in turn is
 * computed from the parameters).
 *
 * The parameters are
 *  - sources:    constant or register source (then drawn from input)
 *  - targets:    constant or register source (then drawn from input)
 *  - inputs:     a matrix of input rows
 *  - paths:      paths present in the fakePathFinder
 *  - calls:      AqlCalls giving the offset, limits, and fullCount
 *  - variants:   whether to output vertices only or vertices and edges
 *  - blockSizes: which outputBlock sizes to test with
 *
 * We never actually perform a shortest path search: testing this is the
 * responsibility of the test for the shortest path finder.
 */

class ShortestPathExecutorInputOutputTest
    : public ShortestPathExecutorTest,
      public ::testing::WithParamInterface<
          std::tuple<Vertex, Vertex, ShortestPathOutput>> {
 protected:
  ShortestPathExecutorInputOutputTest()
      : ShortestPathExecutorTest(GetParam()) {}
};

class ShortestPathExecutorPathTest
    : public ShortestPathExecutorTest,
      public ::testing::WithParamInterface<
          std::tuple<MatrixBuilder<2>, PathSequence, AqlCall, size_t>> {
 protected:
  ShortestPathExecutorPathTest() : ShortestPathExecutorTest(GetParam()) {}
};

TEST_P(ShortestPathExecutorInputOutputTest, the_test) { TestExecutor(); }

TEST_P(ShortestPathExecutorPathTest, the_test) { TestExecutor(); }

// Namespace conflict with the other shortest path executor
namespace {

INSTANTIATE_TEST_CASE_P(ShortestPathExecutorInputOutputTestInstance,
                        ShortestPathExecutorInputOutputTest,
                        testing::Combine(sources, targets, variants));

INSTANTIATE_TEST_CASE_P(ShortestPathExecutorPathsTestInstance,
                        ShortestPathExecutorPathTest,
                        testing::Combine(inputs, paths, calls, blockSizes));
}  // namespace
}  // namespace aql
}  // namespace tests
}  // namespace arangodb

#include "Aql/ShortestPathExecutor.tpp"

template class ::arangodb::aql::ShortestPathExecutorInfos<
    ::arangodb::tests::aql::FakePathFinder>;

template class ::arangodb::aql::ShortestPathExecutor<
    ::arangodb::tests::aql::FakePathFinder>;
