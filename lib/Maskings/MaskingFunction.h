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

#pragma once

#include "Basics/Common.h"

#include "Basics/Utf8Helper.h"

#include <velocypack/Builder.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace arangodb::maskings {
class Maskings;

class MaskingFunction {
 public:
  static bool isNameChar(UChar32 ch) {
    return u_isalpha(ch) || u_isdigit(ch) || ch == U'_' || ch == U'-';
  }

  explicit MaskingFunction(Maskings* maskings) : _maskings(maskings) {}
  virtual ~MaskingFunction() = default;

  // derived classes can specialize these functions
  // the default implementation is to add the original value!
  virtual void mask(bool, velocypack::Builder& out, std::string& buffer) const;
  virtual void mask(std::string_view, velocypack::Builder& out,
                    std::string& buffer) const;
  virtual void mask(int64_t, velocypack::Builder& out,
                    std::string& buffer) const;
  virtual void mask(double, velocypack::Builder& out,
                    std::string& buffer) const;

 protected:
  Maskings* _maskings;
};
}  // namespace arangodb::maskings
