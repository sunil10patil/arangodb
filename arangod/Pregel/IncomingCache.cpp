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

#include "IncomingCache.h"
#include "Pregel/Utils.h"
#include "Pregel/Worker/Messages.h"
#include "Pregel/SenderMessage.h"

#include "Pregel/Algos/ColorPropagation/ColorPropagationValue.h"
#include "Pregel/Algos/DMID/DMIDMessage.h"
#include "Pregel/Algos/EffectiveCloseness/HLLCounter.h"

#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"

#include <velocypack/Iterator.h>

#include <algorithm>
#include <random>
#include <thread>

using namespace arangodb;
using namespace arangodb::pregel;
using namespace arangodb::pregel::algos;

template<typename M>
InCache<M>::InCache(MessageFormat<M> const* format)
    : _containedMessageCount(0), _format(format) {}

template<typename M>
void InCache<M>::parseMessages(worker::message::PregelMessage const& message) {
  // every packet contains one shard
  VPackValueLength i = 0;
  std::string_view key;
  std::lock_guard<std::mutex> guard(this->_bucketLocker[message.shard]);

  for (VPackSlice current : VPackArrayIterator(message.messages.slice())) {
    if (i % 2 == 0) {  // TODO support multiple recipients
      key = current.stringView();
    } else {
      TRI_ASSERT(!key.empty());
      if (current.isArray()) {
        VPackValueLength c = 0;
        for (VPackSlice val : VPackArrayIterator(current)) {
          M newValue;
          _format->unwrapValue(val, newValue);
          _set(message.shard, key, newValue);
          c++;
        }
        this->_containedMessageCount += c;
      } else {
        M newValue;
        _format->unwrapValue(current, newValue);
        _set(message.shard, key, newValue);
        this->_containedMessageCount++;
      }
    }
    i++;
  }

  if (i % 2 != 0) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "There must always be a multiple of 2 entries in message array");
  }
}

template<typename M>
void InCache<M>::storeMessageNoLock(PregelShard shard,
                                    std::string_view vertexId, M const& data) {
  this->_set(shard, vertexId, data);
  this->_containedMessageCount++;
}

template<typename M>
void InCache<M>::storeMessage(PregelShard shard, std::string_view vertexId,
                              M const& data) {
  std::lock_guard<std::mutex> guard(this->_bucketLocker[shard]);
  this->_set(shard, vertexId, data);
  this->_containedMessageCount++;
}

// ================== ArrayIncomingCache ==================

template<typename M>
ArrayInCache<M>::ArrayInCache(containers::FlatHashSet<PregelShard> localShards,
                              MessageFormat<M> const* format)
    : InCache<M>(format), _localShards(std::move(localShards)) {
  // one mutex per shard, we will see how this scales
  for (PregelShard const& pregelShard : _localShards) {
    this->_bucketLocker[pregelShard];
    _shardMap[pregelShard];
  }
}

template<typename M>
void ArrayInCache<M>::_set(PregelShard shard, std::string_view const& key,
                           M const& newValue) {
  HMap& vertexMap(_shardMap[shard]);
  vertexMap[std::string(key)].push_back(newValue);
}

template<typename M>
void ArrayInCache<M>::mergeCache(InCache<M> const* otherCache) {
  ArrayInCache<M>* other = (ArrayInCache<M>*)otherCache;
  this->_containedMessageCount += other->_containedMessageCount;

  // ranomize access to buckets, don't wait for the lock
  std::vector<PregelShard> randomized(_localShards.begin(), _localShards.end());

  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(randomized.begin(), randomized.end(), g);

  size_t i = 0;
  do {
    i = (i + 1) % randomized.size();
    PregelShard shardId = randomized[i];

    auto const& it = other->_shardMap.find(shardId);
    if (it != other->_shardMap.end() && it->second.size() > 0) {
      std::unique_lock<std::mutex> guard(this->_bucketLocker[shardId],
                                         std::try_to_lock);

      if (!guard) {
        if (i == 0) {  // eventually we hit the last one
          std::this_thread::sleep_for(
              std::chrono::microseconds(100));  // don't busy wait
        }
        continue;
      }

      // only access bucket after we acquired the lock
      HMap& myVertexMap = _shardMap[shardId];
      for (auto& vertexMessage : it->second) {
        std::vector<M>& a = myVertexMap[vertexMessage.first];
        std::vector<M> const& b = vertexMessage.second;
        a.insert(a.end(), b.begin(), b.end());
      }
    }

    randomized.erase(randomized.begin() + i);
  } while (randomized.size() > 0);
}

