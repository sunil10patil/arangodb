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

#include "Basics/Identifier.h"
#include "Basics/ReadLocker.h"
#include "Basics/ReadWriteLock.h"
#include "Basics/ReadWriteSpinLock.h"
#include "Basics/Result.h"
#include "Basics/ResultT.h"
#include "Cluster/CallbackGuard.h"
#include "Logger/LogMacros.h"
#include "Transaction/ManagedContext.h"
#include "Transaction/Status.h"
#include "VocBase/AccessMode.h"
#include "VocBase/Identifiers/TransactionId.h"
#include "VocBase/voc-types.h"
#include <absl/hash/hash.h>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <vector>

namespace arangodb {
class TransactionState;

namespace velocypack {
class Builder;
class Slice;
}  // namespace velocypack

namespace transaction {
class Context;
class ManagerFeature;
class Hints;
struct Options;

struct IManager {
  virtual ~IManager() = default;
  virtual Result abortManagedTrx(TransactionId,
                                 std::string const& database) = 0;
};

/// @brief Tracks TransactionState instances
class Manager final : public IManager {
  static constexpr size_t numBuckets = 16;
  static constexpr double tombstoneTTL = 10.0 * 60.0;              // 10 minutes
  static constexpr size_t maxTransactionSize = 128 * 1024 * 1024;  // 128 MiB

  enum class MetaType : uint8_t {
    Managed = 1,        /// global single shard db transaction
    StandaloneAQL = 2,  /// used for a standalone transaction (AQL standalone)
    Tombstone = 3  /// used to ensure we can acknowledge double commits / aborts
  };

  struct ManagedTrx {
    ManagedTrx(ManagerFeature const& feature, MetaType type, double ttl,
               std::shared_ptr<TransactionState> state,
               arangodb::cluster::CallbackGuard rGuard);
    ~ManagedTrx();

    bool hasPerformedIntermediateCommits() const noexcept;
    bool expired() const noexcept;
    void updateExpiry() noexcept;

    /// @brief managed, AQL or tombstone
    MetaType type;
    /// @brief whether or not the transaction has performed any intermediate
    /// commits
    bool intermediateCommits;
    /// @brief whether or not the transaction did expire at least once
    bool wasExpired;

    /// @brief number of (reading) side users of the transaction. this number
    /// is currently only increased on DB servers when they handle incoming
    /// requests by the AQL document function. while this number is > 0, there
    /// are still read requests ongoing, and the transaction status cannot be
    /// changed to committed/aborted.
    std::atomic<uint32_t> sideUsers;
    /// @brief  final TRX state that is valid if this is a tombstone
    /// necessary to avoid getting error on a 'diamond' commit or accidentally
    /// repeated commit / abort messages
    transaction::Status finalStatus;
    double const timeToLive;
    double expiryTime;                        // time this expires
    std::shared_ptr<TransactionState> state;  /// Transaction, may be nullptr
    arangodb::cluster::CallbackGuard rGuard;
    std::string const user;  /// user owning the transaction
    std::string db;          /// database in which the transaction operates
    /// cheap usage lock for _state
    mutable basics::ReadWriteSpinLock rwlock;
  };

 public:
  Manager(Manager const&) = delete;
  Manager& operator=(Manager const&) = delete;

  explicit Manager(ManagerFeature& feature);

  static constexpr double idleTTLDBServer = 5 * 60.0;  //  5 minutes

  // register a transaction
  void registerTransaction(TransactionId transactionId,
                           bool isReadOnlyTransaction,
                           bool isFollowerTransaction);

  // unregister a transaction
  void unregisterTransaction(TransactionId transactionId,
                             bool isReadOnlyTransaction,
                             bool isFollowerTransaction) noexcept;

  uint64_t getActiveTransactionCount();

  void disallowInserts() noexcept {
    _disallowInserts.store(true, std::memory_order_release);
  }

  arangodb::cluster::CallbackGuard buildCallbackGuard(
      TransactionState const& state);

  /// @brief register a AQL transaction
  void registerAQLTrx(std::shared_ptr<TransactionState> const&);
  void unregisterAQLTrx(TransactionId tid) noexcept;

  /// @brief create managed transaction, also generate a tranactionId
  ResultT<TransactionId> createManagedTrx(TRI_vocbase_t& vocbase,
                                          velocypack::Slice trxOpts,
                                          bool allowDirtyReads);

  /// @brief create managed transaction, also generate a tranactionId
  ResultT<TransactionId> createManagedTrx(
      TRI_vocbase_t& vocbase, std::vector<std::string> const& readCollections,
      std::vector<std::string> const& writeCollections,
      std::vector<std::string> const& exclusiveCollections,
      transaction::Options options, double ttl = 0.0);

  /// @brief ensure managed transaction, either use the one on the given tid
  ///        or create a new one with the given tid
  Result ensureManagedTrx(TRI_vocbase_t& vocbase, TransactionId tid,
                          velocypack::Slice trxOpts,
                          bool isFollowerTransaction);

  /// @brief ensure managed transaction, either use the one on the given tid
  ///        or create a new one with the given tid
  Result ensureManagedTrx(TRI_vocbase_t& vocbase, TransactionId tid,
                          std::vector<std::string> const& readCollections,
                          std::vector<std::string> const& writeCollections,
                          std::vector<std::string> const& exclusiveCollections,
                          transaction::Options options, double ttl = 0.0);

