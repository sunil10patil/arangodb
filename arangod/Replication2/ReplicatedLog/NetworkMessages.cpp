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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "NetworkMessages.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>

#include <Basics/application-exit.h>
#include <Containers/ImmerMemoryPolicy.h>
#include <Logger/LogMacros.h>
#include <Basics/voc-errors.h>
#include <Basics/Result.h>

#if (_MSC_VER >= 1)
// suppress warnings:
#pragma warning(push)
// conversion from 'size_t' to 'immer::detail::rbts::count_t', possible loss of
// data
#pragma warning(disable : 4267)
// result of 32-bit shift implicitly converted to 64 bits (was 64-bit shift
// intended?)
#pragma warning(disable : 4334)
#endif
#include <immer/flex_vector_transient.hpp>
#if (_MSC_VER >= 1)
#pragma warning(pop)
#endif

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;

#if (_MSC_VER >= 1)
// suppress false positive warning:
#pragma warning(push)
// function assumed not to throw an exception but does
#pragma warning(disable : 4297)
#endif
AppendEntriesRequest::AppendEntriesRequest(
    AppendEntriesRequest&& other) noexcept try
    : leaderTerm(other.leaderTerm),
      leaderId(std::move(other.leaderId)),
      prevLogEntry(other.prevLogEntry),
      leaderCommit(other.leaderCommit),
      lowestIndexToKeep(other.lowestIndexToKeep),
      messageId(other.messageId),
      entries(std::move(other.entries)),
      waitForSync(other.waitForSync) {
  // Note that immer::flex_vector is currently not nothrow move-assignable,
  // though it probably does not throw any exceptions. However, we *need* this
  // to be noexcept, otherwise we cannot keep the persistent and in-memory state
  // in sync.
  //
  // So even if flex_vector could actually throw here, we deliberately add the
  // noexcept!
  //
  // The try/catch is *only* for logging, but *must* terminate (e.g. by
  // rethrowing) the process if an exception is caught.
} catch (std::exception const& ex) {
  LOG_TOPIC("f8d2e", FATAL, Logger::REPLICATION2)
      << "Caught an exception when moving an AppendEntriesRequest. This is "
         "fatal, as consistency of persistent and in-memory state can no "
         "longer be guaranteed. The process will terminate now. The exception "
         "was: "
      << ex.what();
  FATAL_ERROR_ABORT();
} catch (...) {
  LOG_TOPIC("12f06", FATAL, Logger::REPLICATION2)
      << "Caught an exception when moving an AppendEntriesRequest. This is "
         "fatal, as consistency of persistent and in-memory state can no "
         "longer be guaranteed. The process will terminate now.";
  FATAL_ERROR_ABORT();
}
#if (_MSC_VER >= 1)
#pragma warning(pop)
#endif

auto AppendEntriesRequest::operator=(
    replicated_log::AppendEntriesRequest&& other) noexcept
    -> AppendEntriesRequest& try {
  // Note that immer::flex_vector is currently not nothrow move-assignable,
  // though it probably does not throw any exceptions. However, we *need* this
  // to be noexcept, otherwise we cannot keep the persistent and in-memory state
  // in sync.
  //
  // So even if flex_vector could actually throw here, we deliberately add the
  // noexcept!
  //
  // The try/catch is *only* for logging, but *must* terminate (e.g. by
  // rethrowing) the process if an exception is caught.
  leaderTerm = other.leaderTerm;
  leaderId = std::move(other.leaderId);
  prevLogEntry = other.prevLogEntry;
  leaderCommit = other.leaderCommit;
  lowestIndexToKeep = other.lowestIndexToKeep;
  messageId = other.messageId;
  waitForSync = other.waitForSync;
  entries = std::move(other.entries);

  return *this;
} catch (std::exception const& ex) {
  LOG_TOPIC("dec5f", FATAL, Logger::REPLICATION2)
      << "Caught an exception when moving an AppendEntriesRequest. This is "
         "fatal, as consistency of persistent and in-memory state can no "
         "longer be guaranteed. The process will terminate now. The exception "
         "was: "
      << ex.what();
  FATAL_ERROR_ABORT();
} catch (...) {
  LOG_TOPIC("facec", FATAL, Logger::REPLICATION2)
      << "Caught an exception when moving an AppendEntriesRequest. This is "
         "fatal, as consistency of persistent and in-memory state can no "
         "longer be guaranteed. The process will terminate now.";
  FATAL_ERROR_ABORT();
}

auto replicated_log::operator++(MessageId& id) -> MessageId& {
  ++id._value;
  return id;
}

