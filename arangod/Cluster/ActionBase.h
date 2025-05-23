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

#pragma once

#include "ActionDescription.h"

#include "Basics/Common.h"
#include "Basics/Result.h"
#include "Basics/debugging.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace arangodb {

class MaintenanceFeature;

namespace maintenance {

class Action;

class ActionBase {
 public:
  ActionBase(MaintenanceFeature&, ActionDescription const&);

  ActionBase(MaintenanceFeature&, ActionDescription&&);

  ActionBase() = delete;

  ActionBase(ActionBase const&) = delete;
  ActionBase& operator=(ActionBase const&) = delete;

  virtual ~ActionBase();

  //
  // MaintenanceWork entry points
  //

  /// @brief initial call to object to perform a unit of work.
  ///   really short tasks could do all work here and return false
  /// @return true to continue processing, false done (result() set)
  virtual bool first() = 0;

  /// @brief iterative call to perform a unit of work
  /// @return true to continue processing, false done (result() set)
  virtual bool next() { return false; }

  /// @brief execution finished successfully or failed ... and race timer
  /// expired
  virtual bool done() const;

  /// @brief waiting for a worker to grab it and go!
  bool runnable() const noexcept { return READY == _state; }

  /// @brief did initialization have issues?
  bool ok() const noexcept { return FAILED != _state; }

  /// @brief adjust state of object, assumes WRITE lock on _actionRegistryLock
  ActionState state() const noexcept { return _state; }

  bool fastTrack() const noexcept;

  void notify();

  virtual arangodb::Result progress(double& progress);

  ActionDescription const& describe() const;

  MaintenanceFeature& feature() const;

  std::string const& get(std::string const&) const;

  VPackSlice const properties() const;

  /// @brief adjust state of object, assumes WRITE lock on _actionRegistryLock
  ActionState getState() const;

  /// @brief adjust state of object, assumes WRITE lock on _actionRegistryLock
  virtual void setState(ActionState state);

  /// @brief update incremental statistics
  void startStats();

  /// @brief update incremental statistics
  void incStats();

  /// @brief finalize statistics
  void endStats();

  /**
   *  @brief    update progress by long running processes
   *  @param  d percentage of work done
   *  @return   abort if !ok(), true if ok(), with reason to abort.
   */
  virtual arangodb::Result setProgress(double d);

  /// @brief return progress statistic
  double getProgress() const { return _progress.load(); }

  /// @brief Once PreAction completes, remove its pointer
  void clearPreAction() { _preAction.reset(); }

  /// @brief Retrieve pointer to action that should run before this one
  std::shared_ptr<Action> getPreAction();

  /// @brief Initiate a pre action
  void createPreAction(std::shared_ptr<ActionDescription> const& description);

  /// @brief Initiate a post action
  void createPostAction(std::shared_ptr<ActionDescription> const& description);

  /// @brief Retrieve pointer to action that should run directly after this one
  std::shared_ptr<Action> getPostAction();

  /// @brief Save pointer to successor action
  void setPostAction(std::shared_ptr<ActionDescription>& post) {
    _postAction = post;
  }

  /// @brief hash value of ActionDescription
  /// @return uint64_t hash
  std::string clientId() const { return _clientId; }

  /// @brief hash value of ActionDescription
  /// @return uint64_t hash
  uint64_t hash() const { return _hash; }

  /// @brief hash value of ActionDescription
  /// @return uint64_t hash
  uint64_t id() const { return _id; }

  /// @brief add VPackObject to supplied builder with info about this action
  virtual void toVelocyPack(VPackBuilder& builder) const;

  /// @brief add VPackObject to supplied builder with info about this action
  VPackBuilder toVelocyPack() const;

  /// @brief Returns json array of object contents for status reports
  ///  Thread safety of this function is questionable for some member objects
  //  virtual Result toJson(/* builder */) {return Result;}

  /// @brief Return Result object containing action specific status
  Result result() const;

  /// @brief Set the contained result object
  void result(Result const& result);
  void result(ErrorCode errorNumber,
              std::string const& errorMessage = std::string());

  /// @brief When object was constructed
  std::chrono::system_clock::time_point getCreateTime() const {
    return std::chrono::system_clock::time_point() + _actionCreated.load();
  }

  /// @brief When object was first started
  std::chrono::system_clock::time_point getStartTime() const {
    return std::chrono::system_clock::time_point() + _actionStarted.load();
  }

  /// @brief When object most recently iterated
  std::chrono::system_clock::time_point getLastStatTime() const {
    return std::chrono::system_clock::time_point() + _actionLastStat.load();
  }

  /// @brief When object finished executing
  std::chrono::system_clock::time_point getDoneTime() const {
    return std::chrono::system_clock::time_point() + _actionDone.load();
  }

  /// @brief check if worker lables match ours
  bool matches(std::unordered_set<std::string> const& options) const;

  std::string const static FAST_TRACK;

  /// @brief return priority, inherited from ActionDescription
  int priority() const { return _priority; }

  void setPriority(int prio) { _priority = prio; }

  bool requeueRequested() const { return _requeueRequested; }

  int requeuePriority() const {
    TRI_ASSERT(_requeueRequested);
    return _requeuePriority;
  }

  void requeueMe(int requeuePriority) {
    _requeueRequested = true;
    _requeuePriority = requeuePriority;
  }

 protected:
  /// @brief common initialization for all constructors
  void init();

  arangodb::MaintenanceFeature& _feature;

  ActionDescription _description;

  // @brief optional labels for matching to woker labels
  std::unordered_set<std::string> _labels;

  uint64_t _hash;
  std::string _clientId;

  uint64_t _id;

  std::atomic<ActionState> _state;

  // NOTE: preAction should only be set within first() or post(), not
  // construction
  std::shared_ptr<ActionDescription> _preAction;
  std::shared_ptr<ActionDescription> _postAction;

  // times for user reporting (and _actionDone used by done() to prevent
  //  race conditions of same task executing twice
  std::atomic<std::chrono::system_clock::duration> _actionCreated;
  std::atomic<std::chrono::system_clock::duration> _actionStarted;
  std::atomic<std::chrono::system_clock::duration> _actionLastStat;
  std::atomic<std::chrono::system_clock::duration> _actionDone;

  std::atomic<double> _progress;

  int _priority;

 private:
  mutable std::mutex resLock;
  Result _result;

  bool _requeueRequested = false;
  int _requeuePriority = 0;
};  // class ActionBase

class ShardDefinition {
 public:
  ShardDefinition(ShardDefinition const&) = delete;
  ShardDefinition& operator=(ShardDefinition const&) = delete;

  ShardDefinition(std::string const& database, std::string const& shard);

  virtual ~ShardDefinition() = default;

  std::string const& getDatabase() const noexcept { return _database; }

  std::string const& getShard() const noexcept { return _shard; }

  bool isValid() const noexcept {
    return !_database.empty() && !_shard.empty();
  }

 private:
  std::string const _database;
  std::string const _shard;
};

}  // namespace maintenance

Result actionError(ErrorCode errorCode, std::string const& errorMessage);
Result actionWarn(ErrorCode errorCode, std::string const& errorMessage);

}  // namespace arangodb

namespace std {
ostream& operator<<(ostream& o, arangodb::maintenance::ActionBase const& d);
}
