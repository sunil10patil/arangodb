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

#include "NetworkFeature.h"

#include <fuerte/connection.h>

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/FunctionUtils.h"
#include "Basics/ScopeGuard.h"
#include "Basics/application-exit.h"
#include "Basics/debugging.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "Network/ConnectionPool.h"
#include "Network/Methods.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "Metrics/CounterBuilder.h"
#include "Metrics/FixScale.h"
#include "Metrics/GaugeBuilder.h"
#include "Metrics/HistogramBuilder.h"
#include "Metrics/MetricsFeature.h"
#include "Scheduler/SchedulerFeature.h"

namespace {
void queueGarbageCollection(std::mutex& mutex,
                            arangodb::Scheduler::WorkHandle& workItem,
                            std::function<void(bool)>& gcfunc,
                            std::chrono::seconds offset) {
  std::lock_guard<std::mutex> guard(mutex);
  workItem = arangodb::SchedulerFeature::SCHEDULER->queueDelayed(
      "networkfeature-gc", arangodb::RequestLane::INTERNAL_LOW, offset, gcfunc);
}

constexpr double CongestionRatio = 0.5;
constexpr std::uint64_t MaxAllowedInFlight = 65536;
constexpr std::uint64_t MinAllowedInFlight = 64;
}  // namespace

using namespace arangodb::basics;
using namespace arangodb::options;

namespace arangodb {

NetworkFeature::NetworkFeature(Server& server)
    : NetworkFeature(server,
                     network::ConnectionPool::Config{
                         server.getFeature<metrics::MetricsFeature>()}) {
  static_assert(
      Server::isCreatedAfter<NetworkFeature, metrics::MetricsFeature>());
  this->_numIOThreads = 2;  // override default
}

struct NetworkFeatureScale {
  static metrics::FixScale<double> scale() {
    return {0.0, 100.0, {1.0, 5.0, 15.0, 50.0}};
  }
};

struct NetworkFeatureSendScaleSmall {
  static metrics::FixScale<double> scale() {
    return {0.0, 10.0, {0.000001, 0.00001, 0.0001, 0.001, 0.01, 0.1, 1.0}};
  }
};

struct NetworkFeatureSendScaleLarge {
  static metrics::FixScale<double> scale() {
    return {0.0, 10000.0, {0.01, 0.1, 1.0, 10.0, 100.0, 1000.0}};
  }
};

DECLARE_COUNTER(arangodb_network_forwarded_requests_total,
                "Number of requests forwarded to another coordinator");
DECLARE_COUNTER(arangodb_network_request_timeouts_total,
                "Number of internal requests that have timed out");
DECLARE_HISTOGRAM(
    arangodb_network_request_duration_as_percentage_of_timeout,
    NetworkFeatureScale,
    "Internal request round-trip time as a percentage of timeout [%]");
DECLARE_COUNTER(arangodb_network_unfinished_sends_total,
                "Number of times the sending of a request remained unfinished");
DECLARE_HISTOGRAM(
    arangodb_network_dequeue_duration, NetworkFeatureSendScaleSmall,
    "Time to dequeue a queued network request in fuerte in seconds");
DECLARE_HISTOGRAM(arangodb_network_send_duration, NetworkFeatureSendScaleLarge,
                  "Time to send out internal requests in seconds");
DECLARE_HISTOGRAM(
    arangodb_network_response_duration, NetworkFeatureSendScaleLarge,
    "Time to wait for network response after it was sent out in seconds");
DECLARE_GAUGE(arangodb_network_requests_in_flight, uint64_t,
              "Number of outgoing internal requests in flight");

NetworkFeature::NetworkFeature(Server& server,
                               network::ConnectionPool::Config config)
    : ArangodFeature{server, *this},
      _protocol(),
      _maxOpenConnections(config.maxOpenConnections),
      _idleTtlMilli(config.idleConnectionMilli),
      _numIOThreads(config.numIOThreads),
      _verifyHosts(config.verifyHosts),
      _prepared(false),
      _forwardedRequests(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_network_forwarded_requests_total{})),
      _maxInFlight(::MaxAllowedInFlight),
      _requestsInFlight(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_network_requests_in_flight{})),
      _requestTimeouts(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_network_request_timeouts_total{})),
      _requestDurations(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_network_request_duration_as_percentage_of_timeout{})),
      _unfinishedSends(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_network_unfinished_sends_total{})),
      _dequeueDurations(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_network_dequeue_duration{})),
      _sendDurations(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_network_send_duration{})),
      _responseDurations(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_network_response_duration{})) {
  setOptional(true);
  startsAfter<ClusterFeature>();
  startsAfter<SchedulerFeature>();
  startsAfter<ServerFeature>();
  startsAfter<EngineSelectorFeature>();
}