template<typename M>
MessageIterator<M> ArrayInCache<M>::getMessages(PregelShard shard,
                                                std::string_view const& key) {
  std::string keyS = std::string(key);
  HMap const& vertexMap = _shardMap[shard];
  auto vmsg = vertexMap.find(keyS);
  if (vmsg != vertexMap.end()) {
    M const* ptr = vmsg->second.data();
    return MessageIterator<M>(ptr, vmsg->second.size());
  } else {
    return MessageIterator<M>();
  }
}

template<typename M>
void ArrayInCache<M>::clear() {
  for (auto& pair : _shardMap) {  // keep the keys
    // std::lock_guard guard{this->_bucketLocker[pair.first]};
    pair.second.clear();
  }
  this->_containedMessageCount = 0;
}

/// Deletes one entry. DOES NOT LOCK
template<typename M>
void ArrayInCache<M>::erase(PregelShard shard, std::string_view const& key) {
  std::string keyS = std::string(key);
  HMap& vertexMap = _shardMap[shard];
  auto const& it = vertexMap.find(keyS);
  if (it != vertexMap.end()) {
    vertexMap.erase(it);
    this->_containedMessageCount--;
  }
}

template<typename M>
void ArrayInCache<M>::forEach(
    std::function<void(PregelShard, std::string_view const&, M const&)> func) {
  for (auto const& pair : _shardMap) {
    PregelShard shard = pair.first;
    HMap const& vertexMap = pair.second;
    for (auto& vertexMsgs : vertexMap) {
      for (M const& val : vertexMsgs.second) {
        func(shard, std::string_view(vertexMsgs.first), val);
      }
    }
  }
}

// ================== CombiningIncomingCache ==================

template<typename M>
CombiningInCache<M>::CombiningInCache(
    containers::FlatHashSet<PregelShard> localShards,
    MessageFormat<M> const* format, MessageCombiner<M> const* combiner)
    : InCache<M>(format), _combiner(combiner), _localShards(localShards) {
  // one mutex per shard, we will see how this scales
  for (PregelShard pregelShard : localShards) {
    this->_bucketLocker[pregelShard];
    _shardMap[pregelShard];
  }
}

template<typename M>
void CombiningInCache<M>::_set(PregelShard shard, std::string_view const& key,
                               M const& newValue) {
  std::string keyS = std::string(key);
  HMap& vertexMap = _shardMap[shard];
  auto vmsg = vertexMap.find(keyS);
  if (vmsg != vertexMap.end()) {  // got a message for the same vertex
    _combiner->combine(vmsg->second, newValue);
  } else {
    vertexMap.insert(std::make_pair(std::move(keyS), newValue));
  }
}

template<typename M>
void CombiningInCache<M>::mergeCache(InCache<M> const* otherCache) {
  CombiningInCache<M>* other = (CombiningInCache<M>*)otherCache;
  this->_containedMessageCount += other->_containedMessageCount;

  if (this->_containedMessageCount == 0) {
    return;
  }

  // randomize access to buckets, don't wait for the lock
  std::vector<PregelShard> randomized(_localShards.begin(), _localShards.end());
  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(randomized.begin(), randomized.end(), g);

  size_t i = 0;
  do {
    i = (i + 1) % randomized.size();
    PregelShard shardId = randomized[i];

    auto const& it = other->_shardMap.find(shardId);
    if (it != other->_shardMap.end() && it->second.size() > 0) {
      std::unique_lock<std::mutex> guard(this->_bucketLocker[shardId],
                                         std::try_to_lock);

      if (!guard) {
        if (i == 0) {  // eventually we hit the last one
          std::this_thread::sleep_for(
              std::chrono::microseconds(100));  // don't busy wait
        }
        continue;
      }

      // only access bucket after we acquired the lock
      HMap& myVertexMap = _shardMap[shardId];
      for (auto& vertexMessage : it->second) {
        auto vmsg = myVertexMap.find(vertexMessage.first);
        if (vmsg != myVertexMap.end()) {  // got a message for the same vertex
          _combiner->combine(vmsg->second, vertexMessage.second);
        } else {
          myVertexMap.insert(vertexMessage);
        }
      }
    }

    randomized.erase(randomized.begin() + i);
  } while (randomized.size() > 0);
}

