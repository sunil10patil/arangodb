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

#include <map>
#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include "Pregel/Utils.h"
#include "Logger/LogMacros.h"

namespace arangodb {
namespace pregel {

struct MessageStats {
  size_t sendCount = 0;
  size_t receivedCount = 0;
  size_t memoryBytesUsedForMessages = 0;
  double superstepRuntimeSecs = 0;

  MessageStats() = default;
  MessageStats(size_t s, size_t r, size_t memoryBytesUsedForMessages)
      : sendCount(s),
        receivedCount(r),
        memoryBytesUsedForMessages(memoryBytesUsedForMessages) {}

  void accumulate(MessageStats const& other) {
    sendCount += other.sendCount;
    receivedCount += other.receivedCount;
    superstepRuntimeSecs += other.superstepRuntimeSecs;
  }

  void serializeValues(VPackBuilder& b) const {
    b.add(Utils::sendCountKey, VPackValue(sendCount));
    b.add(Utils::receivedCountKey, VPackValue(receivedCount));
  }

  void reset() {
    sendCount = 0;
    receivedCount = 0;
    superstepRuntimeSecs = 0;
  }
};
template<typename Inspector>
auto inspect(Inspector& f, MessageStats& x) {
  return f.object(x).fields(
      f.field("sendCount", x.sendCount),
      f.field("receivedCount", x.receivedCount),
      f.field("superstepRuntimeInSeconds", x.superstepRuntimeSecs));
}

struct StatsManager {
  void accumulateActiveCounts(VPackSlice data) {
    VPackSlice sender = data.get(Utils::senderKey);
    if (sender.isString()) {
      VPackSlice active = data.get(Utils::activeCountKey);
      if (active.isInteger()) {
        _activeStats[sender.copyString()] += active.getUInt();
      }
    }
  }

  void accumulateActiveCounts(std::string const& sender, uint64_t active) {
    _activeStats[sender] += active;
  }

  void accumulateMessageStats(std::string const& sender,
                              MessageStats const& stats) {
    _serverStats[sender].accumulate(stats);
  }

  void serializeValues(VPackBuilder& b) const {
    MessageStats stats;
    for (auto const& pair : _serverStats) {
      stats.accumulate(pair.second);
    }
    stats.serializeValues(b);
  }

  /// Test if all messages were processed
  bool allMessagesProcessed() const {
    uint64_t send = 0, received = 0;
    for (auto const& pair : _serverStats) {
      send += pair.second.sendCount;
      received += pair.second.receivedCount;
    }
    return send == received;
  }

  void debugOutput() const {
    uint64_t send = 0, received = 0;
    for (auto const& pair : _serverStats) {
      send += pair.second.sendCount;
      received += pair.second.receivedCount;
    }
    LOG_TOPIC("26dad", TRACE, Logger::PREGEL)
        << send << " - " << received << " : " << send - received;
  }

  /// tests if active count is greater 0
  bool noActiveVertices() const {
    for (auto const& pair : _activeStats) {
      if (pair.second > 0) {
        return false;
      }
    }
    return true;
  }

  void resetActiveCount() {
    for (auto& pair : _activeStats) {
      pair.second = 0;
    }
  }

  void reset() { _serverStats.clear(); }

  size_t clientCount() const { return _serverStats.size(); }

 private:
  std::map<std::string, uint64_t> _activeStats;
  std::map<std::string, MessageStats> _serverStats;
};
}  // namespace pregel
}  // namespace arangodb