void NetworkFeature::collectOptions(
    std::shared_ptr<options::ProgramOptions> options) {
  options->addSection("network", "cluster-internal networking");

  options
      ->addOption("--network.io-threads",
                  "The number of network I/O threads for cluster-internal "
                  "communication.",
                  new UInt32Parameter(&_numIOThreads))
      .setIntroducedIn(30600);
  options
      ->addOption("--network.max-open-connections",
                  "The maximum number of open TCP connections for "
                  "cluster-internal communication per endpoint",
                  new UInt64Parameter(&_maxOpenConnections))
      .setIntroducedIn(30600);
  options
      ->addOption("--network.idle-connection-ttl",
                  "The default time-to-live of idle connections for "
                  "cluster-internal communication (in milliseconds).",
                  new UInt64Parameter(&_idleTtlMilli))
      .setIntroducedIn(30600);
  options
      ->addOption("--network.verify-hosts",
                  "Verify peer certificates when using TLS in cluster-internal "
                  "communication.",
                  new BooleanParameter(&_verifyHosts))
      .setIntroducedIn(30600);

  std::unordered_set<std::string> protos = {"", "http", "http2", "h2", "vst"};

  // starting with 3.9, we will hard-code the protocol for cluster-internal
  // communication
  options
      ->addOption(
          "--network.protocol",
          "The network protocol to use for cluster-internal communication.",
          new DiscreteValuesParameter<StringParameter>(&_protocol, protos),
          options::makeDefaultFlags(options::Flags::Uncommon))
      .setIntroducedIn(30700)
      .setDeprecatedIn(30900);

  options
      ->addOption("--network.max-requests-in-flight",
                  "The number of internal requests that can be in "
                  "flight at a given point in time.",
                  new options::UInt64Parameter(&_maxInFlight),
                  options::makeDefaultFlags(options::Flags::Uncommon))
      .setIntroducedIn(30800);
}

void NetworkFeature::validateOptions(
    std::shared_ptr<options::ProgramOptions> opts) {
  _numIOThreads = std::max<unsigned>(1, std::min<unsigned>(_numIOThreads, 8));
  if (_maxOpenConnections < 8) {
    _maxOpenConnections = 8;
  }
  if (!opts->processingResult().touched("--network.idle-connection-ttl")) {
    auto& gs = server().getFeature<GeneralServerFeature>();
    _idleTtlMilli = uint64_t(gs.keepAliveTimeout() * 1000 / 2);
  }
  if (_idleTtlMilli < 10000) {
    _idleTtlMilli = 10000;
  }

  uint64_t clamped =
      std::clamp(_maxInFlight, ::MinAllowedInFlight, ::MaxAllowedInFlight);
  if (clamped != _maxInFlight) {
    LOG_TOPIC("38cd1", WARN, Logger::CONFIG)
        << "Must set --network.max-requests-in-flight between "
        << ::MinAllowedInFlight << " and " << ::MaxAllowedInFlight
        << ", clamping value";
    _maxInFlight = clamped;
  }
}

