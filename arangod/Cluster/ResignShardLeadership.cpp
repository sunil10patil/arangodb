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
/// @author Kaveh Vahedipour
/// @author Matthew Von-Maszewski
////////////////////////////////////////////////////////////////////////////////

#include "ResignShardLeadership.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/FollowerInfo.h"
#include "Cluster/MaintenanceFeature.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "RestServer/DatabaseFeature.h"
#include "Transaction/ClusterUtils.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/DatabaseGuard.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/Databases.h"

#include <velocypack/Compare.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::maintenance;
using namespace arangodb::methods;

std::string const ResignShardLeadership::LeaderNotYetKnownString =
    "LEADER_NOT_YET_KNOWN";

ResignShardLeadership::ResignShardLeadership(MaintenanceFeature& feature,
                                             ActionDescription const& desc)
    : ActionBase(feature, desc),
      ShardDefinition(desc.get(DATABASE), desc.get(SHARD)) {
  std::stringstream error;

  _labels.emplace(FAST_TRACK);

  if (!ShardDefinition::isValid()) {
    error << "database and shard must be specified. ";
  }

  if (!error.str().empty()) {
    LOG_TOPIC("2aa84", ERR, Logger::MAINTENANCE)
        << "ResignLeadership: " << error.str();
    result(TRI_ERROR_INTERNAL, error.str());
    setState(FAILED);
  }
}

ResignShardLeadership::~ResignShardLeadership() = default;

bool ResignShardLeadership::first() {
  std::string const& database = getDatabase();
  std::string const& collection = getShard();

  LOG_TOPIC("14f43", DEBUG, Logger::MAINTENANCE)
      << "trying to withdraw as leader of shard '" << database << "/"
      << collection;

  // This starts a write transaction, just to wait for any ongoing
  // write transaction on this shard to terminate. We will then later
  // report to Current about this resignation. If a new write operation
  // starts in the meantime (which is unlikely, since no coordinator that
  // has seen the _ will start a new one), it is doomed, and we ignore the
  // problem, since similar problems can arise in failover scenarios anyway.

  try {
    // Guard database againts deletion for now
    auto& df = _feature.server().getFeature<DatabaseFeature>();
    DatabaseGuard guard(df, database);
    auto vocbase = &guard.database();

    auto col = vocbase->lookupCollection(collection);
    if (col == nullptr) {
      std::stringstream error;
      error << "Failed to lookup local collection " << collection
            << " in database " << database;
      LOG_TOPIC("e06ca", ERR, Logger::MAINTENANCE)
          << "ResignLeadership: " << error.str();
      result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, error.str());
      return false;
    }

    // Get write transaction on collection
    transaction::StandaloneContext ctx(*vocbase);
    SingleCollectionTransaction trx{
        std::shared_ptr<transaction::Context>(
            std::shared_ptr<transaction::Context>(), &ctx),
        *col, AccessMode::Type::EXCLUSIVE};

    Result res = trx.begin();

    if (!res.ok()) {
      THROW_ARANGO_EXCEPTION(res);
    }

    // Note that it is likely that we will be a follower for this shard
    // with another leader in due course. However, we do not know the
    // name of the new leader yet. This setting will make us a follower
    // for now but we will not accept any replication operation from any
    // leader, until we have negotiated a deal with it. Then the actual
    // name of the leader will be set.
    col->followers()->setTheLeader(LeaderNotYetKnownString);  // resign
    res = trx.abort();                                        // unlock
    if (res.fail()) {
      LOG_TOPIC("10c35", ERR, Logger::MAINTENANCE)
          << "Failed to abort transaction during resign leadership: " << res;
    }

    transaction::cluster::abortLeaderTransactionsOnShard(col->id());

  } catch (std::exception const& e) {
    std::stringstream error;
    error << "exception thrown when resigning:" << e.what();
    LOG_TOPIC("173dd", ERR, Logger::MAINTENANCE)
        << "ResignLeadership: " << error.str();
    result(TRI_ERROR_INTERNAL, error.str());
    return false;
  }

  return false;
}

void ResignShardLeadership::setState(ActionState state) {
  if ((COMPLETE == state || FAILED == state) && _state != state) {
    _feature.unlockShard(getShard());
  }
  ActionBase::setState(state);
}
