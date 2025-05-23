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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Rest/GeneralResponse.h"

#include <cstdint>

#include <velocypack/Buffer.h>

namespace arangodb {
namespace velocypack {
struct Options;
class Slice;
}  // namespace velocypack

class VstResponse : public GeneralResponse {
 public:
  VstResponse(ResponseCode code, uint64_t mid);

  bool isResponseEmpty() const override { return _payload.empty(); }

  virtual Endpoint::TransportType transportType() override {
    return Endpoint::TransportType::VST;
  }

  void reset(ResponseCode code) override final;
  void addPayload(velocypack::Slice slice, velocypack::Options const* = nullptr,
                  bool resolveExternals = true) override;
  void addPayload(velocypack::Buffer<uint8_t>&&,
                  velocypack::Options const* = nullptr,
                  bool resolveExternals = true) override;
  void addRawPayload(std::string_view payload) override;

  velocypack::Buffer<uint8_t>& payload() { return _payload; }

  bool isCompressionAllowed() override { return false; }
  ErrorCode deflate() override;
  ErrorCode gzip() override;

  /// write VST response message header
  void writeMessageHeader(velocypack::Buffer<uint8_t>&) const;

 private:
  velocypack::Buffer<uint8_t> _payload;  /// actual payload
};
}  // namespace arangodb