void NetworkFeature::prepare() {
  ClusterInfo* ci = nullptr;
  if (server().hasFeature<ClusterFeature>() &&
      server().isEnabled<ClusterFeature>()) {
    // in unit tests the ClusterInfo may not be enabled.
    ci = &server().getFeature<ClusterFeature>().clusterInfo();
  }

  network::ConnectionPool::Config config(
      server().getFeature<metrics::MetricsFeature>());
  config.numIOThreads = static_cast<unsigned>(_numIOThreads);
  config.maxOpenConnections = _maxOpenConnections;
  config.idleConnectionMilli = _idleTtlMilli;
  config.verifyHosts = _verifyHosts;
  config.clusterInfo = ci;
  config.name = "ClusterComm";

  // using an internal network protocol other than HTTP/1 is
  // not supported since 3.9. the protocol is always hard-coded
  // to HTTP/1 from now on.
  // note: we plan to upgrade the internal protocol to HTTP/2 at
  // some point in the future
  config.protocol = fuerte::ProtocolType::Http;
  if (_protocol == "http" || _protocol == "h1") {
    config.protocol = fuerte::ProtocolType::Http;
  } else if (_protocol == "http2" || _protocol == "h2") {
    config.protocol = fuerte::ProtocolType::Http2;
  } else if (_protocol == "vst") {
    config.protocol = fuerte::ProtocolType::Vst;
  }

  if (config.protocol != fuerte::ProtocolType::Http) {
    LOG_TOPIC("6d221", WARN, Logger::CONFIG)
        << "using `--network.protocol` is deprecated. "
        << "the network protocol for cluster-internal requests is hard-coded "
           "to HTTP/1 in this version";
    config.protocol = fuerte::ProtocolType::Http;
  }

  _pool = std::make_unique<network::ConnectionPool>(config);
  _poolPtr.store(_pool.get(), std::memory_order_relaxed);

  _gcfunc = [this, ci](bool canceled) {
    if (canceled) {
      return;
    }

    _pool->pruneConnections();

    if (ci != nullptr) {
      auto failed = ci->getFailedServers();
      for (ServerID const& srvId : failed) {
        std::string endpoint = ci->getServerEndpoint(srvId);
        size_t n = _pool->cancelConnections(endpoint);
        LOG_TOPIC_IF("15d94", INFO, Logger::COMMUNICATION, n > 0)
            << "canceling " << n << " connection(s) to failed server '" << srvId
            << "' on endpoint '" << endpoint << "'";
      }
    }

    if (!server().isStopping() && !canceled) {
      std::chrono::seconds off(12);
      ::queueGarbageCollection(_workItemMutex, _workItem, _gcfunc, off);
    }
  };

  _prepared = true;
}

void NetworkFeature::start() {
  Scheduler* scheduler = SchedulerFeature::SCHEDULER;
  if (scheduler != nullptr) {  // is nullptr in catch tests
    auto off = std::chrono::seconds(1);
    ::queueGarbageCollection(_workItemMutex, _workItem, _gcfunc, off);
  }
}

void NetworkFeature::beginShutdown() {
  {
    std::lock_guard<std::mutex> guard(_workItemMutex);
    _workItem.reset();
    for (auto const& [req, item] : _retryRequests) {
      TRI_ASSERT(item != nullptr);
      item->cancel();
    }
    _retryRequests.clear();
  }
  _poolPtr.store(nullptr, std::memory_order_relaxed);
  if (_pool) {  // first cancel all connections
    _pool->shutdownConnections();
  }
}

void NetworkFeature::stop() {
  {
    // we might have posted another workItem during shutdown.
    std::lock_guard<std::mutex> guard(_workItemMutex);
    _workItem.reset();
    for (auto const& [req, item] : _retryRequests) {
      TRI_ASSERT(item != nullptr);
      item->cancel();
    }
    _retryRequests.clear();
  }
  if (_pool) {
    _pool->shutdownConnections();
  }
}

void NetworkFeature::unprepare() {
  if (_pool) {
    _pool->drainConnections();
  }
}

network::ConnectionPool* NetworkFeature::pool() const noexcept {
  return _poolPtr.load(std::memory_order_relaxed);
}

#ifdef ARANGODB_USE_GOOGLE_TESTS
void NetworkFeature::setPoolTesting(network::ConnectionPool* pool) {
  _poolPtr.store(pool, std::memory_order_release);
}
#endif

