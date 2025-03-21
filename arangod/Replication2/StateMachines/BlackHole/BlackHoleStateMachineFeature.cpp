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

#include "BlackHoleStateMachineFeature.h"
#include "BlackHoleStateMachine.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Replication2/ReplicatedState/ReplicatedStateFeature.h"

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_state;
using namespace arangodb::replication2::replicated_state::black_hole;

BlackHoleStateMachineFeature::BlackHoleStateMachineFeature(Server& server)
    : ArangodFeature{server, *this} {
  startsAfter<ReplicatedStateAppFeature>();
  onlyEnabledWith<ReplicatedStateAppFeature>();
  setOptional(true);
}

void BlackHoleStateMachineFeature::prepare() {
  auto& feature = server().getFeature<ReplicatedStateAppFeature>();
  feature.registerStateType<BlackHoleState>("black-hole");
}
