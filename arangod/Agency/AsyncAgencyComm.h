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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <fuerte/message.h>

#include <deque>
#include <memory>
#include <mutex>

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include "Agency/AgencyComm.h"

#include "Agency/PathComponent.h"
#include "Basics/ResultT.h"
#include "Basics/debugging.h"
#include "Futures/Future.h"
#include "Network/Methods.h"
#include "Network/Utils.h"
#include "AgencyCommon.h"

namespace arangodb {

struct AsyncAgencyCommResult {
  arangodb::fuerte::Error error;
  std::unique_ptr<arangodb::fuerte::Response> response;

  [[nodiscard]] bool ok() const {
    return arangodb::fuerte::Error::NoError == this->error;
  }

  [[nodiscard]] bool fail() const { return !ok(); }

  VPackSlice slice() const {
    TRI_ASSERT(response != nullptr);
    return response->slice();
  }

  std::shared_ptr<velocypack::Buffer<uint8_t>> copyPayload() const {
    TRI_ASSERT(response != nullptr);
    return response->copyPayload();
  }

  std::shared_ptr<velocypack::Buffer<uint8_t>> stealPayload() const {
    TRI_ASSERT(response != nullptr);
    return response->stealPayload();
  }

  std::string payloadAsString() const {
    TRI_ASSERT(response != nullptr);
    return response->payloadAsString();
  }

  std::size_t payloadSize() const {
    TRI_ASSERT(response != nullptr);
    return response->payloadSize();
  }

  arangodb::fuerte::StatusCode statusCode() const {
    TRI_ASSERT(response != nullptr);
    return response->statusCode();
  }

