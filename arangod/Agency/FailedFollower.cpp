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
////////////////////////////////////////////////////////////////////////////////

#include "FailedFollower.h"

#include "Agency/Agent.h"
#include "Agency/Job.h"
#include "Agency/JobContext.h"
#include "Agency/Node.h"
#include "Basics/StaticStrings.h"
#include "Basics/TimeString.h"
#include "Logger/LogMacros.h"

using namespace arangodb::consensus;
using namespace arangodb::velocypack;

FailedFollower::FailedFollower(
    Node const& snapshot, AgentInterface* agent, std::string const& jobId,
    std::string const& creator, std::string const& database,
    std::string const& collection, std::string const& shard,
    std::string const& from, std::string const& notBefore)
    : Job(NOTFOUND, snapshot, agent, jobId, creator),
      _database(database),
      _collection(collection),
      _shard(shard),
      _from(from),
      _notBefore(stringToTimepoint(notBefore)) {}

FailedFollower::FailedFollower(Node const& snapshot, AgentInterface* agent,
                               JOB_STATUS status, std::string const& jobId)
    : Job(status, snapshot, agent, jobId) {
  // Get job details from agency:
  std::string path = pos[status] + _jobId + "/";
  auto tmp_database = _snapshot.hasAsString(path + "database");
  auto tmp_collection = _snapshot.hasAsString(path + "collection");
  auto tmp_from = _snapshot.hasAsString(path + "fromServer");

  // set only if already started (test to prevent warning)
  if (_snapshot.has(path + "toServer")) {
    auto tmp_to = _snapshot.hasAsString(path + "toServer");
    _to = tmp_to.value();
  }

  auto tmp_shard = _snapshot.hasAsString(path + "shard");
  auto tmp_creator = _snapshot.hasAsString(path + "creator");
  auto tmp_created = _snapshot.hasAsString(path + "timeCreated");
  auto tmp_notBefore = _snapshot.hasAsString(path + "notBefore");

  if (tmp_notBefore) {
    _notBefore = stringToTimepoint(tmp_notBefore.value());
  }

  if (tmp_database && tmp_collection && tmp_from && tmp_shard && tmp_creator &&
      tmp_created) {
    _database = tmp_database.value();
    _collection = tmp_collection.value();
    _from = tmp_from.value();
    // _to conditionally set above
    _shard = tmp_shard.value();
    _creator = tmp_creator.value();
    _created = stringToTimepoint(tmp_created.value());
  } else {
    std::stringstream err;
    err << "Failed to find job " << _jobId << " in agency";
    LOG_TOPIC("4667d", ERR, Logger::SUPERVISION) << err.str();
    finish("", _shard, false, err.str());
    _status = FAILED;
  }
}

FailedFollower::~FailedFollower() = default;

void FailedFollower::run(bool& aborts) { runHelper("", _shard, aborts); }

bool FailedFollower::create(std::shared_ptr<VPackBuilder> envelope) {
  using namespace std::chrono;
  LOG_TOPIC("b0a34", INFO, Logger::SUPERVISION)
      << "Create failedFollower for " << _shard << " from " << _from;

  _created = system_clock::now();

  if (envelope == nullptr) {
    _jb = std::make_shared<Builder>();
    _jb->openArray();
    _jb->openObject();
  } else {
    _jb = envelope;
  }

  // Todo entry
  _jb->add(VPackValue(toDoPrefix + _jobId));
  {
    VPackObjectBuilder todo(_jb.get());
    _jb->add("creator", VPackValue(_creator));
    _jb->add("type", VPackValue("failedFollower"));
    _jb->add("database", VPackValue(_database));
    _jb->add("collection", VPackValue(_collection));
    _jb->add("shard", VPackValue(_shard));
    _jb->add("fromServer", VPackValue(_from));
    _jb->add("jobId", VPackValue(_jobId));
    _jb->add("timeCreated", VPackValue(timepointToString(_created)));
    _jb->add("notBefore", VPackValue(timepointToString(_notBefore)));
  }

  if (envelope == nullptr) {
    _jb->close();  // object
    _jb->close();  // array
    write_ret_t res = singleWriteTransaction(_agent, *_jb, false);
    return (res.accepted && res.indices.size() == 1 && res.indices[0]);
  }

  return true;
}

