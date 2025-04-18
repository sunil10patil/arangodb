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

#include "Agent.h"

#include <velocypack/Iterator.h>

#include <chrono>
#include <thread>

#include "Agency/AgencyFeature.h"
#include "Agency/AgentCallback.h"
#include "Agency/Supervision.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/ReadLocker.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/system-functions.h"
#include "Basics/WriteLocker.h"
#include "Basics/application-exit.h"
#include "Logger/LogMacros.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Metrics/CounterBuilder.h"
#include "Metrics/GaugeBuilder.h"
#include "Metrics/HistogramBuilder.h"
#include "Metrics/LogScale.h"
#include "Metrics/MetricsFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"
#include "VocBase/vocbase.h"

using namespace arangodb::application_features;
using namespace arangodb::velocypack;
using namespace std::chrono;

struct AppendScale {
  static arangodb::metrics::LogScale<float> scale() {
    return {2.f, 0.f, 1000.f, 10};
  }
};
struct AgentScale {
  static arangodb::metrics::LogScale<float> scale() {
    return {std::exp(1.f), 0.f, 200.f, 10};
  }
};

DECLARE_HISTOGRAM(arangodb_agency_append_hist, AppendScale,
                  "Agency write histogram [ms]");
DECLARE_HISTOGRAM(arangodb_agency_commit_hist, AgentScale,
                  "Agency RAFT commit histogram [ms]");
DECLARE_HISTOGRAM(arangodb_agency_compaction_hist, AgentScale,
                  "Agency compaction histogram [ms]");
DECLARE_GAUGE(arangodb_agency_local_commit_index, uint64_t,
              "This agent's commit index");
DECLARE_COUNTER(arangodb_agency_read_no_leader_total, "Agency read no leader");
DECLARE_COUNTER(arangodb_agency_read_ok_total, "Agency read ok");
DECLARE_HISTOGRAM(arangodb_agency_write_hist, AgentScale,
                  "Agency write histogram [ms]");
DECLARE_COUNTER(arangodb_agency_write_no_leader_total,
                "Agency write no leader");
DECLARE_COUNTER(arangodb_agency_write_ok_total, "Agency write ok");