bool NetworkFeature::prepared() const noexcept { return _prepared; }

void NetworkFeature::trackForwardedRequest() noexcept { ++_forwardedRequests; }

std::size_t NetworkFeature::requestsInFlight() const noexcept {
  return _requestsInFlight.load();
}

bool NetworkFeature::isCongested() const noexcept {
  return _requestsInFlight.load() >= (_maxInFlight * ::CongestionRatio);
}

bool NetworkFeature::isSaturated() const noexcept {
  return _requestsInFlight.load() >= _maxInFlight;
}

void NetworkFeature::sendRequest(network::ConnectionPool& pool,
                                 network::RequestOptions const& options,
                                 std::string const& endpoint,
                                 std::unique_ptr<fuerte::Request>&& req,
                                 RequestCallback&& cb) {
  TRI_ASSERT(req != nullptr);
  prepareRequest(pool, req);
  bool isFromPool = false;
  auto now = std::chrono::steady_clock::now();
  auto conn = pool.leaseConnection(endpoint, isFromPool);
  auto dur = std::chrono::steady_clock::now() - now;
  if (dur > std::chrono::seconds(1)) {
    LOG_TOPIC("52418", WARN, Logger::COMMUNICATION)
        << "have leased connection to '" << endpoint
        << "' came from pool: " << isFromPool << " leasing took "
        << std::chrono::duration_cast<std::chrono::duration<double>>(dur)
               .count()
        << " seconds, url: " << to_string(req->header.restVerb) << " "
        << req->header.path << ", request ptr: " << (void*)req.get();
  } else {
    LOG_TOPIC("52417", TRACE, Logger::COMMUNICATION)
        << "have leased connection to '" << endpoint
        << "' came from pool: " << isFromPool
        << ", url: " << to_string(req->header.restVerb) << " "
        << req->header.path << ", request ptr: " << (void*)req.get();
  }
  conn->sendRequest(
      std::move(req),
      [this, &pool, isFromPool, cb = std::move(cb),
       endpoint = std::move(endpoint)](fuerte::Error err,
                                       std::unique_ptr<fuerte::Request> req,
                                       std::unique_ptr<fuerte::Response> res) {
        if (req->timeQueued().time_since_epoch().count() != 0 &&
            req->timeAsyncWrite().time_since_epoch().count() != 0) {
          // In the 0 cases fuerte did not even accept or start to send
          // the request, so there is nothing to report.
          auto dur = std::chrono::duration_cast<std::chrono::duration<double>>(
              req->timeAsyncWrite() - req->timeQueued());
          _dequeueDurations.count(dur.count());
          if (req->timeSent().time_since_epoch().count() == 0) {
            // The request sending was never finished. This could be a timeout
            // during the sending phase.
            LOG_TOPIC("effc3", DEBUG, Logger::COMMUNICATION)
                << "Time to dequeue request to " << endpoint << ": "
                << dur.count()
                << " seconds, however, the sending has not yet finished so far"
                << ", endpoint: " << endpoint
                << ", request ptr: " << (void*)req.get()
                << ", response ptr: " << (void*)res.get()
                << ", error: " << uint16_t(err);
            _unfinishedSends.count();
          } else {
            // The request was fully sent off, we have received the callback
            // from asio.
            dur = std::chrono::duration_cast<std::chrono::duration<double>>(
                req->timeSent() - req->timeAsyncWrite());
            _sendDurations.count(dur.count());
            // If you suspect network delays in your infrastructure, you
            // can use the following log message to track them down and
            // to associate them with particular requests:
            LOG_TOPIC_IF("effc4", DEBUG, Logger::COMMUNICATION,
                         dur > std::chrono::seconds(3))
                << "Time to send request to " << endpoint << ": " << dur.count()
                << " seconds, endpoint: " << endpoint
                << ", request ptr: " << (void*)req.get()
                << ", response ptr: " << (void*)res.get()
                << ", error: " << uint16_t(err);

            dur = std::chrono::duration_cast<std::chrono::duration<double>>(
                std::chrono::steady_clock::now() - req->timeSent());
            // If you suspect network delays in your infrastructure, you
            // can use the following log message to track them down and
            // to associate them with particular requests:
            LOG_TOPIC_IF("effc5", DEBUG, Logger::COMMUNICATION,
                         dur > std::chrono::seconds(61))
                << "Time since request was sent out to " << endpoint
                << " until now was " << dur.count()
                << " seconds, endpoint: " << endpoint
                << ", request ptr: " << (void*)req.get()
                << ", response ptr: " << (void*)res.get()
                << ", error: " << uint16_t(err);
            _responseDurations.count(dur.count());
          }
        }
        TRI_ASSERT(req != nullptr);
        finishRequest(pool, err, req, res);
        cb(err, std::move(req), std::move(res), isFromPool);
      });
}