  Result beginTransaction(transaction::Hints hints,
                          std::shared_ptr<TransactionState>& state);

  /// @brief lease the transaction, increases nesting
  std::shared_ptr<transaction::Context> leaseManagedTrx(TransactionId tid,
                                                        AccessMode::Type mode,
                                                        bool isSideUser);
  void returnManagedTrx(TransactionId, bool isSideUser) noexcept;

  /// @brief get the meta transaction state
  transaction::Status getManagedTrxStatus(TransactionId,
                                          std::string const& database) const;

  Result commitManagedTrx(TransactionId, std::string const& database);
  Result abortManagedTrx(TransactionId, std::string const& database) override;

  /// @brief collect forgotten transactions
  bool garbageCollect(bool abortAll);

  /// @brief abort all transactions matching
  bool abortManagedTrx(
      std::function<bool(TransactionState const&, std::string const&)>);

  /// @brief abort all managed write transactions
  Result abortAllManagedWriteTrx(std::string const& username, bool fanout);

  /// @brief convert the list of running transactions to a VelocyPack array
  /// the array must be opened already.
  /// will use database and username to fan-out the request to the other
  /// coordinators in a cluster
  void toVelocyPack(arangodb::velocypack::Builder& builder,
                    std::string const& database, std::string const& username,
                    bool fanout) const;

  // ---------------------------------------------------------------------------
  // Hotbackup Stuff
  // ---------------------------------------------------------------------------

  // temporarily block all transactions from committing
  template<typename TimeOutType>
  bool holdTransactions(TimeOutType timeout) {
    bool ret = false;
    std::unique_lock<std::mutex> guard(_hotbackupMutex);
    if (!_hotbackupCommitLockHeld) {
      LOG_TOPIC("eedda", TRACE, Logger::TRANSACTIONS)
          << "Trying to get write lock to hold transactions...";
      ret = _hotbackupCommitLock.tryLockWriteFor(timeout);
      if (ret) {
        LOG_TOPIC("eeddb", TRACE, Logger::TRANSACTIONS)
            << "Got write lock to hold transactions.";
        _hotbackupCommitLockHeld = true;
      } else {
        LOG_TOPIC("eeddc", TRACE, Logger::TRANSACTIONS)
            << "Did not get write lock to hold transactions.";
      }
    }
    return ret;
  }

  // remove the block
  void releaseTransactions() noexcept {
    std::unique_lock<std::mutex> guard(_hotbackupMutex);
    if (_hotbackupCommitLockHeld) {
      LOG_TOPIC("eeddd", TRACE, Logger::TRANSACTIONS)
          << "Releasing write lock to hold transactions.";
      _hotbackupCommitLock.unlockWrite();
      _hotbackupCommitLockHeld = false;
    }
  }

  using TransactionCommitGuard = basics::ReadLocker<basics::ReadWriteLock>;

  TransactionCommitGuard getTransactionCommitGuard();

  void initiateSoftShutdown() {
    _softShutdownOngoing.store(true, std::memory_order_relaxed);
  }

 private:
  Result prepareOptions(transaction::Options& options);
  bool isFollowerTransactionOnDBServer(
      transaction::Options const& options) const;
  Result lockCollections(TRI_vocbase_t& vocbase,
                         std::shared_ptr<TransactionState> state,
                         std::vector<std::string> const& exclusiveCollections,
                         std::vector<std::string> const& writeCollections,
                         std::vector<std::string> const& readCollections);
  transaction::Hints ensureHints(transaction::Options& options) const;

  /// @brief performs a status change on a transaction using a timeout
  Result statusChangeWithTimeout(TransactionId tid, std::string const& database,
                                 transaction::Status status);

  /// @brief hashes the transaction id into a bucket
  inline size_t getBucket(TransactionId tid) const noexcept {
    return absl::Hash<uint64_t>()(tid.id()) % numBuckets;
  }

  std::shared_ptr<ManagedContext> buildManagedContextUnderLock(
      TransactionId tid, ManagedTrx& mtrx);

  Result updateTransaction(
      TransactionId tid, transaction::Status status, bool clearServers,
      std::string const& database =
          "" /* leave empty to operate across all databases */);

  /// @brief calls the callback function for each managed transaction
  void iterateManagedTrx(
      std::function<void(TransactionId, ManagedTrx const&)> const&) const;

  static double ttlForType(ManagerFeature const& feature, Manager::MetaType);

  bool transactionIdExists(TransactionId const& tid) const;

  bool storeManagedState(TransactionId const& tid,
                         std::shared_ptr<arangodb::TransactionState> state,
                         double ttl);

 private:
  ManagerFeature& _feature;

  struct {
    // a lock protecting _managed
    mutable basics::ReadWriteLock _lock;

    // managed transactions, seperate lifetime from above
    std::unordered_map<TransactionId, ManagedTrx> _managed;
  } _transactions[numBuckets];

  /// Nr of running transactions
  std::atomic<uint64_t> _nrRunning;

  std::atomic<bool> _disallowInserts;

  std::mutex _hotbackupMutex;  // Makes sure that we only ever get or release
                               // the write lock and adjust _writeLockHeld at
                               // the same time.
  basics::ReadWriteLock _hotbackupCommitLock;
  bool _hotbackupCommitLockHeld;

  double _streamingLockTimeout;

  std::atomic<bool> _softShutdownOngoing;
};
}  // namespace transaction
}  // namespace arangodb
