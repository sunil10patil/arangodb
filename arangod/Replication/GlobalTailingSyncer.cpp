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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "GlobalTailingSyncer.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/StaticStrings.h"
#include "Basics/Thread.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Replication/GlobalInitialSyncer.h"
#include "Replication/ReplicationFeature.h"

#include <velocypack/Iterator.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::httpclient;

GlobalTailingSyncer::GlobalTailingSyncer(
    ReplicationApplierConfiguration const& configuration,
    TRI_voc_tick_t initialTick, bool useTick)
    : TailingSyncer(configuration._server.getFeature<ReplicationFeature>()
                        .globalReplicationApplier(),
                    configuration, initialTick, useTick),
      _queriedTranslations(false) {
  _ignoreDatabaseMarkers = false;
  _state.databaseName = StaticStrings::SystemDatabase;
}

std::shared_ptr<GlobalTailingSyncer> GlobalTailingSyncer::create(
    ReplicationApplierConfiguration const& configuration,
    TRI_voc_tick_t initialTick, bool useTick) {
  // enable make_shared on a class with a private constructor
  struct Enabler final : GlobalTailingSyncer {
    Enabler(ReplicationApplierConfiguration const& configuration,
            TRI_voc_tick_t initialTick, bool useTick)
        : GlobalTailingSyncer(configuration, initialTick, useTick) {}
  };

  return std::make_shared<Enabler>(configuration, initialTick, useTick);
}

std::string GlobalTailingSyncer::tailingBaseUrl(std::string const& command) {
  TRI_ASSERT(!_state.leader.endpoint.empty());
  TRI_ASSERT(_state.leader.serverId.isSet());
  TRI_ASSERT(_state.leader.majorVersion != 0);

  if (_state.leader.version() < 30300) {
    std::string err =
        "You need >= 3.3 to perform the replication of an entire server";
    LOG_TOPIC("75fa1", ERR, Logger::REPLICATION) << err;
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED, err);
  }
  return TailingSyncer::WalAccessUrl + "/" + command + "?global=true&";
}

/// @brief save the current applier state
Result GlobalTailingSyncer::saveApplierState() {
  return _applier->persistStateResult(false);
}

bool GlobalTailingSyncer::skipMarker(VPackSlice slice) {
  // we do not have a "cname" attribute in the marker...
  // now check for a globally unique id attribute ("cuid")
  // if its present, then we will use our local cuid -> collection name
  // translation table
  VPackSlice name = slice.get("cuid");
  if (!name.isString()) {
    return false;
  }

  if (!_queriedTranslations) {
    // no translations yet... query leader inventory to find names of all
    // collections
    try {
      auto syncer = GlobalInitialSyncer::create(_state.applier);
      VPackBuilder inventoryResponse;
      Result res = syncer->getInventory(inventoryResponse);
      _queriedTranslations = true;

      if (res.fail()) {
        LOG_TOPIC("e25ae", ERR, Logger::REPLICATION)
            << "got error while fetching leader inventory for collection name "
               "translations: "
            << res.errorMessage();
        return false;
      }

      VPackSlice invSlice = inventoryResponse.slice();
      if (!invSlice.isObject()) {
        return false;
      }
      invSlice = invSlice.get("databases");
      if (!invSlice.isObject()) {
        return false;
      }

      for (auto it : VPackObjectIterator(invSlice)) {
        VPackSlice dbObj = it.value;
        if (!dbObj.isObject()) {
          continue;
        }

        dbObj = dbObj.get("collections");
        if (!dbObj.isArray()) {
          return false;
        }

        for (VPackSlice it : VPackArrayIterator(dbObj)) {
          if (!it.isObject()) {
            continue;
          }
          VPackSlice c = it.get("parameters");
          if (c.hasKey("name") && c.hasKey("globallyUniqueId")) {
            // we'll store everything for all databases in a global hash table,
            // as we expect the globally unique ids to be unique...
            _translations[c.get("globallyUniqueId").copyString()] =
                c.get("name").copyString();
          }
        }
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("2c5c2", ERR, Logger::REPLICATION)
          << "got error while fetching inventory: " << ex.what();
      return false;
    }
  }

  // look up cuid in translations map
  auto it = _translations.find(name.copyString());

  if (it != _translations.end()) {
    return isExcludedCollection((*it).second);
  }

  return false;
}
