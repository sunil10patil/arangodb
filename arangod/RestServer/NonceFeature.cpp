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

#include "NonceFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Nonce.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"

using namespace arangodb::basics;
using namespace arangodb::options;

namespace arangodb {

NonceFeature::NonceFeature(Server& server) : ArangodFeature{server, *this} {
  setOptional(true);
  startsAfter<application_features::GreetingsFeaturePhase>();
}

void NonceFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addSection("nonce", "nonces", "", true, true);
  options->addObsoleteOption("--nonce.size",
                             "the size of the hash array for nonces", true);
}

}  // namespace arangodb
