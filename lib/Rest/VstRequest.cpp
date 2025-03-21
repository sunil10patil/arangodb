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

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "VstRequest.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Parser.h>
#include <velocypack/Validator.h>

#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/Utf8Helper.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/debugging.h"
#include "Endpoint/ConnectionInfo.h"
#include "Logger/LogMacros.h"
#include "Meta/conversion.h"
#include "Rest/CommonDefines.h"

using namespace arangodb;
using namespace arangodb::basics;

VstRequest::VstRequest(ConnectionInfo const& connectionInfo,
                       velocypack::Buffer<uint8_t> buffer, size_t payloadOffset,
                       uint64_t messageId)
    : GeneralRequest(connectionInfo, messageId),
      _payloadOffset(payloadOffset),
      _validatedPayload(false) {
  _contentType = ContentType::UNSET;  // intentional
  _contentTypeResponse = ContentType::VPACK;
  _payload = std::move(buffer);
  _memoryUsage += _payload.size();
  parseHeaderInformation();
  _memoryUsage += sizeof(VstRequest);
}

VstRequest::~VstRequest() {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto expected = sizeof(VstRequest) + _fullUrl.size() + _requestPath.size() +
                  _databaseName.size() + _user.size() + _prefix.size() +
                  _contentTypeResponsePlain.size() + _payload.size();
  for (auto const& it : _suffixes) {
    expected += it.size();
  }
  for (auto const& it : _headers) {
    expected += it.first.size() + it.second.size();
  }
  for (auto const& it : _values) {
    expected += it.first.size() + it.second.size();
  }
  for (auto const& it : _arrayValues) {
    expected += it.first.size();
    for (auto const& it2 : it.second) {
      expected += it2.size();
    }
  }
  if (_vpackBuilder) {
    expected += _vpackBuilder->bufferRef().size();
  }

  TRI_ASSERT(_memoryUsage == expected)
      << "expected memory usage: " << expected << ", actual: " << _memoryUsage
      << ", diff: " << (_memoryUsage - expected);
#endif
}

size_t VstRequest::contentLength() const noexcept {
  TRI_ASSERT(_payload.size() >= _payloadOffset);
  if (_payload.size() < _payloadOffset) {
    return 0;
  }
  return (_payload.size() - _payloadOffset);
}

std::string_view VstRequest::rawPayload() const {
  if (_payload.size() <= _payloadOffset) {
    return std::string_view();
  }
  return std::string_view(
      reinterpret_cast<char const*>(_payload.data() + _payloadOffset),
      _payload.size() - _payloadOffset);
}

VPackSlice VstRequest::payload(bool strictValidation) {
  if (_contentType == ContentType::JSON) {
    if (!_vpackBuilder && _payload.size() > _payloadOffset) {
      _vpackBuilder = VPackParser::fromJson(
          _payload.data() + _payloadOffset, _payload.size() - _payloadOffset,
          validationOptions(strictValidation));
      _memoryUsage += _vpackBuilder->bufferRef().size();
    }
    if (_vpackBuilder) {
      return _vpackBuilder->slice();
    }
  } else if ((_contentType == ContentType::UNSET) ||
             (_contentType == ContentType::VPACK)) {
    if (_payload.size() > _payloadOffset) {
      uint8_t const* ptr = _payload.data() + _payloadOffset;
      if (!_validatedPayload) {
        /// the header is validated in VstCommTask, the actual body is only
        /// validated on demand
        VPackOptions const* options = validationOptions(strictValidation);
        VPackValidator validator(options);
        // will throw on error
        _validatedPayload =
            validator.validate(ptr, _payload.size() - _payloadOffset);
      }
      return VPackSlice(ptr);
    }
  }
  return VPackSlice::noneSlice();  // no body
}

void VstRequest::setPayload(arangodb::velocypack::Buffer<uint8_t> buffer) {
  GeneralRequest::setPayload(std::move(buffer));
  _payloadOffset = 0;
}