bool FailedFollower::start(bool& aborts) {
  using namespace std::chrono;

  std::vector<std::string> existing =
      _snapshot.exists(planColPrefix + _database + "/" + _collection + "/" +
                       "distributeShardsLike");

  // Fail if got distributeShardsLike
  if (existing.size() == 5) {
    finish("", _shard, false, "Collection has distributeShardsLike");
    return false;
  }
  // Collection gone
  else if (existing.size() < 4) {
    finish("", _shard, true, "Collection " + _collection + " gone");
    return false;
  }

  // Planned servers vector
  std::string planPath =
      planColPrefix + _database + "/" + _collection + "/shards/" + _shard;
  auto plannedPair = _snapshot.get(planPath);
  if (!plannedPair) {
    finish("", _shard, true,
           "Plan entry for collection " + _collection + " gone");
    return false;
  }
  auto builder = plannedPair->toBuilder();
  Slice planned = builder.slice();

  // Now check if _server is still in this plan, note that it could have
  // been removed by RemoveFollower already, in which case we simply stop:
  bool found = false;
  if (planned.isArray()) {
    for (VPackSlice s : VPackArrayIterator(planned)) {
      if (s.isString() && _from == s.stringView()) {
        found = true;
        break;
      }
    }
  }
  if (!found) {
    finish("", _shard, true,
           "Server no longer found in Plan for collection " + _collection +
               ", our job is done.");
    return false;
  }

  // Exclude servers in failoverCandidates for some clone and those in Plan:
  auto shardsLikeMe = clones(_snapshot, _database, _collection, _shard);

  // Check if configured delay has passed since the creation of the job:
  auto now = std::chrono::system_clock::now();
  if (now < _notBefore) {
    LOG_TOPIC("1de2d", DEBUG, Logger::SUPERVISION)
        << "shard " << _shard
        << " needs FailedFollower but configured FailedFollowerDelay has not "
           "yet passed, delaying job "
        << _jobId;
    return false;
  }

  auto failoverCands =
      Job::findAllFailoverCandidates(_snapshot, _database, shardsLikeMe);
  std::vector<std::string> excludes;
  for (const auto& s : VPackArrayIterator(planned)) {
    if (s.isString()) {
      std::string id = s.copyString();
      if (failoverCands.find(id) == failoverCands.end()) {
        excludes.push_back(std::move(id));
      }
    }
  }
  for (auto const& id : failoverCands) {
    excludes.push_back(id);
  }

  // Get proper replacement:
  _to = randomIdleAvailableServer(_snapshot, excludes);
  if (_to.empty()) {
    finish("", _shard, false, "No server available.");
    return false;
  }

  if (std::chrono::system_clock::now() - _created >
      std::chrono::seconds(4620)) {
    finish("", _shard, false, "Job timed out");
    return false;
  }

  LOG_TOPIC("80b9d", INFO, Logger::SUPERVISION)
      << "Start failedFollower for " << _shard << " from " << _from << " to "
      << _to;

  // Copy todo to pending
  Builder todo;
  {
    VPackArrayBuilder a(&todo);
    if (_jb == nullptr) {
      auto const& jobIdNode = _snapshot.get(toDoPrefix + _jobId);
      if (jobIdNode) {
        jobIdNode->toBuilder(todo);
      } else {
        LOG_TOPIC("4571c", INFO, Logger::SUPERVISION)
            << "Failed to get key " << toDoPrefix << _jobId
            << " from agency snapshot";
        return false;
      }
    } else {
      todo.add(_jb->slice()[0].get(toDoPrefix + _jobId));
    }
  }

  // Replace from by to in plan and that's it
  Builder ns;
  {
    VPackArrayBuilder servers(&ns);
    for (auto const& i : VPackArrayIterator(planned)) {
      auto s = i.copyString();
      ns.add(VPackValue((s != _from) ? s : _to));
    }
    // Add the old one at the end, just in case it comes back more quickly:
    ns.add(VPackValue(_from));
  }

  // Transaction
  Builder job;

  {
    VPackArrayBuilder transactions(&job);

    {
      VPackArrayBuilder transaction(&job);
      // Operations ----------------------------------------------------------
      {
        VPackObjectBuilder operations(&job);
        // Add finished entry -----
        job.add(VPackValue(finishedPrefix + _jobId));
        {
          VPackObjectBuilder ts(&job);
          job.add("timeStarted",  // start
                  VPackValue(timepointToString(system_clock::now())));
          job.add("timeFinished",  // same same :)
                  VPackValue(timepointToString(system_clock::now())));
          job.add("toServer", VPackValue(_to));  // toServer
          for (auto const& obj : VPackObjectIterator(todo.slice()[0])) {
            job.add(obj.key.stringView(), obj.value);
          }
        }
        addRemoveJobFromSomewhere(job, "ToDo", _jobId);
        // Plan change ------------
        for (auto const& clone : shardsLikeMe) {
          job.add(planColPrefix + _database + "/" + clone.collection +
                      "/shards/" + clone.shard,
                  ns.slice());
        }

        addIncreasePlanVersion(job);
      }
      // Preconditions -------------------------------------------------------
      {
        VPackObjectBuilder preconditions(&job);
        // Failed condition persists
        job.add(VPackValue(healthPrefix + _from + "/Status"));
        {
          VPackObjectBuilder stillExists(&job);
          job.add("old", VPackValue(Supervision::HEALTH_STATUS_FAILED));
        }
        // Plan still as we see it:
        addPreconditionUnchanged(job, planPath, planned);
        // Check that failoverCandidates are still as we inspected them:
        doForAllShards(
            _snapshot, _database, shardsLikeMe,
            [this, &job](Slice plan, Slice current, std::string& planPath,
                         std::string& curPath) {
              // take off "servers" from curPath and add
              // "failoverCandidates":
              std::string foCandsPath = curPath.substr(0, curPath.size() - 7);
              foCandsPath += StaticStrings::FailoverCandidates;
              auto foCands = this->_snapshot.hasAsBuilder(foCandsPath);
              if (foCands) {
                addPreconditionUnchanged(job, foCandsPath, foCands->slice());
              }
            });
        // toServer not blocked
        addPreconditionServerNotBlocked(job, _to);
        // shard not blocked
        addPreconditionShardNotBlocked(job, _shard);
        // toServer in good condition
        addPreconditionServerHealth(job, _to, Supervision::HEALTH_STATUS_GOOD);
      }
    }
  }

  // Abort job blocking shard if abortable
  //  (likely to not exist, avoid warning message by testing first)
  if (_snapshot.has(blockedShardsPrefix + _shard)) {
    auto jobId = _snapshot.hasAsString(blockedShardsPrefix + _shard);
    if (jobId && !abortable(_snapshot, *jobId)) {
      return false;
    } else if (jobId) {
      aborts = true;
      JobContext(PENDING, *jobId, _snapshot, _agent)
          .abort("failed follower requests abort");
      return false;
    }
  }

  LOG_TOPIC("2b961", DEBUG, Logger::SUPERVISION)
      << "FailedFollower start transaction: " << job.toJson();

  auto res = generalTransaction(_agent, job);
  if (!res.accepted) {  // lost leadership
    LOG_TOPIC("f5b87", INFO, Logger::SUPERVISION)
        << "Leadership lost! Job " << _jobId << " handed off.";
    return false;
  }

  LOG_TOPIC("84797", DEBUG, Logger::SUPERVISION)
      << "FailedFollower start result: " << res.result->toJson();

  auto result = res.result->slice()[0];

  if (res.accepted && result.isNumber()) {
    return true;
  }

  TRI_ASSERT(result.isObject());

  auto slice = result.get(std::vector<std::string>(
      {agencyPrefix, "Supervision", "Health", _from, "Status"}));
  if (slice.isString() &&
      slice.stringView() != Supervision::HEALTH_STATUS_FAILED) {
    finish("", _shard, false, "Server " + _from + " no longer failing.");
  }

  slice = result.get(std::vector<std::string>(
      {agencyPrefix, "Supervision", "Health", _to, "Status"}));
  if (!slice.isString() ||
      slice.stringView() != Supervision::HEALTH_STATUS_GOOD) {
    LOG_TOPIC("4785b", INFO, Logger::SUPERVISION)
        << "Destination server " << _to << " is no longer in good condition";
  }

  slice = result.get(
      std::vector<std::string>({agencyPrefix, "Plan", "Collections", _database,
                                _collection, "shards", _shard}));
  if (!slice.isNone()) {
    LOG_TOPIC("9fd56", INFO, Logger::SUPERVISION)
        << "Planned db server list is in mismatch with snapshot";
  }

  slice = result.get(std::vector<std::string>(
      {agencyPrefix, "Supervision", "DBServers", _to}));
  if (!slice.isNone()) {
    LOG_TOPIC("ad849", INFO, Logger::SUPERVISION)
        << "Destination " << _to << " is now blocked by job "
        << slice.stringView();
  }

  slice = result.get(std::vector<std::string>(
      {agencyPrefix, "Supervision", "Shards", _shard}));
  if (!slice.isNone()) {
    LOG_TOPIC("57da4", INFO, Logger::SUPERVISION)
        << "Shard " << _shard << " is now blocked by job "
        << slice.stringView();
  }

  return false;
}

JOB_STATUS FailedFollower::status() {
  // We can only be hanging around TODO. start === finished
  return TODO;
}

arangodb::Result FailedFollower::abort(std::string const& reason) {
  // We can assume that the job is in ToDo or not there:
  if (_status == NOTFOUND || _status == FINISHED || _status == FAILED) {
    return Result(TRI_ERROR_SUPERVISION_GENERAL_FAILURE,
                  "Failed aborting failedFollower job beyond pending stage");
  }

  // Can now only be TODO
  if (_status == TODO) {
    finish("", "", false, "job aborted: " + reason);
    return Result{};
  }

  TRI_ASSERT(false);  // cannot happen, since job moves directly to FINISHED
  return Result{};
}
