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

namespace arangodb::replication2::replicated_state {

template<typename T>
struct EntryDeserializer {};
template<typename T>
struct EntrySerializer {};

template<typename S>
struct ReplicatedStateTraits {
  using FactoryType = typename S::FactoryType;
  using LeaderType = typename S::LeaderType;
  using FollowerType = typename S::FollowerType;
  using EntryType = typename S::EntryType;
  using CoreType = typename S::CoreType;
  using Deserializer = EntryDeserializer<EntryType>;
  using Serializer = EntrySerializer<EntryType>;
  using CleanupHandlerType = typename S::CleanupHandlerType;
};

}  // namespace arangodb::replication2::replicated_state