namespace arangodb {
namespace consensus {

// Instanciations of some declarations in AgencyCommon.h:

std::string const pubApiPrefix("/_api/agency/");
std::string const privApiPrefix("/_api/agency_priv/");
std::string const NO_LEADER("");

/// Agent configuration
Agent::Agent(ArangodServer& server, config_t const& config)
    : arangodb::ServerThread<ArangodServer>(server, "Agent"),
      _constituent(server),
      _supervision(std::make_unique<Supervision>(server)),
      _state(server),
      _config(config),
      _commitIndex(0),
      _agentNeedsWakeup(false),
      _compactor(this),
      _ready(false),
      _preparing(0),
      _loaded(false),
      _write_ok(server.getFeature<arangodb::metrics::MetricsFeature>().add(
          arangodb_agency_write_ok_total{})),
      _write_no_leader(
          server.getFeature<arangodb::metrics::MetricsFeature>().add(
              arangodb_agency_write_no_leader_total{})),
      _read_ok(server.getFeature<arangodb::metrics::MetricsFeature>().add(
          arangodb_agency_read_ok_total{})),
      _read_no_leader(
          server.getFeature<arangodb::metrics::MetricsFeature>().add(
              arangodb_agency_read_no_leader_total{})),
      _write_hist_msec(
          server.getFeature<arangodb::metrics::MetricsFeature>().add(
              arangodb_agency_write_hist{})),
      _commit_hist_msec(
          server.getFeature<arangodb::metrics::MetricsFeature>().add(
              arangodb_agency_commit_hist{})),
      _append_hist_msec(
          server.getFeature<arangodb::metrics::MetricsFeature>().add(
              arangodb_agency_append_hist{})),
      _compaction_hist_msec(
          server.getFeature<arangodb::metrics::MetricsFeature>().add(
              arangodb_agency_compaction_hist{})),
      _local_index(server.getFeature<arangodb::metrics::MetricsFeature>().add(
          arangodb_agency_local_commit_index{})) {
  _state.configure(this);
  _constituent.configure(this);
  _inception = std::make_unique<Inception>(*this);

  auto const notifySupervision = [this](std::string_view, VPackSlice) {
    _supervision->notify();
  };

  _spearhead.registerPrefixTrigger("/arango/Target/ReplicatedLogs",
                                   notifySupervision);
  _spearhead.registerPrefixTrigger("/arango/Plan/ReplicatedLogs",
                                   notifySupervision);
  _spearhead.registerPrefixTrigger("/arango/Current/ReplicatedLogs",
                                   notifySupervision);
  _spearhead.registerPrefixTrigger("/arango/Target/CollectionGroups",
                                   notifySupervision);
  _spearhead.registerPrefixTrigger("/arango/Plan/CollectionGroups",
                                   notifySupervision);
  _spearhead.registerPrefixTrigger("/arango/Current/Collections",
                                   notifySupervision);
}

/// Dtor shuts down thread
Agent::~Agent() {
  waitForThreadsStop();
  // This usually was already done called from AgencyFeature::unprepare,
  // but since this only waits for the threads to stop, it can be done
  // multiple times, and we do it just in case the Agent object was
  // created but never really started. Here, we exit with a fatal error
  // if the threads do not stop in time.
  shutdown();  // wait for the main Agent thread to terminate
}

/// This agent's id
std::string Agent::id() const { return _config.id(); }

// Under no circumstances guard the member. Metrics guard themselves.
decltype(Agent::_commit_hist_msec) Agent::commitHist() const {
  return _commit_hist_msec;
}

/// Agent's id is set once from state machine
bool Agent::id(std::string const& id) {
  bool success;
  if ((success = _config.setId(id))) {
    LOG_TOPIC("32d95", DEBUG, Logger::AGENCY) << "My id is " << id;
  } else {
    LOG_TOPIC("37f6b", ERR, Logger::AGENCY)
        << "Cannot reassign id once set: My id is " << _config.id()
        << " reassignment to " << id;
  }
  return success;
}

/// Merge command line and persisted comfigurations
bool Agent::mergeConfiguration(VPackSlice persisted) {
  auto res = _config.merge(persisted);  // Concurrency managed in merge
  syncActiveAndAcknowledged();
  return res;
}

/// Wakeup main loop of the agent (needed from Constituent)
void Agent::wakeupMainLoop() {
  std::lock_guard guard{_appendCV.mutex};
  _agentNeedsWakeup = true;
  _appendCV.cv.notify_all();
}

/// Wait until threads are terminated:
void Agent::waitForThreadsStop() {
  // It is allowed to call this multiple times, we do so from the constructor
  // and from AgencyFeature::unprepare.
  int counter = 0;
  while (_constituent.isRunning() || _compactor.isRunning() ||
         (_config.supervision() && _supervision->isRunning()) ||
         (_inception != nullptr && _inception->isRunning())) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // fail fatally after 5 mins:
    if (++counter >= 10 * 60 * 5) {
      LOG_TOPIC("b8ad5", FATAL, Logger::AGENCY)
          << "some agency thread did not finish";
      FATAL_ERROR_EXIT();
    }
  }
  // initiate shutdown of main Agent thread, but do not wait for it yet
  // -> this happens in the destructor
  beginShutdown();
}

/// State machine
State const& Agent::state() const { return _state; }

/// Start all agent thread
bool Agent::start() {
  LOG_TOPIC("9a90e", DEBUG, Logger::AGENCY) << "Starting agency comm worker.";
  Thread::start();
  return true;
}

/// This agent's term
term_t Agent::term() const { return _constituent.term(); }

/// Agency size
size_t Agent::size() const { return _config.size(); }

/// My endpoint
std::string Agent::endpoint() const { return _config.endpoint(); }

/// Handle voting
priv_rpc_ret_t Agent::requestVote(term_t termOfPeer, std::string const& id,
                                  index_t lastLogIndex, index_t lastLogTerm,
                                  int64_t timeoutMult) {
  if (timeoutMult != -1 && timeoutMult != _config.timeoutMult()) {
    adjustTimeoutMult(timeoutMult);
    LOG_TOPIC("81f2a", WARN, Logger::AGENCY)
        << "Voter: setting timeout multiplier to " << timeoutMult
        << " for next term.";
  }

  bool doIVote = _constituent.vote(termOfPeer, id, lastLogIndex, lastLogTerm);
  return priv_rpc_ret_t(doIVote, this->term());
}

/// Get reference to momentary configuration
config_t const& Agent::config() const { return _config; }

/// Adjust timeoutMult:
void Agent::adjustTimeoutMult(int64_t timeoutMult) {
  _config.setTimeoutMult(timeoutMult);
}

/// Get timeoutMult:
int64_t Agent::getTimeoutMult() const { return _config.timeoutMult(); }

/// Leader's id
std::string Agent::leaderID() const { return _constituent.leaderID(); }

/// Are we leading?
bool Agent::leading() const {
  // When we become leader, we are first entering a preparation phase.
  // Note that this method returns true regardless of whether we
  // are still preparing or not. The preparation phases 1 and 2 are
  // indicated by the _preparing member in the Agent, the Constituent is
  // already LEADER.
  // The Constituent has to send out AppendEntriesRPC calls immediately, but
  // only when we are properly leading (with initialized stores etc.)
  // can we execute requests.
  return _constituent.leading();
}

// Waits here for confirmation of log's commits up to index. Timeout in seconds.
AgentInterface::raft_commit_t Agent::waitFor(index_t index, double timeout) {
  auto startTime = steady_clock::now();
  index_t lastCommitIndex = 0;

  // Wait until woken up through AgentCallback
  while (true) {
    /// success?
    ///  (_waitForCV's mutex stops writes to _commitIndex)
    std::unique_lock guard{_waitForCV.mutex};
    auto ci = _commitIndex.load(std::memory_order_relaxed);
    if (leading()) {
      if (lastCommitIndex != ci) {
        // We restart the timeout computation if there has been progress:
        startTime = steady_clock::now();
      }
      lastCommitIndex = ci;
      if (lastCommitIndex >= index) {
        return Agent::raft_commit_t::OK;
      }
    } else {
      return Agent::raft_commit_t::UNKNOWN;
    }

    duration<double> d = steady_clock::now() - startTime;

    LOG_TOPIC("37e05", DEBUG, Logger::AGENCY)
        << "waitFor: index: " << index << " _commitIndex: " << ci
        << " _lastCommitIndex: " << lastCommitIndex
        << " elapsedTime: " << d.count();

    if (d.count() >= timeout) {
      return Agent::raft_commit_t::TIMEOUT;
    }

    // Go to sleep:
    _waitForCV.cv.wait_for(
        guard, std::chrono::microseconds{
                   static_cast<uint64_t>(1.0e6 * (timeout - d.count()))});

    // shutting down
    if (isStopping()) {
      return Agent::raft_commit_t::UNKNOWN;
    }
  }

  // We should never get here
  TRI_ASSERT(false);

  return Agent::raft_commit_t::UNKNOWN;
}

// Check if log is committed up to index.
bool Agent::isCommitted(index_t index) const {
  std::lock_guard guard{_waitForCV.mutex};
  if (leading()) {
    return _commitIndex.load(std::memory_order_relaxed) >= index;
  } else {
    return false;
  }
}

index_t Agent::index() {
  if (challengeLeadership()) {
    resign();
    return 0;
  }

  return _followerData.getLockedGuard()->at(id())._lastAckedIndex;
}

//  AgentCallback reports id of follower and its highest processed index
void Agent::reportIn(std::string const& peerId, index_t index, size_t toLog) {
  auto startTime = steady_clock::now();

  auto followerLock = _followerData.getLockedGuard();
  FollowerData& follower = followerLock->operator[](peerId);

  if (index == 0) {
    // This is only the empty case (=heartbeat)
    auto n = steady_clock::now();
    // intentional to add entry to map
    auto lastTime = follower._lastEmptyAcked;
    if (lastTime < n) {
      std::chrono::duration<double> d = n - lastTime;
      auto secsSince = d.count();
      if (secsSince < 1.5e9 && peerId != id() &&
          secsSince > _config.minPing() * _config.timeoutMult()) {
        LOG_TOPIC("6fe73", WARN, Logger::AGENCY)
            << "Last confirmation from peer " << peerId
            << " was received more than minPing ago: " << secsSince;
      }
      LOG_TOPIC("9ee0c", DEBUG, Logger::AGENCY)
          << "Setting _lastEmptyAcked[" << peerId << "] to time "
          << std::chrono::duration_cast<std::chrono::microseconds>(
                 n.time_since_epoch())
                 .count();
      follower._lastEmptyAcked = n;
    }
    return;
  }

  // only update the time stamps here:
  {
    // Update last acknowledged answer
    auto t = steady_clock::now();

    // Reference here, the entry will be updated.
    auto& lastTime = follower._lastAckedTime;
    auto& lastIndex = follower._lastAckedIndex;
    LOG_TOPIC("9ee0b", DEBUG, Logger::AGENCY)
        << "Setting _lastAcked[" << peerId << "] to time "
        << std::chrono::duration_cast<std::chrono::microseconds>(
               t.time_since_epoch())
               .count();
    lastTime = t;
    if (index > lastIndex) {  // progress this follower?
      lastIndex = index;
      if (toLog >
          0) {  // We want to reset the wait time only if a package callback
        LOG_TOPIC("ba4d2", DEBUG, Logger::AGENCY)
            << "Got call back of " << toLog
            << " logs, resetting _earliestPackage to now for id " << peerId;
        follower._earliestPackage = steady_clock::now();
      }
      wakeupMainLoop();  // only necessary for non-empty callbacks
    }
  }

  duration<double> reportInTime = steady_clock::now() - startTime;
  if (reportInTime.count() > 0.1) {
    LOG_TOPIC("b4854", DEBUG, Logger::AGENCY)
        << "reportIn took longer than 0.1s: " << reportInTime.count();
  }
}

/// @brief Report a failed append entry call from AgentCallback
void Agent::reportFailed(std::string const& followerId, size_t toLog,
                         bool sent) {
  auto follower = getFollower(followerId);

  if (toLog > 0) {
    // This is only used for non-empty appendEntriesRPC calls. If such calls
    // fail, we have to set this earliestPackage time to now such that the
    // main thread tries again immediately: and for that agent starting at 0
    // which effectively will be _state.firstIndex().

    LOG_TOPIC("9e856", DEBUG, Logger::AGENCY)
        << "Resetting _earliestPackage to now for id " << followerId;
    follower->_earliestPackage = steady_clock::now() + seconds(1);
    follower->_lastAckedIndex = 0;
  } else {
    // answer to sendAppendEntries to empty request, when follower's highest
    // log index is 0. This is necessary so that a possibly restarted agent
    // without persistence immediately is brought up to date. We only want to do
    // this, when the agent was able to answer and no or corrupt answer is
    // handled
    if (sent) {
      follower->_lastAckedIndex = 0;
    }
  }
}

void Agent::logsForTrigger() {
  auto builder = std::make_shared<VPackBuilder>();
  // Wake up poll rest handlers:
  // Get everything from _lowestPromise
  // Create one builder pass shared pointer to all rest handlers
  // Every resthandler takes, what it needs.
  // Delete all promises.
  // Reset _lowestPromise.
  std::lock_guard lck(_promLock);
  {
    VPackObjectBuilder e(builder.get());
    auto const logs = _state.get(_lowestPromise,
                                 _commitIndex.load(std::memory_order_relaxed));

    TRI_ASSERT(!logs.empty());
    if (!logs.empty()) {
      builder->add(VPackValue("result"));
      VPackObjectBuilder e(builder.get());
      builder->add("firstIndex", VPackValue(logs.front().index));
      builder->add("commitIndex", VPackValue(logs.back().index));
      builder->add(VPackValue("log"));
      VPackArrayBuilder ls(builder.get());
      for (auto const& i : logs) {
        VPackObjectBuilder l(builder.get());
        builder->add("index", VPackValue(i.index));
        builder->add("query", VPackSlice(i.entry->data()));
      }
    }
  }

  triggerPollsNoLock(std::move(builder));
  _lowestPromise = std::numeric_limits<index_t>::max();
}

/// Followers' append entries
priv_rpc_ret_t Agent::recvAppendEntriesRPC(term_t term,
                                           std::string const& leaderId,
                                           index_t prevIndex, term_t prevTerm,
                                           index_t leaderCommitIndex,
                                           velocypack::Slice payload) {
  using namespace std::chrono;
  using clock = high_resolution_clock;

  LOG_TOPIC("62f43", DEBUG, Logger::AGENCY)
      << "Got AppendEntriesRPC from " << leaderId << " with term " << term;

  auto start = clock::now();
  term_t t(this->term());
  if (!ready()) {  // We have not been able to put together our configuration
    LOG_TOPIC("7e96c", DEBUG, Logger::AGENCY) << "Agent is not ready yet.";
    return priv_rpc_ret_t(false, t);
  }

  // Update commit index
  if (payload.type() != VPackValueType::Array) {
    LOG_TOPIC("449b2", DEBUG, Logger::AGENCY)
        << "Received malformed entries for appending. Discarding!";
    return priv_rpc_ret_t(false, t);
  }

  size_t nqs = payload.length();
  if (nqs > 0 && !payload[0].get("readDB").isNone()) {
    // We have received a compacted state.
    // Whatever we got in our own state is meaningless now. It is a new world.
    // checkLeader just does plausibility as if it were an empty request
    prevIndex = 0;
    prevTerm = 0;
  }

  // Leadership claim plausibility check
  if (!_constituent.checkLeader(term, leaderId, prevIndex, prevTerm)) {
    LOG_TOPIC("fc654", DEBUG, Logger::AGENCY)
        << "Not accepting appendEntries from " << leaderId;
    return priv_rpc_ret_t(false, t);
  }

  // Empty appendEntries:
  // We answer with success if and only if our highest index is greater 0.
  // Else we want to indicate to the leader that we are behind and need data:
  // a single false will go back and trigger _confirmed[thisfollower] = 0
  if (nqs == 0) {
    auto lastIndex = _state.lastIndex();
    if (lastIndex > 0) {
      LOG_TOPIC("b0b19", DEBUG, Logger::AGENCY)
          << "Finished empty AppendEntriesRPC from " << leaderId
          << " with term " << term;
      {
        WRITE_LOCKER(oLocker, _outputLock);
        auto ci = _commitIndex.load(std::memory_order_relaxed);
        index_t const tmp =
            std::max(ci, std::min(leaderCommitIndex, lastIndex));
        if (tmp > ci) {
          logsForTrigger();
        }
        _commitIndex = tmp;
      }
      return priv_rpc_ret_t(true, t);
    } else {
      return priv_rpc_ret_t(false, t);
    }
  }

  bool ok = true;
  index_t lastIndex = 0;  // Index of last entry in our log
  try {
    lastIndex = _state.logFollower(payload);
    if (lastIndex < payload[nqs - 1].get("index").getNumber<index_t>()) {
      // We could not log all the entries in this query, we need to report
      // this to the leader!
      ok = false;
    }
  } catch (std::exception const& e) {
    LOG_TOPIC("bedb8", DEBUG, Logger::AGENCY)
        << "Exception during log append: " << e.what();
  }

  {
    WRITE_LOCKER(oLocker, _outputLock);
    std::lock_guard guard{_waitForCV.mutex};
    auto ci = _commitIndex.load(std::memory_order_relaxed);
    index_t const tmp = std::max(ci, std::min(leaderCommitIndex, lastIndex));
    if (tmp > ci) {
      logsForTrigger();
    }
    _commitIndex = tmp;
    _local_index = tmp;
    _waitForCV.cv.notify_all();
    if (leaderCommitIndex >= _state.nextCompactionAfter() &&
        payload[nqs - 1].get("index").getNumber<index_t>() >=
            _state.nextCompactionAfter()) {
      _compactor.wakeUp();
    }
  }

  LOG_TOPIC("83504", DEBUG, Logger::AGENCY)
      << "Finished AppendEntriesRPC from " << leaderId << " with term " << term;

  _append_hist_msec.count(
      duration<float, std::milli>(high_resolution_clock::now() - start)
          .count());

  return priv_rpc_ret_t(ok, t);
}

/// Leader's append entries
void Agent::sendAppendEntriesRPC() {
  auto const& nf = server().getFeature<arangodb::NetworkFeature>();
  network::ConnectionPool* cp = nf.pool();

  // _lastSent only accessed in main thread
  std::string const myid = id();

  for (auto const& followerId : _config.active()) {
    if (followerId != myid && leading()) {
      term_t t(term());

      auto startTime = steady_clock::now();
      FollowerData follower = getFollower(followerId).get();
      auto lastConfirmed = follower._lastAckedIndex;

      // We essentially have to send some log entries from their lastConfirmed+1
      // on. However, we have to take care of the case that their lastConfirmed
      // is a value which is very outdated, such that we have in the meantime
      // done a log compaction and do not actually have lastConfirmed+1 any
      // more. In that case, we need to send our latest snapshot at index S
      // (say), and then some log entries from (and including) S on. This is
      // to ensure that the other side does not only have the snapshot, but
      // also the log entry which produced that snapshot.
      // Therefore, we will set lastConfirmed to one less than our latest
      // snapshot in this special case, and we will always fetch enough
      // entries from the log to fulfill our duties.

      if ((steady_clock::now() - follower._earliestPackage).count() < 0 ||
          _state.lastIndex() <= follower._lastAckedIndex) {
        LOG_TOPIC("cfeed", DEBUG, Logger::AGENCY) << "Nothing to append.";
        continue;
      }

      duration<double> lockTime = steady_clock::now() - startTime;
      if (lockTime.count() > 0.1) {
        LOG_TOPIC("b8f60", WARN, Logger::AGENCY)
            << "Reading lastConfirmed took too long: " << lockTime.count();
      }

      index_t commitIndex = _commitIndex.load(std::memory_order_relaxed);

      // If the follower is behind our first log entry send last snapshot and
      // following logs. Else try to have the follower catch up in regular
      // order.
      bool needSnapshot = lastConfirmed < _state.firstIndex();
      if (needSnapshot) {
        lastConfirmed = _state.lastCompactionAt() - 1;
      }

      LOG_TOPIC("7c578", TRACE, Logger::AGENCY)
          << "Getting unconfirmed from " << lastConfirmed << " to "
          << lastConfirmed + 99;
      // If lastConfirmed is one minus the first log entry, then this is
      // corrected in _state::get and we only get from the beginning of the
      // log.
      std::vector<log_t> unconfirmed =
          _state.get(lastConfirmed, lastConfirmed + 99);

      lockTime = steady_clock::now() - startTime;
      if (lockTime.count() > 0.2) {
        LOG_TOPIC("03cb9", WARN, Logger::AGENCY)
            << "Finding unconfirmed entries took too long: "
            << lockTime.count();
      }

      // Note that despite compaction this vector can never be empty, since
      // any compaction keeps at least one active log entry!

      if (unconfirmed.empty()) {
        LOG_TOPIC("0b993", ERR, Logger::AGENCY)
            << "Unexpected empty unconfirmed: "
            << "lastConfirmed=" << lastConfirmed
            << " commitIndex=" << commitIndex;
        TRI_ASSERT(false);
      }

      // Note that if we get here we have at least two entries, otherwise
      // we would have done continue earlier, since this can only happen
      // if lastConfirmed is equal to the last index in our log, in which
      // case there is nothing to replicate.

      duration<double> m = steady_clock::now() - follower._lastSent;

      if (m.count() > _config.minPing() &&
          follower._lastSent.time_since_epoch().count() != 0) {
        LOG_TOPIC("0ddbd", DEBUG, Logger::AGENCY)
            << "Note: sent out last AppendEntriesRPC "
            << "to follower " << followerId
            << " more than minPing ago: " << m.count() << " lastAcked: "
            << duration_cast<duration<double>>(
                   follower._lastAckedTime.time_since_epoch())
                   .count();
      }
      index_t lowest = unconfirmed.front().index;

      Store snapshot("snapshot");
      index_t snapshotIndex;
      term_t snapshotTerm;

      if (lowest > lastConfirmed || needSnapshot) {
        // Ooops, compaction has thrown away so many log entries that
        // we cannot actually update the follower. We need to send our
        // latest snapshot instead:
        bool success = false;
        try {
          success = _state.loadLastCompactedSnapshot(snapshot, snapshotIndex,
                                                     snapshotTerm);
        } catch (std::exception const& e) {
          LOG_TOPIC("f2287", WARN, Logger::AGENCY)
              << "Exception thrown by loadLastCompactedSnapshot: " << e.what();
        }
        if (!success) {
          LOG_TOPIC("6e2b8", WARN, Logger::AGENCY)
              << "Could not load last compacted snapshot, not sending "
                 "appendEntriesRPC!";
          continue;
        }
        if (snapshotTerm == 0) {
          // No shapshot yet
          needSnapshot = false;
        }
      }

      index_t prevLogIndex = unconfirmed.front().index;
      index_t prevLogTerm = unconfirmed.front().term;
      if (needSnapshot) {
        prevLogIndex = snapshotIndex;
        prevLogTerm = snapshotTerm;
      }

      // Body
      VPackBufferUInt8 buffer;
      Builder builder(buffer);
      builder.add(VPackValue(VPackValueType::Array));

      if (needSnapshot) {
        {
          VPackObjectBuilder guard(&builder);
          builder.add(VPackValue("readDB"));
          {
            VPackArrayBuilder guard2(&builder);
            snapshot.dumpToBuilder(builder);
          }
          builder.add("term", VPackValue(snapshotTerm));
          builder.add("index", VPackValue(snapshotIndex));
        }
      }

      size_t toLog = 0;
      index_t highest = 0;
      for (size_t i = 0; i < unconfirmed.size(); ++i) {
        auto const& entry = unconfirmed[i];
        if (entry.index > lastConfirmed) {
          // This condition is crucial, because usually we have one more
          // entry than we need in unconfirmed, so we want to skip this. If,
          // however, we have sent a snapshot, we need to send the log entry
          // with the same index than the snapshot along to retain the
          // invariant of our data structure that the _log in _state is
          // non-empty.
          entry.toVelocyPack(builder);
          highest = entry.index;
          ++toLog;
        }
      }
      builder.close();

      // Really leading?
      if (challengeLeadership()) {
        resign();
        return;
      }

      // Postpone sending the next message for 30 seconds or until an
      // error or successful result occurs.
      auto earliestPackage = steady_clock::now() + std::chrono::seconds(30);
      getFollower(followerId)->_earliestPackage = earliestPackage;
      LOG_TOPIC("99061", DEBUG, Logger::AGENCY)
          << "Setting _earliestPackage to now + 30s for id " << followerId;

      network::RequestOptions reqOpts;
      reqOpts.timeout = network::Timeout(150);
      reqOpts.param("term", std::to_string(t))
          .param("leaderId", id())
          .param("prevLogIndex", std::to_string(prevLogIndex))
          .param("prevLogTerm", std::to_string(prevLogTerm))
          .param("leaderCommit", std::to_string(commitIndex))
          .param("senderTimeStamp",
                 std::to_string(std::llround(steadyClockToDouble() * 1000)));

      // Send request
      auto ac = AgentCallback{this, followerId, highest, toLog};
      network::sendRequest(
          cp, _config.poolAt(followerId), fuerte::RestVerb::Post,
          // cppcheck-suppress accessMoved
          "/_api/agency_priv/appendEntries", std::move(buffer), reqOpts)
          .thenValue(
              [ac = std::move(ac)](network::Response r) { ac.operator()(r); });

      // Note the timeout is relatively long, but due to the 30 seconds
      // above, we only ever have at most 5 messages in flight.

      getFollower(followerId)->_lastSent = steady_clock::now();
      // _constituent.notifyHeartbeatSent(followerId);
      // Do not notify constituent, because the AppendEntriesRPC here could
      // take a very long time, so this must not disturb the empty ones
      // being sent out.

      LOG_TOPIC("2d80d", DEBUG, Logger::AGENCY)
          << "Appending (" << (uint64_t)(TRI_microtime() * 1000000000.0) << ") "
          << unconfirmed.size() - 1 << " entries up to index " << highest
          << (needSnapshot ? " and a snapshot" : "") << " to follower "
          << followerId << ". Next real log contact to " << followerId
          << " in: "
          << std::chrono::duration<double, std::milli>(earliestPackage -
                                                       steady_clock::now())
                 .count()
          << "ms";
    }
  }
}

void Agent::resign(term_t otherTerm) {
  LOG_TOPIC("494a7", DEBUG, Logger::AGENCY)
      << "Resigning in term " << _constituent.term()
      << " because of peer's term " << otherTerm;
  _constituent.follow(otherTerm, NO_LEADER);

  // Wake up all polls with resignation letter
  auto qu = std::make_shared<VPackBuilder>();
  {
    VPackObjectBuilder qb(qu.get());
    qu->add("error", VPackValue(true));
    qu->add("code", VPackValue(TRI_ERROR_HTTP_SERVICE_UNAVAILABLE));
    qu->add(VPackValue("result"));
    VPackArrayBuilder arr(qu.get());
  }

  {
    std::lock_guard lck(_promLock);
    triggerPollsNoLock(std::move(qu));
  }

  endPrepareLeadership();
}

/// Leader's append entries, empty ones for heartbeat, triggered by Constituent
void Agent::sendEmptyAppendEntriesRPC(std::string const& followerId) {
  if (!leading()) {
    LOG_TOPIC("95220", DEBUG, Logger::AGENCY)
        << "Not sending empty appendEntriesRPC to follower " << followerId
        << " because we are no longer leading.";
    return;
  }

  index_t commitIndex = _commitIndex.load(std::memory_order_relaxed);

  auto const& nf = server().getFeature<arangodb::NetworkFeature>();
  network::ConnectionPool* cp = nf.pool();

  // Send request
  VPackBufferUInt8 buffer;
  buffer.append(VPackSlice::emptyArraySlice().begin(), 1);
  auto ac = AgentCallback{this, followerId, 0, 0};

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;
  reqOpts.timeout =
      network::Timeout(3 * _config.minPing() * _config.timeoutMult());
  reqOpts.param("term", std::to_string(_constituent.term()))
      .param("leaderId", id())
      .param("prevLogIndex", "0")
      .param("prevLogTerm", "0")
      .param("leaderCommit", std::to_string(commitIndex))
      .param("senderTimeStamp",
             std::to_string(std::llround(steadyClockToDouble() * 1000)));

  double now = TRI_microtime();
  network::sendRequest(cp, _config.poolAt(followerId), fuerte::RestVerb::Post,
                       // cppcheck-suppress accessMoved
                       "/_api/agency_priv/appendEntries", std::move(buffer),
                       reqOpts)
      .thenValue([=](network::Response r) { ac.operator()(r); });
  double diff = TRI_microtime() - now;
  if (diff > 0.01) {
    LOG_TOPIC("cfb7c", DEBUG, Logger::AGENCY)
        << "Calling network::sendRequest took more than 1/100 of a second: "
        << diff;
  }

  _constituent.notifyHeartbeatSent(followerId);

  now = TRI_microtime();
  LOG_TOPIC("54798", DEBUG, Logger::AGENCY)
      << "Sending empty appendEntriesRPC to follower " << followerId;
  diff = TRI_microtime() - now;
  if (diff > 0.01) {
    LOG_TOPIC("cfb7b", DEBUG, Logger::AGENCY)
        << "Logging of a line took more than 1/100 of a second, this is bad:"
        << diff;
  }
}

void Agent::advanceCommitIndex() {
  // Determine median _confirmed value of followers:
  std::vector<index_t> temp;
  {
    auto guard = _followerData.getLockedGuard();
    for (auto const& follower : guard.get()) {
      temp.push_back(follower.second._lastAckedIndex);
    }
  }

  index_t quorum = size() / 2 + 1;
  if (temp.size() < quorum) {
    LOG_TOPIC("47f8c", WARN, Logger::AGENCY)
        << "_confirmed not populated, quorum: " << quorum << ".";
    return;
  }
  std::sort(temp.begin(), temp.end());
  index_t index = temp[temp.size() - quorum];

  term_t t = _constituent.term();

  auto ci = _commitIndex.load(std::memory_order_relaxed);
  auto slices = _state.slices(ci + 1, index);
  {
    WRITE_LOCKER(oLocker, _outputLock);

    if (index > ci) {
      std::lock_guard guard{_waitForCV.mutex};
      LOG_TOPIC("e24a9", TRACE, Logger::AGENCY)
          << "Critical mass for commiting " << ci + 1 << " through " << index
          << " to read db";

      // Change _readDB and _commitIndex atomically together:
      _readDB.applyLogEntries(slices, ci, t);

      LOG_TOPIC("e24aa", DEBUG, Logger::AGENCY)
          << "Critical mass for commiting " << ci + 1 << " through " << index
          << " to read db, done";

      _commitIndex = index;
      _local_index = index;

      // Wake up write rest handlers:
      _waitForCV.cv.notify_all();

      logsForTrigger();

      if (index >= _state.nextCompactionAfter()) {
        _compactor.wakeUp();
      }
    }
  }
}

std::tuple<futures::Future<query_t>, bool, std::string> Agent::poll(
    index_t index, double timeout) {
  using namespace std::chrono;

  // Please note that the AgencyCache on coordinators and dbservers depends
  // crucially on the behaviour encoded here for correctness at start time.
  // Namely, the AgencyCache must never present the rest of the system with
  // an empty or outdated agency state after a startup, because this could
  // lead to immediate deletion of data. Therefore, it is critical that this
  // code here answers with a current snapshot of the readDB, whenever it is
  // asked for updates since index 0!

  query_t builder;

  std::string leader = _constituent.leaderID();
  if (!loaded() || leader != id()) {
    return std::tuple<futures::Future<query_t>, bool, std::string const&>{
        futures::makeFuture(std::move(builder)), false, std::move(leader)};
  }

  {
    std::unique_lock guard{_waitForCV.mutex};
    while (getPrepareLeadership() != 0 && !isStopping()) {
      _waitForCV.cv.wait_for(guard, std::chrono::microseconds{100});
    }
  }

  {
    READ_LOCKER(oLocker, _outputLock);
    auto ci = _commitIndex.load(std::memory_order_relaxed);
    if (index == 0 || index < _state.firstIndex()) {  // deliver as if index = 0
      builder = std::make_shared<VPackBuilder>();
      VPackObjectBuilder r(builder.get(), /*allowUnindexed*/ true);
      builder->add(VPackValue("result"));
      VPackObjectBuilder r2(builder.get(), /*allowUnindexed*/ true);
      builder->add("commitIndex", VPackValue(ci));
      builder->add("firstIndex", VPackValue(0));
      builder->add(VPackValue("readDB"));
      _readDB.get("", *builder, true);
    } else if (index <= ci) {
      // deliver immediately all logs since index
      builder = std::make_shared<VPackBuilder>();
      VPackObjectBuilder r(builder.get(), /*allowUnindexed*/ true);
      builder->add(VPackValue("result"));
      VPackObjectBuilder r2(builder.get(), /*allowUnindexed*/ true);
      builder->add(VPackValue("log"));
      auto firstIndex = _state.toVelocyPack(*builder, index, ci);
      builder->add("commitIndex", VPackValue(ci));
      builder->add("firstIndex", VPackValue(firstIndex));
    }
  }

  if (builder != nullptr) {
    return std::tuple<futures::Future<query_t>, bool, std::string>{
        futures::makeFuture(std::move(builder)), true, std::string()};
  }

  auto tp = steady_clock::now() +
            duration_cast<milliseconds>(duration<double>(timeout));

  std::lock_guard guard(_promLock);

  try {
    auto res = _promises.emplace(tp, futures::Promise<query_t>());
    if (_lowestPromise > index) {
      _lowestPromise = index;
    }
    return std::tuple<futures::Future<query_t>, bool, std::string>{
        res->second.getFuture(), true, std::string()};
  } catch (...) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "Failed to add promise for polling");
  }
}