template<typename M>
MessageIterator<M> CombiningInCache<M>::getMessages(
    PregelShard shard, std::string_view const& key) {
  std::string keyS = std::string(key);
  HMap const& vertexMap = _shardMap[shard];
  auto vmsg = vertexMap.find(keyS);
  if (vmsg != vertexMap.end()) {
    return MessageIterator<M>(&vmsg->second);
  } else {
    return MessageIterator<M>();
  }
}

template<typename M>
void CombiningInCache<M>::clear() {
  for (auto& pair : _shardMap) {
    pair.second.clear();
  }
  this->_containedMessageCount = 0;
}

/// Deletes one entry. DOES NOT LOCK
template<typename M>
void CombiningInCache<M>::erase(PregelShard shard,
                                std::string_view const& key) {
  std::string keyS = std::string(key);
  HMap& vertexMap = _shardMap[shard];
  auto const& it = vertexMap.find(keyS);
  if (it != vertexMap.end()) {
    vertexMap.erase(it);
    this->_containedMessageCount--;
  }
}

/// Calls function for each entry. DOES NOT LOCK
template<typename M>
void CombiningInCache<M>::forEach(
    std::function<void(PregelShard shard, std::string_view const& key,
                       M const&)>
        func) {
  for (auto const& pair : _shardMap) {
    PregelShard shard = pair.first;
    HMap const& vertexMap = pair.second;
    for (auto& vertexMessage : vertexMap) {
      func(shard, std::string_view(vertexMessage.first), vertexMessage.second);
    }
  }
}

// template types to create
template class arangodb::pregel::InCache<int64_t>;
template class arangodb::pregel::InCache<uint64_t>;
template class arangodb::pregel::InCache<float>;
template class arangodb::pregel::InCache<double>;
template class arangodb::pregel::ArrayInCache<int64_t>;
template class arangodb::pregel::ArrayInCache<uint64_t>;
template class arangodb::pregel::ArrayInCache<float>;
template class arangodb::pregel::ArrayInCache<double>;
template class arangodb::pregel::CombiningInCache<int64_t>;
template class arangodb::pregel::CombiningInCache<uint64_t>;
template class arangodb::pregel::CombiningInCache<float>;
template class arangodb::pregel::CombiningInCache<double>;

// algo specific
template class arangodb::pregel::InCache<SenderMessage<uint64_t>>;
template class arangodb::pregel::ArrayInCache<SenderMessage<uint64_t>>;
template class arangodb::pregel::CombiningInCache<SenderMessage<uint64_t>>;

template class arangodb::pregel::InCache<SenderMessage<double>>;
template class arangodb::pregel::ArrayInCache<SenderMessage<double>>;
template class arangodb::pregel::CombiningInCache<SenderMessage<double>>;

template class arangodb::pregel::InCache<DMIDMessage>;
template class arangodb::pregel::ArrayInCache<DMIDMessage>;
template class arangodb::pregel::CombiningInCache<DMIDMessage>;

template class arangodb::pregel::InCache<HLLCounter>;
template class arangodb::pregel::ArrayInCache<HLLCounter>;
template class arangodb::pregel::CombiningInCache<HLLCounter>;

template class arangodb::pregel::InCache<ColorPropagationMessageValue>;
template class arangodb::pregel::ArrayInCache<ColorPropagationMessageValue>;
template class arangodb::pregel::CombiningInCache<ColorPropagationMessageValue>;
