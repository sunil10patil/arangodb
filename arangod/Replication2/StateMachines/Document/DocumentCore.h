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
/// @author Alexandru Petenchea
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Replication2/StateMachines/Document/DocumentStateMachine.h"

#include "Replication2/LoggerContext.h"
#include "Replication2/ReplicatedLog/LogCommon.h"

struct TRI_vocbase_t;

namespace arangodb::replication2::replicated_state::document {

struct IDocumentStateShardHandler;
struct IDocumentStateTransactionHandler;

struct DocumentCore {
  explicit DocumentCore(
      TRI_vocbase_t& vocbase, GlobalLogIdentifier gid,
      DocumentCoreParameters coreParameters,
      std::shared_ptr<IDocumentStateHandlersFactory> const& handlersFactory,
      LoggerContext loggerContext);

  GlobalLogIdentifier const gid;
  LoggerContext const loggerContext;

  auto getVocbase() -> TRI_vocbase_t&;
  void drop();
  auto getShardHandler() -> std::shared_ptr<IDocumentStateShardHandler>;

 private:
  TRI_vocbase_t& _vocbase;
  DocumentCoreParameters _params;
  std::shared_ptr<IDocumentStateShardHandler> _shardHandler;
};
}  // namespace arangodb::replication2::replicated_state::document
