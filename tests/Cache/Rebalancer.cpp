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
/// @author Dan Larkin-York
/// @author Copyright 2017, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Basics/ScopeGuard.h"
#include "Basics/voc-errors.h"
#include "Cache/BinaryKeyHasher.h"
#include "Cache/CacheOptionsProvider.h"
#include "Cache/Common.h"
#include "Cache/Manager.h"
#include "Cache/PlainCache.h"
#include "Cache/Rebalancer.h"
#include "Cache/Transaction.h"
#include "Cache/TransactionalCache.h"
#include "Random/RandomGenerator.h"
#include "RestServer/SharedPRNGFeature.h"

#include "Mocks/Servers.h"
#include "MockScheduler.h"

using namespace arangodb;
using namespace arangodb::cache;
using namespace arangodb::tests::mocks;

struct ThreadGuard {
  ThreadGuard(ThreadGuard&&) = default;
  ThreadGuard& operator=(ThreadGuard&&) = default;

  ThreadGuard(std::unique_ptr<std::thread> thread)
      : thread(std::move(thread)) {}
  ~ThreadGuard() { join(); }

  void join() {
    if (thread != nullptr) {
      thread->join();
      thread.reset();
    }
  }

  std::unique_ptr<std::thread> thread;
};

// long-running

TEST(CacheRebalancerTest, test_rebalancing_with_plaincache_LongRunning) {
  RandomGenerator::initialize(RandomGenerator::RandomType::MERSENNE);
  MockScheduler scheduler(4);
  auto postFn = [&scheduler](std::function<void()> fn) -> bool {
    scheduler.post(fn);
    return true;
  };
  MockMetricsServer server;
  SharedPRNGFeature& sharedPRNG = server.getFeature<SharedPRNGFeature>();
  CacheOptions co;
  co.cacheSize = 128 * 1024 * 1024;
  Manager manager(sharedPRNG, postFn, co);
  Rebalancer rebalancer(&manager);

  std::size_t cacheCount = 4;
  std::size_t threadCount = 4;
  std::vector<std::shared_ptr<Cache>> caches;
  for (std::size_t i = 0; i < cacheCount; i++) {
    caches.emplace_back(manager.createCache<BinaryKeyHasher>(CacheType::Plain));
  }

  std::atomic<bool> doneRebalancing(false);
  auto rebalanceWorker = [&rebalancer, &doneRebalancing]() -> void {
    while (!doneRebalancing) {
      auto status = rebalancer.rebalance();
      if (status != TRI_ERROR_ARANGO_BUSY) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  };

  ThreadGuard rebalancerThread(std::make_unique<std::thread>(rebalanceWorker));

  std::uint64_t chunkSize = 4 * 1024 * 1024;
  std::uint64_t initialInserts = 1 * 1024 * 1024;
  std::uint64_t operationCount = 4 * 1024 * 1024;
  std::atomic<std::uint64_t> hitCount(0);
  std::atomic<std::uint64_t> missCount(0);
  auto worker = [&caches, cacheCount, initialInserts, operationCount, &hitCount,
                 &missCount](std::uint64_t lower, std::uint64_t upper) -> void {
    // fill with some initial data
    for (std::uint64_t i = 0; i < initialInserts; i++) {
      std::uint64_t item = lower + i;
      std::size_t cacheIndex = item % cacheCount;
      CachedValue* value = CachedValue::construct(&item, sizeof(std::uint64_t),
                                                  &item, sizeof(std::uint64_t));
      TRI_ASSERT(value != nullptr);
      auto status = caches[cacheIndex]->insert(value);
      if (status != TRI_ERROR_NO_ERROR) {
        delete value;
      }
    }

    // initialize valid range for keys that *might* be in cache
    std::uint64_t validLower = lower;
    std::uint64_t validUpper = lower + initialInserts - 1;

    // commence mixed workload
    for (std::uint64_t i = 0; i < operationCount; i++) {
      std::uint32_t r =
          RandomGenerator::interval(static_cast<std::uint32_t>(99UL));

      if (r >= 99) {  // remove something
        if (validLower == validUpper) {
          continue;  // removed too much
        }

        std::uint64_t item = validLower++;
        std::size_t cacheIndex = item % cacheCount;

        caches[cacheIndex]->remove(&item, sizeof(std::uint64_t));
      } else if (r >= 95) {  // insert something
        if (validUpper == upper) {
          continue;  // already maxed out range
        }

        std::uint64_t item = ++validUpper;
        std::size_t cacheIndex = item % cacheCount;
        CachedValue* value = CachedValue::construct(
            &item, sizeof(std::uint64_t), &item, sizeof(std::uint64_t));
        TRI_ASSERT(value != nullptr);
        auto status = caches[cacheIndex]->insert(value);
        if (status != TRI_ERROR_NO_ERROR) {
          delete value;
        }
      } else {  // lookup something
        std::uint64_t item = RandomGenerator::interval(
            static_cast<int64_t>(validLower), static_cast<int64_t>(validUpper));
        std::size_t cacheIndex = item % cacheCount;

        Finding f = caches[cacheIndex]->find(&item, sizeof(std::uint64_t));
        if (f.found()) {
          hitCount++;
          TRI_ASSERT(f.value() != nullptr);
          TRI_ASSERT(BinaryKeyHasher::sameKey(f.value()->key(),
                                              f.value()->keySize(), &item,
                                              sizeof(std::uint64_t)));
        } else {
          missCount++;
          TRI_ASSERT(f.value() == nullptr);
        }
      }
    }
  };

  std::vector<ThreadGuard> threads;
  // dispatch threads
  for (std::size_t i = 0; i < threadCount; i++) {
    std::uint64_t lower = i * chunkSize;
    std::uint64_t upper = ((i + 1) * chunkSize) - 1;
    threads.emplace_back(std::make_unique<std::thread>(worker, lower, upper));
  }

  // join threads
  threads.clear();

  doneRebalancing = true;
  rebalancerThread.join();

  for (auto cache : caches) {
    manager.destroyCache(std::move(cache));
  }
  caches.clear();

  RandomGenerator::shutdown();
}

TEST(CacheRebalancerTest,
     test_rebalancing_with_transactionalcache_LongRunning) {
  RandomGenerator::initialize(RandomGenerator::RandomType::MERSENNE);
  MockScheduler scheduler(4);
  auto postFn = [&scheduler](std::function<void()> fn) -> bool {
    scheduler.post(fn);
    return true;
  };
  MockMetricsServer server;
  SharedPRNGFeature& sharedPRNG = server.getFeature<SharedPRNGFeature>();
  CacheOptions co;
  co.cacheSize = 128 * 1024 * 1024;
  Manager manager(sharedPRNG, postFn, co);
  Rebalancer rebalancer(&manager);

  std::size_t cacheCount = 4;
  std::size_t threadCount = 4;
  std::vector<std::shared_ptr<Cache>> caches;
  for (std::size_t i = 0; i < cacheCount; i++) {
    caches.emplace_back(
        manager.createCache<BinaryKeyHasher>(CacheType::Transactional));
  }

  std::atomic_bool doneRebalancing = false;
  auto rebalanceWorker = [&rebalancer, &doneRebalancing]() -> void {
    while (!doneRebalancing) {
      auto status = rebalancer.rebalance();
      if (status != TRI_ERROR_ARANGO_BUSY) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  };

  ThreadGuard rebalancerThread(std::make_unique<std::thread>(rebalanceWorker));

  std::uint64_t chunkSize = 4 * 1024 * 1024;
  std::uint64_t initialInserts = 1 * 1024 * 1024;
  std::uint64_t operationCount = 4 * 1024 * 1024;
  std::atomic<std::uint64_t> hitCount(0);
  std::atomic<std::uint64_t> missCount(0);
  auto worker = [&manager, &caches, cacheCount, initialInserts, operationCount,
                 &hitCount,
                 &missCount](std::uint64_t lower, std::uint64_t upper) -> void {
    Transaction* tx = manager.beginTransaction(false);
    // fill with some initial data
    for (std::uint64_t i = 0; i < initialInserts; i++) {
      std::uint64_t item = lower + i;
      std::size_t cacheIndex = item % cacheCount;
      CachedValue* value = CachedValue::construct(&item, sizeof(std::uint64_t),
                                                  &item, sizeof(std::uint64_t));
      TRI_ASSERT(value != nullptr);
      auto status = caches[cacheIndex]->insert(value);
      if (status != TRI_ERROR_NO_ERROR) {
        delete value;
      }
    }

    // initialize valid range for keys that *might* be in cache
    std::uint64_t validLower = lower;
    std::uint64_t validUpper = lower + initialInserts - 1;
    std::uint64_t banishUpper = validUpper;

    // commence mixed workload
    for (std::uint64_t i = 0; i < operationCount; i++) {
      std::uint32_t r =
          RandomGenerator::interval(static_cast<std::uint32_t>(99UL));

      if (r >= 99) {  // remove something
        if (validLower == validUpper) {
          continue;  // removed too much
        }

        std::uint64_t item = validLower++;
        std::size_t cacheIndex = item % cacheCount;

        caches[cacheIndex]->remove(&item, sizeof(std::uint64_t));
      } else if (r >= 90) {  // insert something
        if (validUpper == upper) {
          continue;  // already maxed out range
        }

        std::uint64_t item = ++validUpper;
        if (validUpper > banishUpper) {
          banishUpper = validUpper;
        }
        std::size_t cacheIndex = item % cacheCount;
        CachedValue* value = CachedValue::construct(
            &item, sizeof(std::uint64_t), &item, sizeof(std::uint64_t));
        TRI_ASSERT(value != nullptr);
        auto status = caches[cacheIndex]->insert(value);
        if (status != TRI_ERROR_NO_ERROR) {
          delete value;
        }
      } else if (r >= 80) {  // banish something
        if (banishUpper == upper) {
          continue;  // already maxed out range
        }

        std::uint64_t item = ++banishUpper;
        std::size_t cacheIndex = item % cacheCount;
        caches[cacheIndex]->banish(&item, sizeof(std::uint64_t));
      } else {  // lookup something
        std::uint64_t item = RandomGenerator::interval(
            static_cast<int64_t>(validLower), static_cast<int64_t>(validUpper));
        std::size_t cacheIndex = item % cacheCount;

        Finding f = caches[cacheIndex]->find(&item, sizeof(std::uint64_t));
        if (f.found()) {
          hitCount++;
          TRI_ASSERT(f.value() != nullptr);
          TRI_ASSERT(BinaryKeyHasher::sameKey(f.value()->key(),
                                              f.value()->keySize(), &item,
                                              sizeof(std::uint64_t)));
        } else {
          missCount++;
          TRI_ASSERT(f.value() == nullptr);
        }
      }
    }
    manager.endTransaction(tx);
  };

  std::vector<ThreadGuard> threads;
  // dispatch threads
  for (std::size_t i = 0; i < threadCount; i++) {
    std::uint64_t lower = i * chunkSize;
    std::uint64_t upper = ((i + 1) * chunkSize) - 1;
    threads.emplace_back(std::make_unique<std::thread>(worker, lower, upper));
  }

  // join threads
  threads.clear();

  doneRebalancing = true;
  rebalancerThread.join();

  for (auto cache : caches) {
    manager.destroyCache(std::move(cache));
  }
  caches.clear();

  RandomGenerator::shutdown();
}

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
TEST(
    CacheRebalancerTest,
    test_rebalancing_with_transactionalcache_and_dispatch_failures_LongRunning) {
  RandomGenerator::initialize(RandomGenerator::RandomType::MERSENNE);
  MockScheduler scheduler(4);
  auto postFn = [&scheduler](std::function<void()> fn) -> bool {
    scheduler.post(fn);
    return true;
  };
  MockMetricsServer server;
  SharedPRNGFeature& sharedPRNG = server.getFeature<SharedPRNGFeature>();
  CacheOptions co;
  // small enough so that we have memory pressure!
  co.cacheSize = 8 * 1024 * 1024;
  Manager manager(sharedPRNG, postFn, co);
  Rebalancer rebalancer(&manager);

  std::size_t cacheCount = 4;
  std::size_t threadCount = 4;
  std::vector<std::shared_ptr<Cache>> caches;
  for (std::size_t i = 0; i < cacheCount; i++) {
    caches.emplace_back(
        manager.createCache<BinaryKeyHasher>(CacheType::Transactional));
  }

  std::atomic_bool doneRebalancing = false;
  auto rebalanceWorker = [&rebalancer, &doneRebalancing]() -> void {
    while (!doneRebalancing) {
      auto status = rebalancer.rebalance();
      if (status != TRI_ERROR_ARANGO_BUSY) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  };

  ThreadGuard rebalancerThread(std::make_unique<std::thread>(rebalanceWorker));

  std::uint64_t chunkSize = 4 * 1024 * 1024;
  std::uint64_t initialInserts = 2 * 1024 * 1024;
  std::uint64_t operationCount = 8 * 1024 * 1024;
  std::atomic<std::uint64_t> hitCount(0);
  std::atomic<std::uint64_t> missCount(0);
  auto worker = [&manager, &caches, cacheCount, initialInserts, operationCount,
                 &hitCount,
                 &missCount](std::uint64_t lower, std::uint64_t upper) -> void {
    Transaction* tx = manager.beginTransaction(false);
    // fill with some initial data
    for (std::uint64_t i = 0; i < initialInserts; i++) {
      std::uint64_t item = lower + i;
      std::size_t cacheIndex = item % cacheCount;
      CachedValue* value = CachedValue::construct(&item, sizeof(std::uint64_t),
                                                  &item, sizeof(std::uint64_t));
      TRI_ASSERT(value != nullptr);
      auto status = caches[cacheIndex]->insert(value);
      if (status != TRI_ERROR_NO_ERROR) {
        delete value;
      }
    }

    // initialize valid range for keys that *might* be in cache
    std::uint64_t validLower = lower;
    std::uint64_t validUpper = lower + initialInserts - 1;
    std::uint64_t banishUpper = validUpper;

    // commence mixed workload
    for (std::uint64_t i = 0; i < operationCount; i++) {
      std::uint32_t r =
          RandomGenerator::interval(static_cast<std::uint32_t>(99UL));

      if (r >= 99) {  // remove something
        if (validLower == validUpper) {
          continue;  // removed too much
        }

        std::uint64_t item = validLower++;
        std::size_t cacheIndex = item % cacheCount;

        caches[cacheIndex]->remove(&item, sizeof(std::uint64_t));
      } else if (r >= 90) {  // insert something
        if (validUpper == upper) {
          continue;  // already maxed out range
        }

        std::uint64_t item = ++validUpper;
        if (validUpper > banishUpper) {
          banishUpper = validUpper;
        }
        std::size_t cacheIndex = item % cacheCount;
        CachedValue* value = CachedValue::construct(
            &item, sizeof(std::uint64_t), &item, sizeof(std::uint64_t));
        TRI_ASSERT(value != nullptr);
        auto status = caches[cacheIndex]->insert(value);
        if (status != TRI_ERROR_NO_ERROR) {
          delete value;
        }
      } else if (r >= 80) {  // banish something
        if (banishUpper == upper) {
          continue;  // already maxed out range
        }

        std::uint64_t item = ++banishUpper;
        std::size_t cacheIndex = item % cacheCount;
        caches[cacheIndex]->banish(&item, sizeof(std::uint64_t));
      } else {  // lookup something
        std::uint64_t item = RandomGenerator::interval(
            static_cast<int64_t>(validLower), static_cast<int64_t>(validUpper));
        std::size_t cacheIndex = item % cacheCount;

        Finding f = caches[cacheIndex]->find(&item, sizeof(std::uint64_t));
        if (f.found()) {
          hitCount++;
          TRI_ASSERT(f.value() != nullptr);
          TRI_ASSERT(BinaryKeyHasher::sameKey(f.value()->key(),
                                              f.value()->keySize(), &item,
                                              sizeof(std::uint64_t)));
        } else {
          missCount++;
          TRI_ASSERT(f.value() == nullptr);
        }
      }
    }
    manager.endTransaction(tx);
  };

  TRI_AddFailurePointDebugging("CacheManagerTasks::dispatchFailures");
  auto guard = scopeGuard([]() noexcept { TRI_ClearFailurePointsDebugging(); });

  std::vector<ThreadGuard> threads;
  // dispatch threads
  for (std::size_t i = 0; i < threadCount; i++) {
    std::uint64_t lower = i * chunkSize;
    std::uint64_t upper = ((i + 1) * chunkSize) - 1;
    threads.emplace_back(std::make_unique<std::thread>(worker, lower, upper));
  }

  // join threads
  threads.clear();

  doneRebalancing = true;
  rebalancerThread.join();

  guard.fire();

  for (auto cache : caches) {
    manager.destroyCache(std::move(cache));
  }
  caches.clear();

  RandomGenerator::shutdown();
}
#endif