/// @brief Activate agency (Inception thread for multi-host, main thread else)
void Agent::activateAgency() {
  _config.activate();
  syncActiveAndAcknowledged();

  try {
    _state.persistActiveAgents(_config.activeToBuilder(),
                               _config.poolToBuilder());
  } catch (std::exception const& e) {
    LOG_TOPIC("6578d", FATAL, Logger::AGENCY)
        << "Failed to persist active agency: " << e.what();
    FATAL_ERROR_EXIT();
  }
}

/// Load persistent state called once
void Agent::load() {
  arangodb::SystemDatabaseFeature::ptr vocbase =
      server().hasFeature<SystemDatabaseFeature>()
          ? server().getFeature<SystemDatabaseFeature>().use()
          : nullptr;
  if (vocbase == nullptr) {
    LOG_TOPIC("63e36", FATAL, Logger::AGENCY)
        << "could not determine _system database";
    FATAL_ERROR_EXIT();
  }

  {
    std::lock_guard guard{_ioLock};  // need this for callback to set _spearhead
    // Note that _state.loadCollections eventually does a callback to the
    // setPersistedState method, which acquires _outputLock and _waitForCV.

    LOG_TOPIC("c07e1", DEBUG, Logger::AGENCY) << "Loading persistent state.";

    if (!_state.loadCollections(vocbase.get(), _config.waitForSync())) {
      LOG_TOPIC("9b680", FATAL, Logger::AGENCY)
          << "Failed to load persistent state on startup.";
      FATAL_ERROR_EXIT();
    }
  }

  // Note that the agent thread is terminated immediately when there is only
  // one agent, since no AppendEntriesRPC have to be issued. Therefore,
  // this thread is almost certainly terminated (and thus isStopping() returns
  // true), when we get here.
  if (isStopping()) {
    return;
  }

  wakeupMainLoop();

  _compactor.start();

  LOG_TOPIC("6e997", DEBUG, Logger::AGENCY) << "Starting spearhead worker.";

  _constituent.start(vocbase.get());
  persistConfiguration(term());

  if (_config.supervision()) {
    LOG_TOPIC("7658f", DEBUG, Logger::AGENCY)
        << "Starting cluster supervision facilities";
    _supervision->start(this);
  }

  _inception->start();
  _loaded = true;
}