auto replicated_log::operator<<(std::ostream& os, MessageId id)
    -> std::ostream& {
  return os << id._value;
}

auto replicated_log::to_string(MessageId id) -> std::string {
  return std::to_string(id._value);
}

auto replicated_log::to_string(AppendEntriesResult const& res) -> std::string {
  auto builder = velocypack::Builder();
  res.toVelocyPack(builder);
  return builder.toJson();
}

MessageId::operator velocypack::Value() const noexcept {
  return velocypack::Value(_value);
}
auto MessageId::value() -> std::uint64_t { return _value; }

void replicated_log::AppendEntriesResult::toVelocyPack(
    velocypack::Builder& builder) const {
  {
    velocypack::ObjectBuilder ob(&builder);
    builder.add("term", VPackValue(logTerm.value));
    builder.add("errorCode", VPackValue(errorCode));
    builder.add(VPackValue("reason"));
    reason.toVelocyPack(builder);
    builder.add("messageId", VPackValue(messageId));
    builder.add("snapshotAvailable", VPackValue(snapshotAvailable));
    builder.add("syncIndex", VPackValue(syncIndex));
    if (conflict.has_value()) {
      TRI_ASSERT(errorCode ==
                 TRI_ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED);
      TRI_ASSERT(reason.error ==
                 AppendEntriesErrorReason::ErrorType::kNoPrevLogMatch);
      builder.add(VPackValue("conflict"));
      conflict->toVelocyPack(builder);
    }
  }
}

auto replicated_log::AppendEntriesResult::fromVelocyPack(
    velocypack::Slice slice) -> AppendEntriesResult {
  auto logTerm = slice.get("term").extract<LogTerm>();
  auto errorCode = ErrorCode{slice.get("errorCode").extract<int>()};
  auto reason = AppendEntriesErrorReason::fromVelocyPack(slice.get("reason"));
  auto messageId = slice.get("messageId").extract<MessageId>();
  auto snapshotAvailable = slice.get("snapshotAvailable").isTrue();
  auto syncIndex = slice.get("syncIndex").extract<LogIndex>();

  if (reason.error == AppendEntriesErrorReason::ErrorType::kNoPrevLogMatch) {
    TRI_ASSERT(errorCode ==
               TRI_ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED);
    auto conflict = slice.get("conflict");
    TRI_ASSERT(conflict.isObject());
    return AppendEntriesResult{logTerm,
                               messageId,
                               TermIndexPair::fromVelocyPack(conflict),
                               std::move(reason),
                               snapshotAvailable,
                               syncIndex};
  }

  TRI_ASSERT(errorCode == TRI_ERROR_NO_ERROR ||
             reason.error != AppendEntriesErrorReason::ErrorType::kNone);
  return AppendEntriesResult{logTerm,   errorCode,         reason,
                             messageId, snapshotAvailable, syncIndex};
}

replicated_log::AppendEntriesResult::AppendEntriesResult(
    LogTerm logTerm, ErrorCode errorCode, AppendEntriesErrorReason reason,
    MessageId id, bool snapshotAvailable, LogIndex syncIndex) noexcept
    : logTerm(logTerm),
      errorCode(errorCode),
      reason(std::move(reason)),
      messageId(id),
      snapshotAvailable(snapshotAvailable),
      syncIndex(syncIndex) {
  static_assert(std::is_nothrow_move_constructible_v<AppendEntriesErrorReason>);
  TRI_ASSERT(errorCode == TRI_ERROR_NO_ERROR ||
             reason.error != AppendEntriesErrorReason::ErrorType::kNone);
}

replicated_log::AppendEntriesResult::AppendEntriesResult(
    LogTerm logTerm, MessageId id, bool snapshotAvailable,
    LogIndex syncIndex) noexcept
    : AppendEntriesResult(logTerm, TRI_ERROR_NO_ERROR, {}, id,
                          snapshotAvailable, syncIndex) {}

replicated_log::AppendEntriesResult::AppendEntriesResult(
    LogTerm term, replicated_log::MessageId id, TermIndexPair conflict,
    AppendEntriesErrorReason reason, bool snapshotAvailable,
    LogIndex syncIndex) noexcept
    : AppendEntriesResult(
          term, TRI_ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED,
          std::move(reason), id, snapshotAvailable, syncIndex) {
  static_assert(std::is_nothrow_move_constructible_v<AppendEntriesErrorReason>);
  this->conflict = conflict;
}

