////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "VocbaseInfo.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/FeatureFlags.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Logger/LogMacros.h"
#include "Replication2/Version.h"
#include "RestServer/DatabaseFeature.h"
#include "Utils/Events.h"
#include "Utilities/NameValidator.h"
#include "VocBase/Methods/Databases.h"

#include <absl/strings/str_cat.h>

namespace arangodb {

CreateDatabaseInfo::CreateDatabaseInfo(ArangodServer& server,
                                       ExecContext const& context)
    : _server(server), _context(context) {}

ShardingPrototype CreateDatabaseInfo::shardingPrototype() const {
  if (_name != StaticStrings::SystemDatabase) {
    return ShardingPrototype::Graphs;
  }
  return _shardingPrototype;
}

uint64_t CreateDatabaseInfo::getId() const {
  TRI_ASSERT(_valid);
  TRI_ASSERT(_validId || !_strictValidation);
  return _id;
}

void CreateDatabaseInfo::shardingPrototype(ShardingPrototype type) {
  _shardingPrototype = type;
}

void CreateDatabaseInfo::setSharding(std::string_view sharding) {
  // sharding -- must be "", "flexible" or "single"
  bool isValidProperty =
      (sharding.empty() || sharding == "flexible" || sharding == "single");
  TRI_ASSERT(isValidProperty);
  if (isValidProperty) {
    _sharding = sharding;
  }
}

Result CreateDatabaseInfo::load(std::string_view name, uint64_t id) {
  _name = name;
  _id = id;

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  _valid = true;
#endif
  return checkOptions();
}

Result CreateDatabaseInfo::load(VPackSlice options, VPackSlice users) {
  Result res = extractOptions(options, true /*getId*/, true /*getUser*/);
  if (res.ok()) {
    res = extractUsers(users);
  }
  if (!res.ok()) {
    return res;
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  _valid = true;
#endif

  return checkOptions();
}

Result CreateDatabaseInfo::load(std::string_view name, VPackSlice options,
                                VPackSlice users) {
  _name = name;

  Result res = extractOptions(options, true /*getId*/, false /*getName*/);
  if (res.ok()) {
    res = extractUsers(users);
  }
  if (!res.ok()) {
    return res;
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  _valid = true;
#endif

  return checkOptions();
}

Result CreateDatabaseInfo::load(std::string_view name, uint64_t id,
                                VPackSlice options, VPackSlice users) {
  _name = name;
  _id = id;

  Result res = extractOptions(options, false /*getId*/, false /*getUser*/);
  if (res.ok()) {
    res = extractUsers(users);
  }
  if (!res.ok()) {
    return res;
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  _valid = true;
#endif

  return checkOptions();
}

void CreateDatabaseInfo::toVelocyPack(VPackBuilder& builder,
                                      bool withUsers) const {
  TRI_ASSERT(_validId);
  TRI_ASSERT(builder.isOpenObject());
  std::string const idString(basics::StringUtils::itoa(_id));
  builder.add(StaticStrings::DatabaseId, VPackValue(idString));
  builder.add(StaticStrings::DatabaseName, VPackValue(_name));
  builder.add(StaticStrings::DataSourceSystem,
              VPackValue(_name == StaticStrings::SystemDatabase));

  if (ServerState::instance()->isCoordinator() ||
      ServerState::instance()->isDBServer()) {
    addClusterOptions(builder, _sharding, _replicationFactor, _writeConcern,
                      _replicationVersion);
  }

  if (withUsers) {
    builder.add(VPackValue("users"));
    UsersToVelocyPack(builder);
  }
}

void CreateDatabaseInfo::UsersToVelocyPack(VPackBuilder& builder) const {
  VPackArrayBuilder arrayGuard(&builder);
  for (auto const& user : _users) {
    VPackObjectBuilder objectGuard(&builder);
    builder.add("username", VPackValue(user.name));
    builder.add("passwd", VPackValue(user.password));
    builder.add("active", VPackValue(user.active));
    if (user.extra) {
      builder.add("extra", user.extra->slice());
    }
  }
}

ArangodServer& CreateDatabaseInfo::server() const { return _server; }

Result CreateDatabaseInfo::extractUsers(VPackSlice users) {
  if (users.isNone() || users.isNull()) {
    return Result();
  } else if (!users.isArray()) {
    events::CreateDatabase(_name, Result(TRI_ERROR_HTTP_BAD_PARAMETER),
                           _context);
    return Result(TRI_ERROR_HTTP_BAD_PARAMETER, "invalid users slice");
  }

  for (VPackSlice user : VPackArrayIterator(users)) {
    if (!user.isObject()) {
      events::CreateDatabase(_name, Result(TRI_ERROR_HTTP_BAD_PARAMETER),
                             _context);
      return Result(TRI_ERROR_HTTP_BAD_PARAMETER);
    }

    std::string name;
    bool userSet = false;
    for (std::string const& key :
         std::vector<std::string>{"username", "user"}) {
      auto slice = user.get(key);
      if (slice.isNone()) {
        continue;
      } else if (slice.isString()) {
        name = slice.copyString();
        userSet = true;
      } else {
        events::CreateDatabase(_name, Result(TRI_ERROR_HTTP_BAD_PARAMETER),
                               _context);
        return Result(TRI_ERROR_HTTP_BAD_PARAMETER);
      }
    }

    std::string password;
    if (VPackSlice passwd = user.get("passwd"); !passwd.isNone()) {
      if (!passwd.isString()) {
        events::CreateDatabase(_name, Result(TRI_ERROR_HTTP_BAD_PARAMETER),
                               _context);
        return Result(TRI_ERROR_HTTP_BAD_PARAMETER);
      }
      password = passwd.copyString();
    }

    bool active = true;
    if (VPackSlice act = user.get("active"); act.isBool()) {
      active = act.getBool();
    }

    std::shared_ptr<VPackBuilder> extraBuilder;
    if (VPackSlice extra = user.get("extra"); extra.isObject()) {
      extraBuilder = std::make_shared<VPackBuilder>();
      extraBuilder->add(extra);
    }

    if (userSet) {
      _users.emplace_back(std::move(name), std::move(password), active,
                          std::move(extraBuilder));
    } else {
      return Result(TRI_ERROR_HTTP_BAD_PARAMETER);
    }
  }
  return Result();
}

Result CreateDatabaseInfo::extractOptions(VPackSlice options, bool extractId,
                                          bool extractName) {
  if (options.isNone() || options.isNull()) {
    options = VPackSlice::emptyObjectSlice();
  }
  if (!options.isObject()) {
    events::CreateDatabase(_name, Result(TRI_ERROR_HTTP_BAD_PARAMETER),
                           _context);
    return Result(TRI_ERROR_HTTP_BAD_PARAMETER, "invalid options slice");
  }

  try {
    auto vocopts = getVocbaseOptions(_server, options, _strictValidation);
    _replicationFactor = vocopts.replicationFactor;
    _writeConcern = vocopts.writeConcern;
    _sharding = vocopts.sharding;
    _replicationVersion = vocopts.replicationVersion;

    if (extractName) {
      auto nameSlice = options.get(StaticStrings::DatabaseName);
      if (!nameSlice.isString()) {
        return Result(TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD, "no valid id given");
      }
      _name = nameSlice.copyString();
    }
    if (extractId) {
      auto idSlice = options.get(StaticStrings::DatabaseId);
      if (idSlice.isString()) {
        // improve once it works
        // look for some nice internal helper this has proably been done before
        auto idStr = idSlice.copyString();
        _id = basics::StringUtils::uint64(idSlice.stringView().data(),
                                          idSlice.stringView().size());

      } else if (idSlice.isUInt()) {
        _id = idSlice.getUInt();
      } else if (idSlice.isNone()) {
        // do nothing - can be set later - this sucks
      } else {
        return Result(TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD, "no valid id given");
      }
    }

    return checkOptions();
  } catch (basics::Exception const& ex) {
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL, "an unknown error occurred");
  }
}

Result CreateDatabaseInfo::checkOptions() {
  _validId = (_id != 0);

  if (_replicationVersion == replication::Version::TWO &&
      !replication2::EnableReplication2) {
    LOG_TOPIC("8fdd7", ERR, Logger::REPLICATION2)
        << "Replication version 2 is disabled in this binary, but loading a "
           "version 2 database "
        << "(named '" << _name << "'). "
        << "Creating such databases is disabled. Loading a version 2 database "
           "that was created with another binary will work, but it is strongly "
           "discouraged to use it in production. Please dump the data, and "
           "recreate the database with replication version 1 (the default), "
           "and then restore the data.";
  }

  bool isSystem = _name == StaticStrings::SystemDatabase;
  bool extendedNames = _server.getFeature<DatabaseFeature>().extendedNames();

  return DatabaseNameValidator::validateName(isSystem, extendedNames, _name);
}

#ifdef ARANGODB_USE_GOOGLE_TESTS
CreateDatabaseInfo::CreateDatabaseInfo(CreateDatabaseInfo::MockConstruct,
                                       ArangodServer& server,
                                       ExecContext const& execContext,
                                       std::string const& name,
                                       std::uint64_t id)
    : _server(server),
      _context(execContext),
      _id(id),
      _name(name),
      _valid(true) {}
#endif

VocbaseOptions getVocbaseOptions(ArangodServer& server, VPackSlice options,
                                 bool strictValidation) {
  TRI_ASSERT(options.isObject());
  // Invalid options will be silently ignored. Default values will be used
  // instead.
  //
  // This Function will be called twice - the second time we do not run in
  // the risk of consulting the ClusterFeature, because defaults are provided
  // during the first call.
  VocbaseOptions vocbaseOptions;
  vocbaseOptions.replicationFactor = 1;
  vocbaseOptions.writeConcern = 1;
  vocbaseOptions.sharding = "";

  vocbaseOptions.replicationVersion =
      server.getFeature<DatabaseFeature>().defaultReplicationVersion();

  //  sanitize input for vocbase creation
  //  sharding -- must be "", "flexible" or "single"
  //  replicationFactor must be "satellite" or a natural number
  //  writeConcern must be a natural number

  {
    auto shardingSlice = options.get(StaticStrings::Sharding);
    if (shardingSlice.isString() && shardingSlice.stringView() == "single") {
      vocbaseOptions.sharding = shardingSlice.copyString();
    }
  }

  bool haveCluster = server.hasFeature<ClusterFeature>();
  {
    VPackSlice replicationSlice = options.get(StaticStrings::ReplicationFactor);
    bool isSatellite =
        (replicationSlice.isString() &&
         replicationSlice.stringView() == StaticStrings::Satellite);
    bool isNumber = replicationSlice.isNumber();
    isSatellite = isSatellite || (isNumber && replicationSlice.getUInt() == 0);
    if (!isSatellite && !isNumber) {
      if (haveCluster) {
        vocbaseOptions.replicationFactor =
            server.getFeature<ClusterFeature>().defaultReplicationFactor();
      } else {
        LOG_TOPIC("eeeee", ERR, Logger::CLUSTER)
            << "Cannot access ClusterFeature to determine replicationFactor";
      }
    } else if (isSatellite) {
      vocbaseOptions.replicationFactor = 0;
    } else if (isNumber) {
      vocbaseOptions.replicationFactor =
          replicationSlice
              .getNumber<decltype(vocbaseOptions.replicationFactor)>();
      if (haveCluster && strictValidation) {
        auto const minReplicationFactor =
            server.getFeature<ClusterFeature>().minReplicationFactor();
        auto const maxReplicationFactor =
            server.getFeature<ClusterFeature>().maxReplicationFactor();
        // make sure the replicationFactor value is between the configured min
        // and max values
        if (0 < maxReplicationFactor &&
            maxReplicationFactor < vocbaseOptions.replicationFactor) {
          THROW_ARANGO_EXCEPTION_MESSAGE(
              TRI_ERROR_BAD_PARAMETER,
              absl::StrCat("replicationFactor must not be higher than "
                           "maximum allowed replicationFactor (",
                           maxReplicationFactor, ")"));
        } else if (0 < minReplicationFactor &&
                   vocbaseOptions.replicationFactor < minReplicationFactor) {
          THROW_ARANGO_EXCEPTION_MESSAGE(
              TRI_ERROR_BAD_PARAMETER,
              absl::StrCat("replicationFactor must not be lower than "
                           "minimum allowed replicationFactor (",
                           minReplicationFactor, ")"));
        }
      }
    }
#ifndef USE_ENTERPRISE
    if (vocbaseOptions.replicationFactor == 0) {
      if (haveCluster) {
        vocbaseOptions.replicationFactor =
            server.getFeature<ClusterFeature>().defaultReplicationFactor();
      } else {
        LOG_TOPIC("eeeef", ERR, Logger::CLUSTER)
            << "Cannot access ClusterFeature to determine replicationFactor";
        vocbaseOptions.replicationFactor = 1;
      }
    }
#endif
  }

  {
    // simon: new API in 3.6 no need to check legacy "minReplicationFactor"
    VPackSlice writeConcernSlice = options.get(StaticStrings::WriteConcern);
    bool isNumber = (writeConcernSlice.isNumber() &&
                     writeConcernSlice.getNumber<int>() > 0);
    if (!isNumber) {
      if (haveCluster) {
        vocbaseOptions.writeConcern =
            server.getFeature<ClusterFeature>().writeConcern();
      } else {
        LOG_TOPIC("eeeed", ERR, Logger::CLUSTER)
            << "Cannot access ClusterFeature to determine writeConcern";
      }
    } else if (isNumber) {
      vocbaseOptions.writeConcern =
          writeConcernSlice.getNumber<decltype(vocbaseOptions.writeConcern)>();
    }
  }

  {
    auto replicationVersionSlice =
        options.get(StaticStrings::ReplicationVersion);
    if (!replicationVersionSlice.isNone()) {
      auto res = replication::parseVersion(replicationVersionSlice);
      if (res.ok()) {
        auto version = res.get();

        vocbaseOptions.replicationVersion = version;
      } else {
        THROW_ARANGO_EXCEPTION(
            std::move(res).result().withError([&](result::Error& err) {
              err.resetErrorMessage(basics::StringUtils::concatT(
                  "Error parsing ", StaticStrings::ReplicationVersion, ": ",
                  err.errorMessage()));
            }));
      }
    }
  }

  return vocbaseOptions;
}

void addClusterOptions(VPackBuilder& builder, std::string const& sharding,
                       std::uint32_t replicationFactor,
                       std::uint32_t writeConcern,
                       replication::Version replicationVersion) {
  TRI_ASSERT(builder.isOpenObject());
  builder.add(StaticStrings::Sharding, VPackValue(sharding));
  if (replicationFactor) {
    builder.add(StaticStrings::ReplicationFactor,
                VPackValue(replicationFactor));
  } else {  // 0 is satellite
    builder.add(StaticStrings::ReplicationFactor,
                VPackValue(StaticStrings::Satellite));
  }
  builder.add(StaticStrings::WriteConcern, VPackValue(writeConcern));
  builder.add(StaticStrings::ReplicationVersion,
              VPackValue(replication::versionToString(replicationVersion)));
}

void addClusterOptions(VPackBuilder& builder, VocbaseOptions const& opt) {
  addClusterOptions(builder, opt.sharding, opt.replicationFactor,
                    opt.writeConcern, opt.replicationVersion);
}
}  // namespace arangodb