void NetworkFeature::prepareRequest(network::ConnectionPool const& pool,
                                    std::unique_ptr<fuerte::Request>& req) {
  _requestsInFlight += 1;
  if (req) {
    req->timestamp(std::chrono::steady_clock::now());
  }
}

void NetworkFeature::finishRequest(network::ConnectionPool const& pool,
                                   fuerte::Error err,
                                   std::unique_ptr<fuerte::Request> const& req,
                                   std::unique_ptr<fuerte::Response>& res) {
  _requestsInFlight -= 1;
  if (err == fuerte::Error::RequestTimeout) {
    _requestTimeouts.count();
  } else if (req && res) {
    res->timestamp(std::chrono::steady_clock::now());
    std::chrono::milliseconds duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(res->timestamp() -
                                                              req->timestamp());
    std::chrono::milliseconds timeout = req->timeout();
    TRI_ASSERT(timeout.count() > 0);
    if (timeout.count() > 0) {
      // only go in here if we are sure to not divide by zero
      double percentage =
          std::clamp(100.0 * static_cast<double>(duration.count()) /
                         static_cast<double>(timeout.count()),
                     0.0, 100.0);
      _requestDurations.count(percentage);
    } else {
      // the timeout value was 0, for whatever reason. this is unexpected,
      // but we must not make the program crash here.
      // so instead log a warning and interpret this as a request that took
      // 100% of the timeout duration.
      _requestDurations.count(100.0);
      LOG_TOPIC("1688c", WARN, Logger::FIXME)
          << "encountered invalid 0s timeout for internal request to path "
          << req->header.path;
    }
  }
}

void NetworkFeature::retryRequest(
    std::shared_ptr<network::RetryableRequest> req, RequestLane lane,
    std::chrono::steady_clock::duration duration) {
  auto cb = [this, weak = std::weak_ptr(req)](bool cancelled) {
    std::unique_lock guard(_workItemMutex);
    if (auto self = weak.lock(); self) {
      _retryRequests.erase(self);
      guard.unlock();  // resuming the request does not access _retryRequests
      if (cancelled) {
        self->cancel();
      } else {
        self->retry();
      }
    }
  };

  // this will automatically cancel the request when we leave this
  // method, unless we are canceling this scopeGuard explicitly.
  // note that the mutex lock will be unlocked before the scopeGuard
  // fires.
  auto cancelGuard = scopeGuard([req]() noexcept {
    if (req) {
      req->cancel();
    }
  });

  // we need the mutex during `queueDelayed` because the lambda might be
  // executed faster than we can add the work item to the _retryRequests map.
  std::unique_lock guard(_workItemMutex);

  if (server().isStopping()) {
    // with trigger cancelGuard - cancels request
    return;
  }

  auto item = SchedulerFeature::SCHEDULER->queueDelayed(
      "retry-requests", lane, duration, std::move(cb));

  if (item != nullptr) {
    try {
      TRI_IF_FAILURE("NetworkFeature::retryRequestFail") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }

      _retryRequests.emplace(std::move(req), std::move(item));
      // the request successfully made it into _retryRequests.
      // now abandon the automatic cancelation.
      cancelGuard.cancel();
    } catch (...) {
    }
  }
  // if we get here and haven't canceled cancelGuard,
  // the request will be automatically canceled
}

}  // namespace arangodb
