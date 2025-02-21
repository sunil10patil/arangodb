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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Cluster/ServerState.h"
#include "Metrics/Fwd.h"
#include "RestServer/arangod.h"
#include "SimpleHttpClient/ConnectionCache.h"

struct TRI_vocbase_t;

namespace arangodb {
namespace application_features {
class ApplicationServer;
}

class GeneralResponse;
class GlobalReplicationApplier;

class ReplicationFeature final : public ArangodFeature {
 public:
  static constexpr std::string_view name() noexcept { return "Replication"; }

  explicit ReplicationFeature(Server& server);
  ~ReplicationFeature();

  void collectOptions(
      std::shared_ptr<options::ProgramOptions> options) override final;
  void validateOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void prepare() override final;
  void start() override final;
  void beginShutdown() override final;
  void stop() override final;
  void unprepare() override final;

  httpclient::ConnectionCache& connectionCache();

  /// @brief return a pointer to the global replication applier
  GlobalReplicationApplier* globalReplicationApplier() const;

  /// @brief disable replication appliers
  void disableReplicationApplier();

  /// @brief start the replication applier for a single database
  void startApplier(TRI_vocbase_t* vocbase);

  /// @brief stop the replication applier for a single database
  void stopApplier(TRI_vocbase_t* vocbase);

  /// @brief returns the connect timeout for replication requests
  double connectTimeout() const;

  /// @brief returns the request timeout for replication requests
  double requestTimeout() const;

  double activeFailoverLeaderGracePeriod() const;

  /// @brief returns the connect timeout for replication requests
  /// this will return the provided value if the user has not adjusted the
  /// timeout via configuration. otherwise it will return the configured
  /// timeout value
  double checkConnectTimeout(double value) const;

  /// @brief returns the request timeout for replication requests
  /// this will return the provided value if the user has not adjusted the
  /// timeout via configuration. otherwise it will return the configured
  /// timeout value
  double checkRequestTimeout(double value) const;

  /// @brief automatic failover of replication using the agency
  bool isActiveFailoverEnabled() const;

  bool syncByRevision() const noexcept;

  bool autoRepairRevisionTrees() const noexcept;

#ifdef ARANGODB_USE_GOOGLE_TESTS
  // only used during testing
  void autoRepairRevisionTrees(bool value) noexcept;
#endif

  /// @brief track the number of (parallel) tailing operations
  /// will throw an exception if the number of concurrently running operations
  /// would exceed the configured maximum
  void trackTailingStart();

  /// @brief count down the number of parallel tailing operations
  /// must only be called after a successful call to trackTailingstart
  void trackTailingEnd() noexcept;

  void trackInventoryRequest() noexcept;

  /// @brief set the x-arango-endpoint header
  void setEndpointHeader(GeneralResponse*, arangodb::ServerState::Mode);

  /// @brief fill a response object with correct response for a follower
  void prepareFollowerResponse(GeneralResponse*, arangodb::ServerState::Mode);

  /// @brief get max document num for quick call to _api/replication/keys to get
  /// actual keys or only doc count
  uint64_t quickKeysLimit() const { return _quickKeysLimit; }

  /// @brief return a reference to the "number of clients" metric
  metrics::Gauge<uint64_t>& clientsMetric() { return _clients; }

 private:
  /// @brief connection timeout for replication requests
  double _connectTimeout;

  /// @brief request timeout for replication requests
  double _requestTimeout;

  /// @brief  amount of time (in seconds) for which the current leader will
  /// continue to assume its leadership even if it lost connection to the
  /// agency (0 = unlimited)
  double _activeFailoverLeaderGracePeriod;

  /// @brief whether or not the user-defined connect timeout is forced to be
  /// used this is true only if the user set the connect timeout at startup
  bool _forceConnectTimeout;

  /// @brief whether or not the user-defined request timeout is forced to be
  /// used this is true only if the user set the request timeout at startup
  bool _forceRequestTimeout;

  bool _replicationApplierAutoStart;

  /// Enable the active failover
  bool _enableActiveFailover;

  /// Use the revision-based replication protocol
  bool _syncByRevision;

  /// automatically repair revision trees of shards after too many failed
  /// shard synchronization attempts
  bool _autoRepairRevisionTrees;

  /// @brief cache for reusable connections
  httpclient::ConnectionCache _connectionCache;

  /// @brief number of currently operating tailing operations
  std::atomic<uint64_t> _parallelTailingInvocations;

  /// @brief maximum number of parallel tailing operations invocations
  uint64_t _maxParallelTailingInvocations;

  std::unique_ptr<GlobalReplicationApplier> _globalReplicationApplier;

  /// @brief quick replication keys limit
  uint64_t _quickKeysLimit;

  metrics::Counter& _inventoryRequests;

  /// @brief number of currently active clients
  metrics::Gauge<uint64_t>& _clients;
};

}  // namespace arangodb