/// Still leading? Under MUTEX from ::read or ::write
bool Agent::challengeLeadership() {
  auto followerGuard = _followerData.getLockedGuard();
  size_t good = 0;

  std::string const myid = id();

  for (auto const& follower : followerGuard.get()) {
    auto i = follower.second._lastEmptyAcked;
    if (follower.first != myid) {  // do not count ourselves
      duration<double> m = steady_clock::now() - i;
      LOG_TOPIC("22f78", DEBUG, Logger::AGENCY)
          << "challengeLeadership: found "
             "_lastAcked["
          << follower.first << "] to be " << m.count()
          << " seconds in the past.";

      // This is rather arbitrary here: We used to have 0.9 here to absolutely
      // ensure that a leader resigns before another one even starts an
      // election. However, the Raft paper does not mention this at all. Rather,
      // in the paper it is written that the leader should resign immediately if
      // it sees a higher term from another server. Currently we have not
      // implemented to return the follower's term with a response to
      // AppendEntriesRPC, so the leader cannot find out a higher term this way.
      // The leader can, however, see a higher term in the incoming
      // AppendEntriesRPC a new leader sends out, and it will immediately resign
      // if it sees that. For the moment, this value here can stay. We should
      // soon implement sending the follower's term back with each response and
      // probably get rid of this method altogether, but this requires a bit
      // more thought.
      if (_config.maxPing() * _config.timeoutMult() > m.count()) {
        ++good;
      }
    }
  }
  LOG_TOPIC("0e75d", DEBUG, Logger::AGENCY)
      << "challengeLeadership: good=" << good;

  return (good < size() / 2);  // not counting myself
}

