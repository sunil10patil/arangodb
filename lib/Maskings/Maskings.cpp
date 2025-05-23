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

#include <stdint.h>
#include <iostream>
#include <string_view>

#include <velocypack/Builder.h>
#include <velocypack/Dumper.h>
#include <velocypack/Exception.h>
#include <velocypack/Iterator.h>
#include <velocypack/Options.h>
#include <velocypack/Parser.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-common.h>

#include "Maskings.h"

#include "Basics/FileUtils.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringBuffer.h"
#include "Basics/VPackStringBufferAdapter.h"
#include "Basics/debugging.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Maskings/CollectionSelection.h"
#include "Maskings/MaskingFunction.h"
#include "Random/RandomGenerator.h"

using namespace arangodb;
using namespace arangodb::maskings;

MaskingsResult Maskings::fromFile(std::string const& filename) {
  std::string definition;

  try {
    definition = basics::FileUtils::slurp(filename);
  } catch (std::exception const& e) {
    std::string msg =
        "cannot read maskings file '" + filename + "': " + e.what();
    LOG_TOPIC("379fe", DEBUG, Logger::CONFIG) << msg;

    return MaskingsResult(MaskingsResult::CANNOT_READ_FILE, msg);
  }

  LOG_TOPIC("fe73b", DEBUG, Logger::CONFIG)
      << "found maskings file '" << filename;

  if (definition.empty()) {
    std::string msg = "maskings file '" + filename + "' is empty";
    LOG_TOPIC("5018d", DEBUG, Logger::CONFIG) << msg;
    return MaskingsResult(MaskingsResult::CANNOT_READ_FILE, msg);
  }

  auto maskings = std::make_unique<Maskings>();

  maskings.get()->_randomSeed = RandomGenerator::interval(UINT64_MAX);

  try {
    std::shared_ptr<VPackBuilder> parsed =
        velocypack::Parser::fromJson(definition);

    ParseResult<Maskings> res = maskings->parse(parsed->slice());

    if (res.status != ParseResult<Maskings>::VALID) {
      return MaskingsResult(MaskingsResult::ILLEGAL_DEFINITION, res.message);
    }

    return MaskingsResult(std::move(maskings));
  } catch (velocypack::Exception const& e) {
    std::string msg =
        "cannot parse maskings file '" + filename + "': " + e.what();
    LOG_TOPIC("5cb4c", DEBUG, Logger::CONFIG)
        << msg << ". file content: " << definition;

    return MaskingsResult(MaskingsResult::CANNOT_PARSE_FILE, msg);
  }
}

ParseResult<Maskings> Maskings::parse(velocypack::Slice def) {
  if (!def.isObject()) {
    return ParseResult<Maskings>(ParseResult<Maskings>::DUPLICATE_COLLECTION,
                                 "expecting an object for masking definition");
  }

  for (auto const& entry : VPackObjectIterator(def, false)) {
    std::string key = entry.key.copyString();

    if (key == "*") {
      LOG_TOPIC("b0d99", TRACE, Logger::CONFIG) << "default masking";

      if (_hasDefaultCollection) {
        return ParseResult<Maskings>(
            ParseResult<Maskings>::DUPLICATE_COLLECTION,
            "duplicate default entry");
      }
    } else {
      LOG_TOPIC("f5aac", TRACE, Logger::CONFIG)
          << "masking collection '" << key << "'";

      if (_collections.find(key) != _collections.end()) {
        return ParseResult<Maskings>(
            ParseResult<Maskings>::DUPLICATE_COLLECTION,
            "duplicate collection entry '" + key + "'");
      }
    }

    ParseResult<Collection> c = Collection::parse(this, entry.value);

    if (c.status != ParseResult<Collection>::VALID) {
      return ParseResult<Maskings>(
          (ParseResult<Maskings>::StatusCode)(int)c.status, c.message);
    }

    if (key == "*") {
      _hasDefaultCollection = true;
      _defaultCollection = c.result;
    } else {
      _collections[key] = c.result;
    }
  }

  return ParseResult<Maskings>(ParseResult<Maskings>::VALID);
}

bool Maskings::shouldDumpStructure(std::string const& name) {
  CollectionSelection select = CollectionSelection::EXCLUDE;
  auto const itr = _collections.find(name);

  if (itr == _collections.end()) {
    if (_hasDefaultCollection) {
      select = _defaultCollection.selection();
    }
  } else {
    select = itr->second.selection();
  }

  switch (select) {
    case CollectionSelection::FULL:
      return true;
    case CollectionSelection::MASKED:
      return true;
    case CollectionSelection::EXCLUDE:
      return false;
    case CollectionSelection::STRUCTURE:
      return true;
  }

  // should not get here. however, compiler warns about it
  TRI_ASSERT(false);
  return false;
}

bool Maskings::shouldDumpData(std::string const& name) {
  CollectionSelection select = CollectionSelection::EXCLUDE;
  auto const itr = _collections.find(name);

  if (itr == _collections.end()) {
    if (_hasDefaultCollection) {
      select = _defaultCollection.selection();
    }
  } else {
    select = itr->second.selection();
  }

  switch (select) {
    case CollectionSelection::FULL:
      return true;
    case CollectionSelection::MASKED:
      return true;
    case CollectionSelection::EXCLUDE:
      return false;
    case CollectionSelection::STRUCTURE:
      return false;
  }

  // should not get here. however, compiler warns about it
  TRI_ASSERT(false);
  return false;
}