auto replicated_log::AppendEntriesResult::withConflict(
    LogTerm term, replicated_log::MessageId id, TermIndexPair conflict,
    bool snapshotAvailable, LogIndex syncIndex) noexcept
    -> replicated_log::AppendEntriesResult {
  return {term,
          id,
          conflict,
          {AppendEntriesErrorReason::ErrorType::kNoPrevLogMatch},
          snapshotAvailable,
          syncIndex};
}

auto replicated_log::AppendEntriesResult::withRejection(
    LogTerm term, MessageId id, AppendEntriesErrorReason reason,
    bool snapshotAvailable, LogIndex syncIndex) noexcept
    -> AppendEntriesResult {
  static_assert(std::is_nothrow_move_constructible_v<AppendEntriesErrorReason>);
  return {term,
          TRI_ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED,
          std::move(reason),
          id,
          snapshotAvailable,
          syncIndex};
}

auto replicated_log::AppendEntriesResult::withPersistenceError(
    LogTerm term, replicated_log::MessageId id, Result const& res,
    bool snapshotAvailable, LogIndex syncIndex) noexcept
    -> replicated_log::AppendEntriesResult {
  return {term,
          res.errorNumber(),
          {AppendEntriesErrorReason::ErrorType::kPersistenceFailure,
           std::string{res.errorMessage()}},
          id,
          snapshotAvailable,
          syncIndex};
}

auto replicated_log::AppendEntriesResult::withOk(LogTerm term,
                                                 replicated_log::MessageId id,
                                                 bool snapshotAvailable,
                                                 LogIndex syncIndex) noexcept
    -> replicated_log::AppendEntriesResult {
  return {term, id, snapshotAvailable, syncIndex};
}

auto replicated_log::AppendEntriesResult::isSuccess() const noexcept -> bool {
  return errorCode == TRI_ERROR_NO_ERROR;
}

void replicated_log::AppendEntriesRequest::toVelocyPack(
    velocypack::Builder& builder) const {
  {
    velocypack::ObjectBuilder ob(&builder);
    builder.add("leaderTerm", VPackValue(leaderTerm.value));
    builder.add("leaderId", VPackValue(leaderId));
    builder.add(VPackValue("prevLogEntry"));
    prevLogEntry.toVelocyPack(builder);
    builder.add("leaderCommit", VPackValue(leaderCommit.value));
    builder.add("lowestIndexToKeep", VPackValue(lowestIndexToKeep.value));
    builder.add("messageId", VPackValue(messageId));
    builder.add("waitForSync", VPackValue(waitForSync));
    builder.add("entries", VPackValue(VPackValueType::Array));
    for (auto const& it : entries) {
      it.entry().toVelocyPack(builder);
    }
    builder.close();  // close entries
  }
}

auto replicated_log::AppendEntriesRequest::fromVelocyPack(
    velocypack::Slice slice) -> AppendEntriesRequest {
  auto leaderTerm = slice.get("leaderTerm").extract<LogTerm>();
  auto leaderId = ParticipantId{slice.get("leaderId").copyString()};
  auto prevLogEntry = TermIndexPair::fromVelocyPack(slice.get("prevLogEntry"));
  auto leaderCommit = slice.get("leaderCommit").extract<LogIndex>();
  auto largestCommonIndex = slice.get("lowestIndexToKeep").extract<LogIndex>();
  auto messageId = slice.get("messageId").extract<MessageId>();
  auto waitForSync = slice.get("waitForSync").extract<bool>();
  auto entries = std::invoke([&] {
    auto entriesVp = velocypack::ArrayIterator(slice.get("entries"));
    auto transientEntries = EntryContainer::transient_type{};
    for (auto it : entriesVp) {
      transientEntries.push_back(
          InMemoryLogEntry(LogEntry::fromVelocyPack(it)));
    }
    return std::move(transientEntries).persistent();
  });

  return AppendEntriesRequest{leaderTerm,   leaderId,           prevLogEntry,
                              leaderCommit, largestCommonIndex, messageId,
                              waitForSync,  std::move(entries)};
}

replicated_log::AppendEntriesRequest::AppendEntriesRequest(
    LogTerm leaderTerm, ParticipantId leaderId, TermIndexPair prevLogEntry,
    LogIndex leaderCommit, LogIndex lowestIndexToKeep,
    replicated_log::MessageId messageId, bool waitForSync,
    EntryContainer entries)
    : leaderTerm(leaderTerm),
      leaderId(std::move(leaderId)),
      prevLogEntry(prevLogEntry),
      leaderCommit(leaderCommit),
      lowestIndexToKeep(lowestIndexToKeep),
      messageId(messageId),
      entries(std::move(entries)),
      waitForSync(waitForSync) {}