/// Get last acknowledged responses on leader
void Agent::lastAckedAgo(Builder& ret) const {
  std::unordered_map<std::string, FollowerData> followerData;
  index_t lastCompactionAt, nextCompactionAfter;

  {
    followerData = _followerData.copy();
    lastCompactionAt = _state.lastCompactionAt();
    nextCompactionAfter = _state.nextCompactionAfter();
  }

  auto dur2str = [&](std::string const& key,
                     SteadyTimePoint const& time) -> double {
    return id() == key
               ? 0.0
               : 1.0e-3 *
                     std::floor(
                         duration<double>(steady_clock::now() - time).count() *
                         1.0e3);
  };

  ret.add("lastCompactionAt", VPackValue(lastCompactionAt));
  ret.add("nextCompactionAfter", VPackValue(nextCompactionAfter));

  if (leading()) {
    ret.add(VPackValue("lastAcked"));
    VPackObjectBuilder b(&ret);
    for (auto const& [followerId, follower] : followerData) {
      // Note that it is possible that a server is already in lastAcked
      // but not yet in lastSent, since lastSent only has times of non-empty
      // appendEntriesRPC calls, but we also get lastAcked entries for the
      // empty ones.
      ret.add(VPackValue(followerId));
      {
        VPackObjectBuilder o(&ret);
        ret.add("lastAckedTime",
                VPackValue(dur2str(followerId, follower._lastAckedTime)));
        ret.add("lastAckedIndex", VPackValue(follower._lastAckedIndex));
        if (followerId != id()) {
          if (follower._lastSent != SteadyTimePoint{}) {
            ret.add("lastAppend",
                    VPackValue(dur2str(followerId, follower._lastSent)));
          } else {
            ret.add("lastAppend",
                    VPackValue(dur2str(followerId, follower._lastAckedTime)));
            // This is just for the above mentioned case, which will very
            // soon be rectified.
          }
        }
      }
    }
  }
}

trans_ret_t Agent::transact(velocypack::Slice qs) {
  arangodb::consensus::index_t maxind = 0;  // maximum write index

  // Note that we are leading (_constituent.leading()) if and only
  // if _constituent.leaderId == our own ID. Therefore, we do not have
  // to use leading() or _constituent.leading() here, but can simply
  // look at the leaderID.
  auto leader = _constituent.leaderID();
  if (leader != id()) {
    return trans_ret_t(false, std::move(leader));
  }

  {
    std::unique_lock guard{_waitForCV.mutex};
    while (getPrepareLeadership() != 0 && !isStopping()) {
      _waitForCV.cv.wait_for(guard, std::chrono::microseconds{100});
    }
  }

  // Apply to spearhead and get indices for log entries
  addTrxsOngoing(qs);  // remember that these are ongoing
  size_t failed;
  auto ret = std::make_shared<arangodb::velocypack::Builder>();
  {
    auto sg = arangodb::scopeGuard([&]() noexcept { removeTrxsOngoing(qs); });
    // Note that once the transactions are in our log, we can remove them
    // from the list of ongoing ones, although they might not yet be committed.
    // This is because then, inquire will find them in the log and draw its
    // own conclusions. The map of ongoing trxs is only to cover the time
    // from when we receive the request until we have appended the trxs
    // ourselves.
    failed = 0;
    ret->openArray();
    // Only leader else redirect
    if (challengeLeadership()) {
      resign();
      return trans_ret_t(false, NO_LEADER);
    }

    term_t currentTerm = term();  // this is the term we will be working with

    // Check that we are actually still the leader:
    if (!leading()) {
      return trans_ret_t(false, NO_LEADER);
    }

    std::lock_guard ioLocker{_ioLock};

    for (auto query : VPackArrayIterator(qs)) {
      // Check that we are actually still the leader:
      if (!leading()) {
        return trans_ret_t(false, NO_LEADER);
      }
      if (query[0].isObject()) {
        check_ret_t res = _spearhead.applyTransaction(query);
        if (res.successful()) {
          maxind = (query.length() == 3 && query[2].isString())
                       ? _state.logLeaderSingle(query[0], currentTerm,
                                                query[2].copyString())
                       : _state.logLeaderSingle(query[0], currentTerm);
          ret->add(VPackValue(maxind));
        } else {
          _spearhead.read(res.failed->slice(), *ret);
          ++failed;
        }
      } else if (query[0].isString()) {
        _spearhead.read(query, *ret);
      }
    }
    ret->close();
  }

  // Report that leader has persisted
  reportIn(id(), maxind);

  return trans_ret_t(true, id(), maxind, failed, std::move(ret));
}

// Non-persistent write to non-persisted key-value store
trans_ret_t Agent::transient(velocypack::Slice queries) {
  // Note that we are leading (_constituent.leading()) if and only
  // if _constituent.leaderId == our own ID. Therefore, we do not have
  // to use leading() or _constituent.leading() here, but can simply
  // look at the leaderID.
  auto leader = _constituent.leaderID();
  if (leader != id()) {
    return trans_ret_t(false, std::move(leader));
  }

  {
    std::unique_lock guard{_waitForCV.mutex};
    while (getPrepareLeadership() != 0 && !isStopping()) {
      _waitForCV.cv.wait_for(guard, std::chrono::microseconds{100});
    }
  }

  auto ret = std::make_shared<arangodb::velocypack::Builder>();

  // Apply to _transient and get indices for log entries
  {
    VPackArrayBuilder b(ret.get());

    // Only leader else redirect
    if (challengeLeadership()) {
      resign();
      return trans_ret_t(false, NO_LEADER);
    }

    std::lock_guard transientLocker{_transientLock};

    // Read and writes
    for (auto query : VPackArrayIterator(queries)) {
      if (query[0].isObject()) {
        ret->add(VPackValue(_transient.applyTransaction(query).successful()));
      } else if (query[0].isString()) {
        _transient.read(query, *ret);
      }
    }
  }

  return trans_ret_t(true, id(), 0, 0, std::move(ret));
}

write_ret_t Agent::inquire(velocypack::Slice query) {
  // Note that we are leading (_constituent.leading()) if and only
  // if _constituent.leaderId == our own ID. Therefore, we do not have
  // to use leading() or _constituent.leading() here, but can simply
  // look at the leaderID.
  auto leader = _constituent.leaderID();
  if (leader != id()) {
    return write_ret_t(false, std::move(leader));
  }

  write_ret_t ret;

  while (true) {
    // Check ongoing ones:
    bool found = false;
    for (VPackSlice s : VPackArrayIterator(query)) {
      std::string ss = s.copyString();
      if (isTrxOngoing(ss)) {
        found = true;
        break;
      }
    }
    if (!found) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(0.1));
    leader = _constituent.leaderID();
    if (leader != id()) {
      return write_ret_t(false, std::move(leader));
    }
  }

  std::lock_guard ioLocker{_ioLock};

  ret.indices = _state.inquire(query);

  ret.accepted = true;

  return ret;
}

bool Agent::loaded() const { return _loaded.load(); }

/// Write new entries to replicated state and store
write_ret_t Agent::write(velocypack::Slice query, WriteMode const& wmode) {
  using namespace std::chrono;
  std::vector<apply_ret_t> applied;
  std::vector<index_t> indices;

  // Note that we are leading (_constituent.leading()) if and only
  // if _constituent.leaderId == our own ID. Therefore, we do not have
  // to use leading() or _constituent.leading() here, but can simply
  // look at the leaderID.
  auto leader = _constituent.leaderID();
  if ((!loaded() && wmode != WriteMode(true, true)) || leader != id()) {
    ++_write_no_leader;
    return write_ret_t(false, std::move(leader));
  }

  if (!wmode.discardStartup()) {
    std::unique_lock guard{_waitForCV.mutex};
    while (getPrepareLeadership() != 0 && !isStopping()) {
      _waitForCV.cv.wait_for(guard, std::chrono::microseconds{100});
    }
  }

  {
    addTrxsOngoing(query);  // remember that these are ongoing
    auto sg =
        arangodb::scopeGuard([&]() noexcept { removeTrxsOngoing(query); });
    // Note that once the transactions are in our log, we can remove them
    // from the list of ongoing ones, although they might not yet be committed.
    // This is because then, inquire will find them in the log and draw its
    // own conclusions. The map of ongoing trxs is only to cover the time
    // from when we receive the request until we have appended the trxs
    // ourselves.

    size_t ntrans = query.length();
    size_t npacks = ntrans / _config.maxAppendSize();
    if (ntrans % _config.maxAppendSize() != 0) {
      npacks++;
    }

    term_t currentTerm = term();  // this is the term we will be working with

    // Check that we are actually still the leader:
    if (!leading()) {
      ++_write_no_leader;
      return write_ret_t(false, NO_LEADER);
    }

    auto const start = high_resolution_clock::now();
    // Apply to spearhead and get indices for log entries
    // Avoid keeping lock indefinitely
    VPackBuilder chunk;
    for (size_t i = 0, l = 0; i < npacks; ++i) {
      chunk.clear();
      {
        VPackArrayBuilder b(&chunk);
        for (size_t j = 0; j < _config.maxAppendSize() && l < ntrans;
             ++j, ++l) {
          chunk.add(query.at(l));
        }
      }

      // Only leader else redirect
      if (challengeLeadership()) {
        resign();
        ++_write_no_leader;
        return write_ret_t(false, NO_LEADER);
      }

      // Check that we are actually still the leader:
      if (!leading()) {
        ++_write_no_leader;
        return write_ret_t(false, NO_LEADER);
      }

      std::lock_guard ioLocker{_ioLock};

      applied = _spearhead.applyTransactions(chunk.slice(), wmode);
      auto tmp = _state.logLeaderMulti(chunk.slice(), applied, currentTerm);
      indices.insert(indices.end(), tmp.begin(), tmp.end());
    }
    _write_hist_msec.count(
        duration<float, std::milli>(high_resolution_clock::now() - start)
            .count());
  }

  // Maximum log index
  index_t maxind = 0;
  if (!indices.empty()) {
    maxind = *std::max_element(indices.begin(), indices.end());
  }

  // Report that leader has persisted
  reportIn(id(), maxind);

  ++_write_ok;
  return write_ret_t(true, id(), std::move(applied), std::move(indices));
}