void VstRequest::setHeader(VPackSlice keySlice, VPackSlice valSlice) {
  if (!keySlice.isString() || !valSlice.isString()) {
    return;
  }

  std::string key = keySlice.copyString();
  StringUtils::tolowerInPlace(key);
  std::string value = valSlice.copyString();

  if (key == StaticStrings::Accept) {
    StringUtils::tolowerInPlace(value);
    _contentTypeResponse = rest::stringToContentType(value, ContentType::VPACK);
    if (value.find(',') != std::string::npos) {
      setStringValue(_contentTypeResponsePlain, std::move(value));
    } else {
      setStringValue(_contentTypeResponsePlain, std::string());
    }
    return;  // don't insert this header!!
  } else if ((_contentType == ContentType::UNSET) &&
             (key == StaticStrings::ContentTypeHeader)) {
    StringUtils::tolowerInPlace(value);
    auto res = rest::stringToContentType(value, /*default*/ ContentType::UNSET);
    // simon: the "@arangodb/requests" module by default the "text/plain"
    // content-types for JSON in most tests. As soon as someone fixes all the
    // tests we can enable these again.
    if (res == ContentType::JSON || res == ContentType::VPACK ||
        res == ContentType::DUMP) {
      _contentType = res;
      return;  // don't insert this header!!
    }
  }

  auto memoryUsage = key.size() + value.size();
  auto it = _headers.try_emplace(std::move(key), std::move(value));
  if (!it.second) {
    auto old = it.first->first.size() + it.first->second.size();
    // cppcheck-suppress accessMoved
    _headers[std::move(key)] = std::move(value);
    _memoryUsage -= old;
  }
  _memoryUsage += memoryUsage;
}

void VstRequest::parseHeaderInformation() {
  /// the header was already validated here, the actual body was not
  VPackSlice vHeader(_payload.data());
  if (!vHeader.isArray() || vHeader.length() != 7) {
    LOG_TOPIC("0007b", WARN, Logger::COMMUNICATION)
        << "invalid VST message header";
    throw std::runtime_error("invalid VST message header");
  }

  try {
    TRI_ASSERT(vHeader.isArray());
    auto version = vHeader.at(0).getInt();  // version
    auto type = vHeader.at(1).getInt();     // type
    {
      VPackSlice dbName = vHeader.at(2);  // database
      setDatabaseName(dbName.copyString());
      if (_databaseName != normalizeUtf8ToNFC(_databaseName)) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_ARANGO_ILLEGAL_NAME,
            "database name is not properly UTF-8 NFC-normalized");
      }
    }
    _type = meta::toEnum<RequestType>(vHeader.at(3).getInt());  // request type
    setRequestPath(vHeader.at(4).copyString());  // request (path)
    VPackSlice params = vHeader.at(5);           // parameter
    VPackSlice meta = vHeader.at(6);             // meta

    if (version != 1) {
      LOG_TOPIC("e7fe5", WARN, Logger::COMMUNICATION)
          << "invalid version in vst message";
    }
    if (type != 1) {
      LOG_TOPIC("d8a18", WARN, Logger::COMMUNICATION) << "not a VST request";
      return;
    }

    for (auto it : VPackObjectIterator(params, true)) {
      if (it.value.isArray()) {
        for (auto itInner : VPackArrayIterator(it.value)) {
          setArrayValue(it.key.copyString(), itInner.copyString());
        }
      } else {
        setValue(it.key.copyString(), it.value.copyString());
      }
    }

    for (auto it : VPackObjectIterator(meta, true)) {
      setHeader(it.key, it.value);
    }

    // fullUrl should not be necessary for Vst
    auto old = _fullUrl.size();
    _fullUrl = _requestPath;
    _fullUrl.push_back('?');  // intentional
    for (auto const& param : _values) {
      _fullUrl.append(param.first + "=" +
                      basics::StringUtils::urlEncode(param.second) + "&");
    }

    for (auto const& param : _arrayValues) {
      for (auto const& value : param.second) {
        _fullUrl.append(param.first);
        _fullUrl.append("[]=");
        _fullUrl.append(basics::StringUtils::urlEncode(value));
        _fullUrl.push_back('&');
      }
    }
    _fullUrl.pop_back();  // remove last & or ?
    _memoryUsage += _fullUrl.size();
    TRI_ASSERT(_memoryUsage >= old);
    _memoryUsage -= old;
  } catch (std::exception const& e) {
    throw std::runtime_error(
        std::string("Error during Parsing of VstHeader: ") + e.what());
  }
}
