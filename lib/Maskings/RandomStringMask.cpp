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
/// @author Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "RandomStringMask.h"

#include "Basics/StringUtils.h"
#include "Basics/fasthash.h"
#include "Maskings/Maskings.h"

#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

#include <memory>

using namespace arangodb;
using namespace arangodb::maskings;

ParseResult<AttributeMasking> RandomStringMask::create(
    Path path, Maskings* maskings, velocypack::Slice /*def*/) {
  return ParseResult<AttributeMasking>(AttributeMasking(
      std::move(path), std::make_shared<RandomStringMask>(maskings)));
}

void RandomStringMask::mask(std::string_view data, velocypack::Builder& out,
                            std::string& buffer) const {
  uint64_t len = data.size();
  uint64_t hash;

  hash = fasthash64(data.data(), data.size(), _maskings->randomSeed());

  std::string hash64 = basics::StringUtils::encodeBase64(
      std::string((char const*)&hash, sizeof(decltype(hash))));

  buffer.clear();
  buffer.reserve(len);
  buffer.append(hash64);

  if (buffer.size() < len) {
    while (buffer.size() < len) {
      buffer.append(hash64);
    }

    buffer.resize(len);
  }

  out.add(VPackValue(buffer));
}