/// Read from store
read_ret_t Agent::read(velocypack::Slice query) {
  // Note that we are leading (_constituent.leading()) if and only
  // if _constituent.leaderId == our own ID. Therefore, we do not have
  // to use leading() or _constituent.leading() here, but can simply
  // look at the leaderID.
  auto leader = _constituent.leaderID();
  if (!loaded() || leader != id()) {
    ++_read_no_leader;
    return read_ret_t(false, std::move(leader));
  }

  {
    std::unique_lock guard{_waitForCV.mutex};
    while (getPrepareLeadership() != 0 && !isStopping()) {
      _waitForCV.cv.wait_for(guard, std::chrono::microseconds{100});
    }
  }

  // Only leader else redirect
  if (challengeLeadership()) {
    resign();
    ++_read_no_leader;
    return read_ret_t(false, NO_LEADER);
  }

  leader = _constituent.leaderID();

  auto result = std::make_shared<arangodb::velocypack::Builder>();

  READ_LOCKER(oLocker, _outputLock);

  // Retrieve data from readDB
  std::vector<bool> success = _readDB.readMultiple(query, *result);

  ++_read_ok;
  return read_ret_t(true, std::move(leader), std::move(success),
                    std::move(result));
}

// trigger all polls, who have timed out with empty result
void Agent::clearExpiredPolls() {
  index_t commitIndex = _commitIndex.load(std::memory_order_relaxed);
  auto empty = std::make_shared<VPackBuilder>();
  {
    VPackObjectBuilder obj(empty.get());
    empty->add(VPackValue("result"));
    VPackObjectBuilder res(empty.get());
    empty->add("firstIndex", VPackValue(commitIndex));
    empty->add("commitIndex", VPackValue(commitIndex));
    empty->add(VPackValue("log"));
    VPackArrayBuilder a(empty.get());
  }

  std::lock_guard lck(_promLock);
  triggerPollsNoLock(std::move(empty), std::chrono::steady_clock::now());
}

/// Clear expired polls
/// Wake up everybody with query and delete with empty.
/// If qu is nullptr, we're resigning.
void Agent::triggerPollsNoLock(query_t qu, SteadyTimePoint const& tp) {
  auto* scheduler = SchedulerFeature::SCHEDULER;
  auto pit = _promises.begin();
  while (pit != _promises.end()) {
    if (pit->first < tp) {
      auto pp =
          std::make_shared<futures::Promise<query_t>>(std::move(pit->second));
      scheduler->queue(RequestLane::CLUSTER_INTERNAL,
                       [pp = std::move(pp), qu] { pp->setValue(qu); });
      pit = _promises.erase(pit);
    } else {
      ++pit;
    }
  }
}

/// Send out append entries to followers regularly or on event
void Agent::run() {
  while (!this->isStopping()) {
    try {
      {
        // We set the variable to false here, if any change happens during
        // or after the calls in this loop, this will be set to true to
        // indicate no sleeping. Any change will happen under the mutex.
        std::lock_guard guard{_appendCV.mutex};
        _agentNeedsWakeup = false;
      }

      if (leading() && getPrepareLeadership() == 1) {
        // If we are officially leading but the _preparing flag is set, we
        // are in the process of preparing for leadership. This flag is
        // set when the Constituent celebrates an election victory. Here,
        // in the main thread, we do the actual preparations:

        if (!prepareLead()) {
          _constituent.follow(0);  // do not change _term or _votedFor
        } else {
          // we need to start work as leader
          lead();
        }

        donePrepareLeadership();  // we are ready to roll, except that we
        // have to wait for the _commitIndex to
        // reach the end of our log
      }

      // Clear expired long polls
      clearExpiredPolls();

      // Leader working only
      if (leading()) {
        if (1 == getPrepareLeadership()) {
          // Skip the usual work and the waiting such that above preparation
          // code runs immediately. We will return with value 2 such that
          // replication and confirmation of it can happen. Service will
          // continue once _commitIndex has reached the end of the log and
          // then getPrepareLeadership() will finally return 0.
          continue;
        }

        // Challenge leadership.
        // Let's proactively know, that we no longer lead instead of finding
        // out through read/write.
        if (challengeLeadership()) {
          resign();
          continue;
        }

        // Append entries to followers
        sendAppendEntriesRPC();

        // Check whether we can advance _commitIndex
        advanceCommitIndex();

        bool commenceService = false;
        {
          READ_LOCKER(oLocker, _outputLock);
          if (leading() && getPrepareLeadership() == 2 &&
              _commitIndex.load(std::memory_order_relaxed) ==
                  _state.lastIndex()) {
            commenceService = true;
          }
        }

        if (commenceService) {
          std::lock_guard ioLocker{_ioLock};
          READ_LOCKER(oLocker, _outputLock);
          _spearhead = _readDB;
          endPrepareLeadership();  // finally service can commence
        }

        // Go to sleep some:
        {
          std::unique_lock guard{_appendCV.mutex};
          if (!_agentNeedsWakeup) {
            // wait up to minPing():
            _appendCV.cv.wait_for(
                guard, std::chrono::microseconds{
                           static_cast<uint64_t>(1.0e6 * _config.minPing())});
            // We leave minPing here without the multiplier to run this
            // loop often enough in cases of high load.
          }
        }
      } else {
        std::unique_lock guard{_appendCV.mutex};
        if (!_agentNeedsWakeup) {
          _appendCV.cv.wait_for(guard, std::chrono::seconds{1});
        }
      }
    } catch (std::exception const& e) {
      LOG_TOPIC("70efa", WARN, Logger::AGENCY)
          << "Caught exception in multi-host agent thread " << e.what();
    }
  }
}

void Agent::persistConfiguration(term_t t) {
  // Agency configuration
  velocypack::Builder agency;
  {
    VPackArrayBuilder trxs(&agency);
    {
      VPackArrayBuilder trx(&agency);
      {
        VPackObjectBuilder oper(&agency);
        agency.add(VPackValue(RECONFIGURE));
        {
          VPackObjectBuilder a(&agency);
          agency.add("op", VPackValue("set"));
          agency.add(VPackValue("new"));
          {
            VPackObjectBuilder aa(&agency);
            agency.add("term", VPackValue(t));
            agency.add(config_t::idStr, VPackValue(id()));
            agency.add(config_t::activeStr, _config.activeToBuilder()->slice());
            agency.add(config_t::poolStr, _config.poolToBuilder()->slice());
            agency.add("size", VPackValue(size()));
            agency.add(config_t::timeoutMultStr,
                       VPackValue(_config.timeoutMult()));
          }
        }
      }
    }
  }

  {
    // Make sure we have setup the local list of lastAckedIndex
    // only containing the leader
    auto follower = _followerData.getLockedGuard();
    if (follower->find(id()) == follower->end()) {
      follower->emplace(id(),
                        FollowerData{._lastAckedTime = steady_clock::now(),
                                     ._lastAckedIndex = 0});
    }
  }
  // In case we've lost leadership, no harm will arise as the failed write
  // prevents bogus agency configuration to be replicated among agents. ***
  write(agency.slice(), WriteMode(true, true));
}

/// Orderly shutdown
void Agent::beginShutdown() {
  Thread::beginShutdown();

  // Stop constituent and key value stores
  _constituent.beginShutdown();

  // Stop supervision
  if (_config.supervision()) {
    _supervision->beginShutdown();
  }

  // Stop inception process
  if (_inception != nullptr) {  // resilient agency only
    _inception->beginShutdown();
  }

  // Compactor
  _compactor.beginShutdown();

  // Wake up all waiting rest handlers
  {
    std::lock_guard guard{_waitForCV.mutex};
    _waitForCV.cv.notify_all();
  }

  // Wake up run
  wakeupMainLoop();
}

bool Agent::prepareLead() {
  {
    // Erase _earliestPackage, which allows for immediate sending of
    // AppendEntriesRPC when we become a leader.
    auto follower = _followerData.getLockedGuard();
    for (auto& [id, data] : follower.get()) {
      data._earliestPackage = SteadyTimePoint{};
    }
  }

  {
    // Clear transient for supervision start
    std::lock_guard transientLocker{_transientLock};
    _transient.clear();
  }

  // Key value stores
  try {
    rebuildDBs();
  } catch (std::exception const& e) {
    LOG_TOPIC("aa3cd", ERR, Logger::AGENCY)
        << "Failed to rebuild key value stores." << e.what();
    return false;
  }

  // Reset last acknowledged
  syncActiveAndAcknowledged();

  return true;
}

/// Becoming leader
void Agent::lead() {
  {
    // We cannot start sendAppendentries before first log index.
    // Any missing indices before _commitIndex were compacted.
    // DO NOT EDIT without understanding the consequences for sendAppendEntries!
    index_t commitIndex = _commitIndex.load(std::memory_order_relaxed);

    auto follower = _followerData.getLockedGuard();
    for (auto& [followerId, data] : follower.get()) {
      if (followerId != id()) {
        data._lastAckedIndex = commitIndex;
        data._lastEmptyAcked = steady_clock::now();
      }
    }
  }

  // Agency configuration
  term_t myterm;
  myterm = _constituent.term();

  persistConfiguration(myterm);

  // This is all right now, in the main loop we will wait until a
  // majority of all servers have replicated this configuration.
  // Then we will copy the _readDB to the _spearhead and start service.
}

// How long back did I take over leadership, result in seconds
int64_t Agent::leaderFor() const {
  return std::chrono::duration_cast<std::chrono::duration<int64_t>>(
             std::chrono::steady_clock::now().time_since_epoch())
             .count() -
         _leaderSince;
}

void Agent::updatePeerEndpoint(velocypack::Slice message) {
  if (!message.isObject() || message.length() == 0) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_AGENCY_INFORM_MUST_BE_OBJECT,
        std::string("Improper greeting: ") + message.toJson());
  }

  std::string uuid;
  try {
    uuid = message.keyAt(0).copyString();
  } catch (std::exception const& e) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_AGENCY_INFORM_MUST_BE_OBJECT,
        std::string("Cannot deal with UUID: ") + e.what());
  }

  std::string endpoint;
  try {
    endpoint = message.valueAt(0).copyString();
  } catch (std::exception const& e) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_AGENCY_INFORM_MUST_BE_OBJECT,
        std::string("Cannot deal with UUID: ") + e.what());
  }

  updatePeerEndpoint(uuid, endpoint);
}