void Maskings::maskedItem(Collection const& collection,
                          std::vector<std::string_view>& path,
                          velocypack::Slice data, velocypack::Builder& out,
                          std::string& buffer) const {
  if (path.size() == 1 && path[0].starts_with('_')) {
    if (data.isString()) {
      out.add(data);
      return;
    } else if (data.isInteger()) {
      out.add(data);
      return;
    }
  }

  MaskingFunction* func = collection.masking(path);

  if (func == nullptr) {
    if (data.isBool() || data.isString() || data.isInteger() ||
        data.isDouble()) {
      out.add(data);
      return;
    }
  } else {
    if (data.isBool()) {
      func->mask(data.getBool(), out, buffer);
      return;
    } else if (data.isString()) {
      func->mask(data.stringView(), out, buffer);
      return;
    } else if (data.isInteger()) {
      func->mask(data.getInt(), out, buffer);
      return;
    } else if (data.isDouble()) {
      func->mask(data.getDouble(), out, buffer);
      return;
    }
  }

  out.add(VPackValue(VPackValueType::Null));
}

void Maskings::addMaskedArray(Collection const& collection,
                              std::vector<std::string_view>& path,
                              velocypack::Slice data, VPackBuilder& out,
                              std::string& buffer) const {
  for (VPackSlice value : VPackArrayIterator(data)) {
    if (value.isObject()) {
      VPackObjectBuilder ob(&out);
      addMaskedObject(collection, path, value, out, buffer);
    } else if (value.isArray()) {
      VPackArrayBuilder ap(&out);
      addMaskedArray(collection, path, value, out, buffer);
    } else {
      maskedItem(collection, path, value, out, buffer);
    }
  }
}

void Maskings::addMaskedObject(Collection const& collection,
                               std::vector<std::string_view>& path,
                               velocypack::Slice data, VPackBuilder& out,
                               std::string& buffer) const {
  for (auto entry : VPackObjectIterator(data, false)) {
    auto key = entry.key.stringView();
    velocypack::Slice value = entry.value;

    path.push_back(key);

    if (value.isObject()) {
      VPackObjectBuilder ob(&out, key);
      addMaskedObject(collection, path, value, out, buffer);
    } else if (value.isArray()) {
      VPackArrayBuilder ap(&out, key);
      addMaskedArray(collection, path, value, out, buffer);
    } else {
      out.add(VPackValue(key));
      maskedItem(collection, path, value, out, buffer);
    }

    path.pop_back();
  }
}

void Maskings::addMasked(Collection const& collection, VPackBuilder& out,
                         velocypack::Slice data) const {
  if (!data.isObject()) {
    return;
  }

  std::string buffer;
  std::vector<std::string_view> path;
  std::string_view dataStr("data");
  VPackObjectBuilder ob(&out, dataStr);

  addMaskedObject(collection, path, data, out, buffer);
}

void Maskings::addMasked(Collection const& collection,
                         basics::StringBuffer& data,
                         velocypack::Slice slice) const {
  if (!slice.isObject()) {
    return;
  }

  VPackBuilder builder;

  if (slice.hasKey(StaticStrings::KeyString)) {
    // non-enveloped format - the document is at the top level
    {
      VPackObjectBuilder ob(&builder);
      addMasked(collection, builder, slice);
    }

    // the maskings will generate a result object that contains a "data"
    // attribute at the top
    slice = builder.slice().get("data");
  } else {
    // enveloped format -  the document is underneath the "data" attribute
    std::string_view dataStr("data");

    {
      VPackObjectBuilder ob(&builder);

      for (auto entry : VPackObjectIterator(slice, false)) {
        auto key = entry.key.stringView();

        if (key == dataStr) {
          addMasked(collection, builder, entry.value);
        } else {
          builder.add(key, entry.value);
        }
      }
    }

    slice = builder.slice();
  }

  // directly emit JSON into result StringBuffer
  basics::VPackStringBufferAdapter adapter(data.stringBuffer());
  VPackDumper dumper(&adapter, &VPackOptions::Defaults);
  dumper.dump(slice);

  data.appendChar('\n');
}

void Maskings::mask(std::string const& name, basics::StringBuffer const& data,
                    basics::StringBuffer& result) const {
  result.clear();

  Collection const* collection;
  auto const itr = _collections.find(name);

  if (itr == _collections.end()) {
    if (_hasDefaultCollection) {
      collection = &_defaultCollection;
    } else {
      result.copy(data);
      return;
    }
  } else {
    collection = &(itr->second);
  }

  if (collection->selection() == CollectionSelection::FULL) {
    result.copy(data);
    return;
  }

  result.reserve(data.length());

  char const* p = data.c_str();
  char const* e = p + data.length();
  char const* q = p;

  while (p < e) {
    while (p < e && (*p != '\n' && *p != '\r')) {
      ++p;
    }

    std::shared_ptr<VPackBuilder> builder = VPackParser::fromJson(q, p - q);

    addMasked(*collection, result, builder->slice());

    while (p < e && (*p == '\n' || *p == '\r')) {
      ++p;
    }

    q = p;
  }
}
