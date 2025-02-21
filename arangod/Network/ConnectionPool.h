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

#pragma once

#include <fuerte/types.h>
#include "Basics/Common.h"
#include "Metrics/Fwd.h"

namespace arangodb {
namespace fuerte {
inline namespace v1 {
class Connection;
class ConnectionBuilder;
class EventLoopService;
}  // namespace v1
}  // namespace fuerte
class ClusterInfo;

namespace network {

class ConnectionPtr;

/// @brief simple connection pool managing fuerte connections
#ifdef ARANGODB_USE_GOOGLE_TESTS
class ConnectionPool {
#else
class ConnectionPool final {
#endif
 protected:
  struct Connection;
  friend class ConnectionPtr;

 public:
  struct Config {
    metrics::MetricsFeature& metricsFeature;
    // note: clusterInfo can remain a nullptr in unit tests
    ClusterInfo* clusterInfo = nullptr;
    uint64_t maxOpenConnections = 1024;     /// max number of connections
    uint64_t idleConnectionMilli = 120000;  /// unused connection lifetime
    unsigned int numIOThreads = 1;          /// number of IO threads
    bool verifyHosts = false;
    fuerte::ProtocolType protocol = fuerte::ProtocolType::Http;
    // name must remain valid for the lifetime of the Config object.
    char const* name = "";

    Config(metrics::MetricsFeature& metricsFeature)
        : metricsFeature(metricsFeature) {}
  };

 public:
  explicit ConnectionPool(ConnectionPool::Config const& config);
  TEST_VIRTUAL ~ConnectionPool();

  /// @brief request a connection for a specific endpoint
  /// note: it is the callers responsibility to ensure the endpoint
  /// is always the same, we do not do any post-processing
  ConnectionPtr leaseConnection(std::string const& endpoint, bool& isFromPool);

  /// @brief event loop service to create a connection seperately
  /// user is responsible for correctly shutting it down
  fuerte::EventLoopService& eventLoopService();

  /// @brief shutdown all connections
  void drainConnections();

  /// @brief shutdown all connections
  void shutdownConnections();

  /// @brief automatically prune connections
  void pruneConnections();

  /// @brief cancel connections to this endpoint
  size_t cancelConnections(std::string const& endpoint);

  /// @brief return the number of open connections
  size_t numOpenConnections() const;

  Config const& config() const;

 protected:
  struct Context;

  /// @brief endpoint bucket
  struct Bucket;

  TEST_VIRTUAL std::shared_ptr<fuerte::Connection> createConnection(
      fuerte::ConnectionBuilder&);

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

class ConnectionPtr {
 public:
  ConnectionPtr(std::shared_ptr<ConnectionPool::Context> context);
  ConnectionPtr(ConnectionPtr&& ctx) noexcept;
  ConnectionPtr(ConnectionPtr const&) = delete;
  ~ConnectionPtr();

  ConnectionPtr operator=(ConnectionPtr&&) = delete;
  ConnectionPtr operator=(ConnectionPtr const&) = delete;

  fuerte::Connection& operator*() const;
  fuerte::Connection* operator->() const;
  fuerte::Connection* get() const;

 private:
  std::shared_ptr<ConnectionPool::Context> _context;
};

}  // namespace network
}  // namespace arangodb