bool Agent::addGossipPeer(std::string const& endpoint) {
  return _config.addGossipPeer(endpoint);
}

void Agent::updatePeerEndpoint(std::string const& id, std::string const& ep) {
  if (_config.updateEndpoint(id, ep)) {
    if (!challengeLeadership()) {
      persistConfiguration(term());
    }
  }
}

void Agent::notify(velocypack::Slice message) {
  if (!message.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_AGENCY_INFORM_MUST_BE_OBJECT,
        std::string("Inform message must be an object. Incoming type is ") +
            message.typeName());
  }

  if (!message.get("id").isString()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_AGENCY_INFORM_MUST_CONTAIN_ID);
  }
  if (!message.hasKey("term")) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_AGENCY_INFORM_MUST_CONTAIN_TERM);
  }
  _constituent.update(message.get("id").copyString(),
                      message.get("term").getUInt());

  if (!message.get("active").isArray()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_AGENCY_INFORM_MUST_CONTAIN_ACTIVE);
  }
  if (!message.get("pool").isObject()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_AGENCY_INFORM_MUST_CONTAIN_POOL);
  }
  if (!message.get("min ping").isNumber()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_AGENCY_INFORM_MUST_CONTAIN_MIN_PING);
  }
  if (!message.get("max ping").isNumber()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_AGENCY_INFORM_MUST_CONTAIN_MAX_PING);
  }
  if (!message.get("timeoutMult").isInteger()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_AGENCY_INFORM_MUST_CONTAIN_TIMEOUT_MULT);
  }

  _config.update(message);

  _state.persistActiveAgents(_config.activeToBuilder(),
                             _config.poolToBuilder());
  // update causes the list of peers to change potentially
  syncActiveAndAcknowledged();
}

// Rebuild key value stores
void Agent::rebuildDBs() {
  term_t term = _constituent.term();

  std::lock_guard ioLocker{_ioLock};
  WRITE_LOCKER(oLocker, _outputLock);
  std::lock_guard guard{_waitForCV.mutex};

  index_t lastCompactionIndex;

  // We must go back to clean sheet
  _readDB.clear();
  _spearhead.clear();

  if (!_state.loadLastCompactedSnapshot(_readDB, lastCompactionIndex, term)) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_AGENCY_CANNOT_REBUILD_DBS);
  }

  _commitIndex = lastCompactionIndex;
  _local_index = lastCompactionIndex;
  _waitForCV.cv.notify_all();

  // Apply logs from last applied index to leader's commit index
  LOG_TOPIC("b12cb", DEBUG, Logger::AGENCY)
      << "Rebuilding key-value stores from index " << lastCompactionIndex
      << " to " << _commitIndex.load(std::memory_order_relaxed) << " "
      << _state;

  {
    auto logs = _state.slices(lastCompactionIndex + 1,
                              _commitIndex.load(std::memory_order_relaxed));
    _readDB.applyLogEntries(logs, _commitIndex.load(std::memory_order_relaxed),
                            term);
  }
  _spearhead = _readDB;

  LOG_TOPIC("a66dc", INFO, Logger::AGENCY)
      << id() << " rebuilt key-value stores - serving.";
}

/// Compact read db
void Agent::compact() {
  // We do not allow the case _config.compactionKeepSize() == 0, since
  // we need to keep a part of the recent log. Therefore we cannot use
  // the _readDB ever, since we have to compute a state of the key/value
  // space well before _commitIndex anyway. Apart from this, the compaction
  // code runs on the followers as well where we do not have a _readDB
  // anyway.
  index_t commitIndex = _commitIndex.load(std::memory_order_relaxed);

  using namespace std::chrono;
  using clock = std::chrono::high_resolution_clock;

  if (commitIndex >= _state.nextCompactionAfter()) {
    // This check needs to be here, because the compactor thread wakes us
    // up every 5 seconds.
    // Note that it is OK to compact anywhere before or at _commitIndex.
    auto const start = clock::now();
    if (!_state.compact(commitIndex, _config.compactionKeepSize())) {
      LOG_TOPIC("70234", WARN, Logger::AGENCY)
          << "Compaction for index " << commitIndex << " with keep size "
          << _config.compactionKeepSize() << " did not work.";
    } else {
      _compaction_hist_msec.count(
          duration<float, std::milli>(clock::now() - start).count());
    }
  }
}

/// Last commit index
arangodb::consensus::index_t Agent::lastCommitted() const {
  return _commitIndex.load(std::memory_order_relaxed);
}

/// Last log entry
log_t Agent::lastLog() const { return _state.lastLog(); }

/// Get spearhead
Store const& Agent::spearhead() const { return _spearhead; }

/// Get _readDB reference with intentionally no lock acquired here.
///   Safe ONLY IF via executeLock() (see example Supervisor.cpp)
Store const& Agent::readDB() const { return _readDB; }

/// Get readdb
arangodb::consensus::index_t Agent::readDB(VPackBuilder& builder) const {
  TRI_ASSERT(builder.isOpenObject());

  uint64_t commitIndex = 0;

  {
    READ_LOCKER(oLocker, _outputLock);

    commitIndex = _commitIndex.load(std::memory_order_relaxed);
    // commit index
    builder.add("index", VPackValue(commitIndex));
    builder.add("term", VPackValue(term()));

    // key-value store {}
    builder.add(VPackValue("agency"));
    _readDB.get("", builder, true);
  }

  // replicated log []
  _state.toVelocyPack(commitIndex, builder);

  return commitIndex;
}

void Agent::executeLockedRead(std::function<void()> const& cb) {
  std::lock_guard ioLocker{_ioLock};
  READ_LOCKER(oLocker, _outputLock);
  cb();
}

#if 0
// currently not called from anywhere
void Agent::executeLockedWrite(std::function<void()> const& cb) {
  std::lock_guard ioLocker{_ioLock};
  WRITE_LOCKER(oLocker, _outputLock);
  std::lock_guard guard{_waitForCV.mutex};
  cb();
}
#endif

void Agent::executeTransientLocked(std::function<void()> const& cb) {
  std::lock_guard transientLocker{_transientLock};
  cb();
}

/// Get transient
/// intentionally no lock is acquired here, so we can return
/// a const reference
/// the caller has to make sure the lock is actually held
Store const& Agent::transient() const { return _transient; }

/// Rebuild from persisted state
void Agent::setPersistedState(VPackSlice compaction) {
  // Catch up with compacted state, this is only called at startup
  _spearhead.setNodeValue(compaction);

  // Catch up with commit
  try {
    WRITE_LOCKER(oLocker, _outputLock);
    std::lock_guard guard{_waitForCV.mutex};
    _readDB.setNodeValue(compaction);
    _commitIndex = arangodb::basics::StringUtils::uint64(
        compaction.get(StaticStrings::KeyString).copyString());
    _local_index = _commitIndex.load(std::memory_order_relaxed);
    _waitForCV.cv.notify_all();
  } catch (std::exception const& e) {
    LOG_TOPIC("70844", ERR, Logger::AGENCY) << e.what();
  }
}