  [[nodiscard]] Result asResult() const {
    using namespace arangodb::network;
    if (!ok()) {
      return Result{fuerteToArangoErrorCode(error), to_string(error)};
    } else {
      auto code = statusCode();
      auto internalCode = fuerteStatusToArangoErrorCode(code);
      if (internalCode == TRI_ERROR_NO_ERROR) {
        return Result(internalCode);
      }
      return Result{internalCode, fuerteStatusToArangoErrorMessage(code)};
    }
  }
};

// Work around a spurious compiler warning in GCC 9.3 with our maintainer mode
// switched off. And since warnings are considered to be errors, we must
// switch the warning off:

#if defined(__GNUC__) && \
    (__GNUC__ > 9 || (__GNUC__ == 9 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

struct AgencyReadResult : public AsyncAgencyCommResult {
  AgencyReadResult(
      AsyncAgencyCommResult&& result,
      std::shared_ptr<arangodb::cluster::paths::Path const> valuePath)
      : AsyncAgencyCommResult(std::move(result)),
        _value(nullptr),
        _valuePath(std::move(valuePath)) {}
  VPackSlice value() {
    if (this->_value.start() == nullptr) {
      this->_value = slice().at(0).get(_valuePath->vec());
    }
    return this->_value;
  }

 private:
  VPackSlice _value;
  std::shared_ptr<arangodb::cluster::paths::Path const> _valuePath;
};

#if defined(__GNUC__) && \
    (__GNUC__ > 9 || (__GNUC__ == 9 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic pop
#endif

class AsyncAgencyComm;

class AsyncAgencyCommManager final {
 public:
  static std::unique_ptr<AsyncAgencyCommManager> INSTANCE;

  static void initialize(ArangodServer& server) {
    INSTANCE = std::make_unique<AsyncAgencyCommManager>(server);
  }

  static bool isEnabled() { return INSTANCE != nullptr; }
  static AsyncAgencyCommManager& getInstance();

  explicit AsyncAgencyCommManager(ArangodServer&);

  void addEndpoint(std::string const& endpoint);
  void updateEndpoints(std::vector<std::string> const& endpoints);

  std::deque<std::string> endpoints() const {
    std::unique_lock<std::mutex> guard(_lock);
    return _endpoints;
  }

  std::string endpointsString() const;
  auto getSkipScheduler() const -> bool { return _skipScheduler; };
  void setSkipScheduler(bool v) { _skipScheduler = v; };

  std::string getCurrentEndpoint();
  void reportError(std::string const& endpoint);
  void reportRedirect(std::string const& endpoint,
                      std::string const& redirectTo);

  network::ConnectionPool* pool() const { return _pool; }
  void pool(network::ConnectionPool* pool) { _pool = pool; }

  ArangodServer& server();

  uint64_t nextRequestId() {
    return _nextRequestId.fetch_add(1, std::memory_order_relaxed);
  }

  bool isStopping() const { return _isStopping; }
  void setStopping(bool stopping) { _isStopping = stopping; }

 private:
  std::atomic<bool> _isStopping = false;
  std::atomic<bool> _skipScheduler = true;
  ArangodServer& _server;
  mutable std::mutex _lock;
  std::deque<std::string> _endpoints;
  network::ConnectionPool* _pool = nullptr;

  std::atomic<uint64_t> _nextRequestId = 0;
};

struct SetTransientOptions {
  bool skipScheduler = false;
  network::Timeout timeout = std::chrono::seconds{20};
};

class AsyncAgencyComm final {
 public:
  using FutureResult = arangodb::futures::Future<AsyncAgencyCommResult>;
  using FutureReadResult = arangodb::futures::Future<AgencyReadResult>;

  [[nodiscard]] FutureResult getValues(
      std::string const& path,
      std::optional<network::Timeout> timeout = {}) const;
  [[nodiscard]] FutureReadResult getValues(
      std::shared_ptr<arangodb::cluster::paths::Path const> const& path,
      std::optional<network::Timeout> timeout = {}) const;
  [[nodiscard]] FutureResult poll(network::Timeout timeout,
                                  uint64_t index) const;

  [[nodiscard]] futures::Future<consensus::index_t> getCurrentCommitIndex()
      const;

  template<typename T>
  [[nodiscard]] FutureResult setValue(
      network::Timeout timeout,
      std::shared_ptr<arangodb::cluster::paths::Path const> const& path,
      T const& value, uint64_t ttl = 0) {
    return setValue(timeout, path->str(), value, ttl);
  }

  template<typename T>
  [[nodiscard]] FutureResult setValue(network::Timeout timeout,
                                      std::string const& path, T const& value,
                                      uint64_t ttl = 0) {
    VPackBuffer<uint8_t> transaction;
    {
      VPackBuilder trxBuilder(transaction);
      VPackArrayBuilder env(&trxBuilder);
      {
        VPackArrayBuilder trx(&trxBuilder);
        {
          VPackObjectBuilder ops(&trxBuilder);
          {
            VPackObjectBuilder op(&trxBuilder, path);
            trxBuilder.add("op", VPackValue("set"));
            trxBuilder.add("new", value);
            if (ttl > 0) {
              trxBuilder.add("ttl", VPackValue(ttl));
            }
          }
        }
      }
    }

    return sendWriteTransaction(timeout, std::move(transaction));
  }

  [[nodiscard]] FutureResult deleteKey(
      network::Timeout timeout,
      std::shared_ptr<arangodb::cluster::paths::Path const> const& path) const;
  [[nodiscard]] FutureResult deleteKey(network::Timeout timeout,
                                       std::string const& path) const;

  [[nodiscard]] FutureResult sendWriteTransaction(
      network::Timeout timeout, velocypack::Buffer<uint8_t>&& body) const;
  [[nodiscard]] FutureResult sendReadTransaction(
      network::Timeout timeout, velocypack::Buffer<uint8_t>&& body) const;
  [[nodiscard]] FutureResult sendPollTransaction(network::Timeout timeout,
                                                 uint64_t index) const;

  [[nodiscard]] FutureResult sendTransaction(
      network::Timeout timeout, AgencyReadTransaction const&) const;
  [[nodiscard]] FutureResult sendTransaction(
      network::Timeout timeout, AgencyWriteTransaction const&) const;

  [[nodiscard]] FutureResult setTransientValue(
      std::string const& key, arangodb::velocypack::Slice const& slice,
      SetTransientOptions const& opts = {});

  enum class RequestType {
    READ,    // send the transaction again in the case of no response
    WRITE,   // does not send the transaction again but instead tries to do
             // inquiry with the given ids
    CUSTOM,  // talk to the leader and always return the result, even on timeout
             // or redirect
  };

  using ClientId = std::string;

  [[nodiscard]] FutureResult sendWithFailover(
      arangodb::fuerte::RestVerb method, std::string const& url,
      network::Timeout timeout, RequestType type,
      std::vector<ClientId> clientIds,
      velocypack::Buffer<uint8_t>&& body) const;

  [[nodiscard]] FutureResult sendWithFailover(
      arangodb::fuerte::RestVerb method, std::string const& url,
      network::Timeout timeout, RequestType type,
      std::vector<ClientId> clientIds, AgencyTransaction const& trx) const;

  [[nodiscard]] FutureResult sendWithFailover(
      arangodb::fuerte::RestVerb method, std::string const& url,
      network::Timeout timeout, RequestType type,
      velocypack::Buffer<uint8_t>&& body) const;

  [[nodiscard]] FutureResult sendWithFailover(arangodb::fuerte::RestVerb method,
                                              std::string const& url,
                                              network::Timeout timeout,
                                              RequestType type,
                                              uint64_t index) const;

  AsyncAgencyComm() : _manager(AsyncAgencyCommManager::getInstance()) {}
  explicit AsyncAgencyComm(AsyncAgencyCommManager& manager)
      : _manager(manager) {}

  auto withSkipScheduler(bool v) -> AsyncAgencyComm& {
    _skipScheduler = v;
    return *this;
  }

 private:
  bool _skipScheduler = false;
  AsyncAgencyCommManager& _manager;
};

}  // namespace arangodb