/// We expect an object as follows {id:<id>,endpoint:<endpoint>,pool:{...}}
/// key: uuid value: endpoint
/// Lock configuration and compare
/// Add whatever is missing in our list.
/// Compare whatever is in our list already. (ASSERT identity)
/// If I know more immediately contact peer with my list.
query_t Agent::gossip(VPackSlice slice, bool isCallback, size_t version) {
  LOG_TOPIC("1ae7b", DEBUG, Logger::AGENCY)
      << "Incoming gossip: " << slice.toJson();

  if (!slice.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_AGENCY_MALFORMED_GOSSIP_MESSAGE,
        std::string("Gossip message must be an object. Incoming type is ") +
            slice.typeName());
  }

  if (slice.hasKey(StaticStrings::Error)) {
    if (slice.get(StaticStrings::Code).getNumber<int>() == 403) {
      LOG_TOPIC("6591b", FATAL, Logger::AGENCY)
          << "Gossip peer does not have us in their pool " << slice.toJson();
      FATAL_ERROR_EXIT();  /// We don't belong here
    } else {
      LOG_TOPIC("949bb", DEBUG, Logger::AGENCY)
          << "Received gossip error. We'll retry " << slice.toJson();
    }
    query_t out = std::make_shared<Builder>();
    return out;
  }

  if (!slice.hasKey("id") || !slice.get("id").isString()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_AGENCY_MALFORMED_GOSSIP_MESSAGE,
        "Gossip message must contain string parameter 'id'");
  }
  std::string id = slice.get("id").copyString();

  // If pool is complete and id not in our pool reject under all circumstances
  if (_config.poolComplete() && !_config.findInPool(id)) {
    query_t ret = std::make_shared<VPackBuilder>();
    {
      VPackObjectBuilder o(ret.get());
      ret->add(StaticStrings::Code, VPackValue(403));
      ret->add(StaticStrings::Error, VPackValue(true));
      ret->add(StaticStrings::ErrorMessage,
               VPackValue("This agents is not member of this pool"));
      ret->add(StaticStrings::ErrorNum, VPackValue(403));
    }
    return ret;
  }

  if (!slice.hasKey("endpoint") || !slice.get("endpoint").isString()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_AGENCY_MALFORMED_GOSSIP_MESSAGE,
        "Gossip message must contain string parameter 'endpoint'");
  }
  std::string endpoint = slice.get("endpoint").copyString();

  if (_inception != nullptr && isCallback) {
    _inception->reportVersionForEp(endpoint, version);
  }

  LOG_TOPIC("9d2d9", TRACE, Logger::AGENCY)
      << "Gossip " << ((isCallback) ? "callback" : "call") << " from "
      << endpoint;

  if (!slice.hasKey("pool") || !slice.get("pool").isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_AGENCY_MALFORMED_GOSSIP_MESSAGE,
        "Gossip message must contain object parameter 'pool'");
  }
  VPackSlice pslice = slice.get("pool");

  LOG_TOPIC("65dd8", TRACE, Logger::AGENCY)
      << "Received gossip " << slice.toJson();
  for (auto pair : VPackObjectIterator(pslice)) {
    if (!pair.value.isString()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_AGENCY_MALFORMED_GOSSIP_MESSAGE,
          "Gossip message pool must contain string parameters");
    }
  }

  query_t out = std::make_shared<Builder>();

  {
    VPackObjectBuilder b(out.get());

    std::unordered_set<std::string> gossipPeers = _config.gossipPeers();
    if (!gossipPeers.empty() && !isCallback) {
      try {
        _config.eraseGossipPeer(endpoint);
      } catch (std::exception const& e) {
        LOG_TOPIC("58f08", ERR, Logger::AGENCY) << e.what();
      }
    }

    std::string err;
    config_t::upsert_t upsert = config_t::UNCHANGED;

    /// Pool incomplete or the other guy is in my pool: I'll gossip.
    if (!_config.poolComplete() || _config.matchPeer(id, endpoint)) {
      upsert = _config.upsertPool(pslice, id);
      if (upsert == config_t::WRONG) {
        LOG_TOPIC("32973", FATAL, Logger::AGENCY)
            << "Discrepancy in agent pool!";
        FATAL_ERROR_EXIT();  /// disagreement over pool membership are fatal!
      }

      // Wrapped in envelope in RestAgencyPrivHandler
      auto pool = _config.pool();
      out->add(VPackValue("pool"));
      {
        VPackObjectBuilder bb(out.get());
        for (auto const& i : pool) {
          out->add(i.first, VPackValue(i.second));
        }
      }

    } else {  // Pool complete & id's endpoint not matching.

      // Not leader: redirect / 503
      if (challengeLeadership()) {
        out->add("redirect", VPackValue(true));
        out->add("id", VPackValue(leaderID()));
      } else {  // leader magic
        auto tmp = _config;
        tmp.upsertPool(pslice, id);

        velocypack::Builder query;
        {
          VPackArrayBuilder trs(&query);
          {
            VPackArrayBuilder tr(&query);
            {
              VPackObjectBuilder o(&query);
              query.add(VPackValue(RECONFIGURE));
              {
                VPackObjectBuilder o(&query);
                query.add("op", VPackValue("set"));
                query.add(VPackValue("new"));
                {
                  VPackObjectBuilder c(&query);
                  tmp.toBuilder(query);
                }
              }
            }
          }
        }

        LOG_TOPIC("e85f0", DEBUG, Logger::AGENCY)
            << "persisting new agency configuration via RAFT: "
            << query.toJson();

        // Do write
        write_ret_t ret;
        try {
          ret = write(query.slice(), WriteMode(false, true));
          arangodb::consensus::index_t max_index = 0;
          if (ret.indices.size() > 0) {
            max_index =
                *std::max_element(ret.indices.begin(), ret.indices.end());
          }
          if (max_index >
              0) {  // We have a RAFT index. Wait for the RAFT commit.
            auto result = waitFor(max_index);
            if (result != Agent::raft_commit_t::OK) {
              err =
                  "failed to retrieve RAFT index for updated agency endpoints";
            } else {
              auto pool = _config.pool();
              out->add(VPackValue("pool"));
              {
                VPackObjectBuilder bb(out.get());
                for (auto const& i : pool) {
                  out->add(i.first, VPackValue(i.second));
                }
              }
            }
          } else {
            err = "failed to retrieve RAFT index for updated agency endpoints";
          }
        } catch (std::exception const& e) {
          err = std::string("failed to write new agency to RAFT") + e.what();
          LOG_TOPIC("17dc2", ERR, Logger::AGENCY) << err;
        }
      }

      if (!err.empty()) {
        out->add(StaticStrings::Code, VPackValue(500));
        out->add(StaticStrings::Error, VPackValue(true));
        out->add(StaticStrings::ErrorMessage, VPackValue(err));
        out->add(StaticStrings::ErrorNum, VPackValue(500));
      }
    }

    // let gossip loop know that it has new data
    if (_inception != nullptr && upsert == config_t::CHANGED) {
      _inception->signalConditionVar();
    }
  }

  if (!isCallback) {
    LOG_TOPIC("1e95f", TRACE, Logger::AGENCY)
        << "Answering with gossip " << out->slice().toJson();
  }

  return out;
}

void Agent::resetRAFTTimes(double minTimeout, double maxTimeout) {
  _config.pingTimes(minTimeout, maxTimeout);
}

void Agent::ready(bool b) {
  // From main thread of Inception
  _ready = b;
}

bool Agent::ready() const { return _ready; }

query_t Agent::buildDB(arangodb::consensus::index_t index) {
  Store store;
  index_t oldIndex;
  term_t term;
  if (!_state.loadLastCompactedSnapshot(store, oldIndex, term)) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_AGENCY_CANNOT_REBUILD_DBS);
  }

  {
    READ_LOCKER(oLocker, _outputLock);
    auto ci = _commitIndex.load(std::memory_order_relaxed);
    if (index > ci) {
      LOG_TOPIC("88754", INFO, Logger::AGENCY)
          << "Cannot snapshot beyond leaderCommitIndex: " << ci;
      index = ci;
    } else if (index < oldIndex) {
      LOG_TOPIC("cb67b", INFO, Logger::AGENCY)
          << "Cannot snapshot before last compaction index: " << oldIndex;
      index = oldIndex;
    }
  }

  {
    if (index > oldIndex) {
      auto logs = _state.slices(oldIndex + 1, index);
      store.applyLogEntries(logs, index, term);
    } else {
      VPackBuilder logs;
      logs.openArray();
      logs.close();
      store.applyLogEntries(logs, index, term);
    }
  }

  auto builder = std::make_shared<VPackBuilder>();
  store.toBuilder(*builder);

  return builder;
}

void Agent::addTrxsOngoing(Slice trxs) {
  try {
    std::lock_guard guard{_trxsLock};
    for (auto const& trx : VPackArrayIterator(trxs)) {
      if (trx.isArray() && trx.length() == 3 && trx[0].isObject() &&
          trx[2].isString()) {
        // only those are interesting:
        _ongoingTrxs.insert(trx[2].copyString());
      }
    }
  } catch (...) {
  }
}

void Agent::removeTrxsOngoing(Slice trxs) noexcept {
  try {
    std::lock_guard guard{_trxsLock};
    for (auto const& trx : VPackArrayIterator(trxs)) {
      if (trx.isArray() && trx.length() == 3 && trx[0].isObject() &&
          trx[2].isString()) {
        // only those are interesting:
        _ongoingTrxs.erase(trx[2].copyString());
      }
    }
  } catch (...) {
  }
}

bool Agent::isTrxOngoing(std::string const& id) const noexcept {
  std::lock_guard guard{_trxsLock};
  auto it = _ongoingTrxs.find(id);
  return it != _ongoingTrxs.end();
}

Inception const* Agent::inception() const { return _inception.get(); }

void Agent::updateConfiguration(Slice slice) {
  _config.updateConfiguration(slice);
  // updateConfiguration causes the list of peers to change potentially
  syncActiveAndAcknowledged();
}

void Agent::updateSomeConfigValues(velocypack::Slice data) {
  if (!data.isObject()) {
    return;
  }
  double d;
  VPackSlice slice = data.get("okThreshold");
  if (slice.isNumber()) {
    d = slice.getNumber<double>();
    LOG_TOPIC("12341", DEBUG, Logger::SUPERVISION)
        << "Updating okThreshold to " << d;
    _config.setSupervisionOkThreshold(d);
    _supervision->setOkThreshold(d);
  }
  slice = data.get("gracePeriod");
  if (slice.isNumber()) {
    d = slice.getNumber<double>();
    LOG_TOPIC("12342", DEBUG, Logger::SUPERVISION)
        << "Updating gracePeriod to " << d;
    _config.setSupervisionGracePeriod(d);
    _supervision->setGracePeriod(d);
  }
  slice = data.get("delayAddFollower");
  uint64_t u;
  if (slice.isNumber()) {
    u = slice.getNumber<uint64_t>();
    LOG_TOPIC("12343", DEBUG, Logger::SUPERVISION)
        << "Updating delayAddFollower to " << u;
    _config.setSupervisionDelayAddFollower(u);
    _supervision->setDelayAddFollower(u);
  }
  slice = data.get("delayFailedFollower");
  if (slice.isNumber()) {
    u = slice.getNumber<uint64_t>();
    LOG_TOPIC("12344", DEBUG, Logger::SUPERVISION)
        << "Updating delayFailedFollower to " << u;
    _config.setSupervisionDelayFailedFollower(u);
    _supervision->setDelayFailedFollower(u);
  }
  slice = data.get("failedLeaderAddsFollower");
  if (slice.isBool()) {
    bool b = slice.getBool();
    LOG_TOPIC("12345", DEBUG, Logger::SUPERVISION)
        << "Updating failedLeaderAddsFollower to " << b;
    _config.setSupervisionFailedLeaderAddsFollower(b);
    _supervision->setFailedLeaderAddsFollower(b);
  }
}

std::vector<log_t> Agent::logs(index_t begin, index_t end) const {
  return _state.get(begin, end);
}

void Agent::syncActiveAndAcknowledged() {
  // We reset the list of last Acknowledged indexes, to contain
  // at least every peer. If there is a new peer it will be inserted
  // with lastAckknowledged NOW for index 0.
  {
    auto follower = _followerData.getLockedGuard();
    // The number of Agents is small, so we can afford to always scan linearly
    // here
    for (auto const& peer : _config.active()) {
      if (follower->find(peer) == follower->end()) {
        follower->emplace(peer,
                          FollowerData{._lastAckedTime = steady_clock::now(),
                                       ._lastAckedIndex = 0});
      }
    }
  }
}

MutexGuard<Agent::FollowerData, std::unique_lock<std::mutex>>
Agent::getFollower(std::string const& followerId) {
  auto guard = _followerData.getLockedGuard();
  auto it = guard->find(followerId);
  TRI_ASSERT(it != guard->end());
  return MutexGuard(it->second, std::move(guard));
}

}  // namespace consensus
}  // namespace arangodb
