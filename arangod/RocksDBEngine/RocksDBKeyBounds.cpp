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
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBKeyBounds.h"
#include "Basics/Exceptions.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBFormat.h"
#include "RocksDBEngine/RocksDBTypes.h"

#include <iostream>

using namespace arangodb;
using namespace arangodb::rocksutils;
using namespace arangodb::velocypack;

namespace {
constexpr char kStringSeparator = '\0';
}

RocksDBKeyBounds RocksDBKeyBounds::Empty() {
  return RocksDBKeyBounds::PrimaryIndex(0);
}

RocksDBKeyBounds RocksDBKeyBounds::Databases() {
  return RocksDBKeyBounds(RocksDBEntryType::Database);
}

RocksDBKeyBounds RocksDBKeyBounds::DatabaseCollections(
    TRI_voc_tick_t databaseId) {
  return RocksDBKeyBounds(RocksDBEntryType::Collection, databaseId);
}

RocksDBKeyBounds RocksDBKeyBounds::CollectionDocuments(
    uint64_t collectionObjectId) {
  return RocksDBKeyBounds(RocksDBEntryType::Document, collectionObjectId);
}

RocksDBKeyBounds RocksDBKeyBounds::CollectionDocuments(
    uint64_t collectionObjectId, uint64_t lower, uint64_t upper) {
  return RocksDBKeyBounds(RocksDBEntryType::Document, collectionObjectId, lower,
                          upper);
}

RocksDBKeyBounds RocksDBKeyBounds::PrimaryIndex(uint64_t indexId) {
  return RocksDBKeyBounds(RocksDBEntryType::PrimaryIndexValue, indexId);
}

RocksDBKeyBounds RocksDBKeyBounds::EdgeIndex(uint64_t indexId) {
  return RocksDBKeyBounds(RocksDBEntryType::EdgeIndexValue, indexId);
}

RocksDBKeyBounds RocksDBKeyBounds::EdgeIndexVertex(uint64_t indexId,
                                                   std::string_view vertexId) {
  return RocksDBKeyBounds(RocksDBEntryType::EdgeIndexValue, indexId, vertexId);
}

RocksDBKeyBounds RocksDBKeyBounds::VPackIndex(uint64_t indexId, bool reverse) {
  return RocksDBKeyBounds(RocksDBEntryType::VPackIndexValue, indexId, reverse);
}

RocksDBKeyBounds RocksDBKeyBounds::UniqueVPackIndex(uint64_t indexId,
                                                    bool reverse) {
  return RocksDBKeyBounds(RocksDBEntryType::UniqueVPackIndexValue, indexId,
                          reverse);
}

RocksDBKeyBounds RocksDBKeyBounds::FulltextIndex(uint64_t indexId) {
  return RocksDBKeyBounds(RocksDBEntryType::FulltextIndexValue, indexId);
}

RocksDBKeyBounds RocksDBKeyBounds::LegacyGeoIndex(uint64_t indexId) {
  return RocksDBKeyBounds(RocksDBEntryType::LegacyGeoIndexValue, indexId);
}

RocksDBKeyBounds RocksDBKeyBounds::GeoIndex(uint64_t indexId) {
  return RocksDBKeyBounds(RocksDBEntryType::GeoIndexValue, indexId);
}

RocksDBKeyBounds RocksDBKeyBounds::GeoIndex(uint64_t indexId, uint64_t minCell,
                                            uint64_t maxCell) {
  return RocksDBKeyBounds(RocksDBEntryType::GeoIndexValue, indexId, minCell,
                          maxCell);
}

RocksDBKeyBounds RocksDBKeyBounds::VPackIndex(uint64_t indexId, VPackSlice left,
                                              VPackSlice right) {
  return RocksDBKeyBounds(RocksDBEntryType::VPackIndexValue, indexId, left,
                          right);
}

RocksDBKeyBounds RocksDBKeyBounds::ZkdIndex(uint64_t indexId) {
  return RocksDBKeyBounds(RocksDBEntryType::ZkdIndexValue, indexId, false);
}

/// used for seeking lookups
RocksDBKeyBounds RocksDBKeyBounds::UniqueVPackIndex(uint64_t indexId,
                                                    VPackSlice left,
                                                    VPackSlice right) {
  return RocksDBKeyBounds(RocksDBEntryType::UniqueVPackIndexValue, indexId,
                          left, right);
}

RocksDBKeyBounds RocksDBKeyBounds::PrimaryIndex(uint64_t indexId,
                                                std::string const& left,
                                                std::string const& right) {
  return RocksDBKeyBounds(RocksDBEntryType::PrimaryIndexValue, indexId, left,
                          right);
}

/// used for point lookups
RocksDBKeyBounds RocksDBKeyBounds::UniqueVPackIndex(uint64_t indexId,
                                                    VPackSlice left) {
  return RocksDBKeyBounds(RocksDBEntryType::UniqueVPackIndexValue, indexId,
                          left);
}

RocksDBKeyBounds RocksDBKeyBounds::DatabaseViews(TRI_voc_tick_t databaseId) {
  return RocksDBKeyBounds(RocksDBEntryType::View, databaseId);
}

RocksDBKeyBounds RocksDBKeyBounds::DatabaseStates(TRI_voc_tick_t databaseId) {
  return RocksDBKeyBounds(RocksDBEntryType::ReplicatedState, databaseId);
}

RocksDBKeyBounds RocksDBKeyBounds::FulltextIndexPrefix(uint64_t objectId,
                                                       std::string_view word) {
  // I did not want to pass a bool to the constructor for this
  RocksDBKeyBounds b(RocksDBEntryType::FulltextIndexValue);

  auto& internals = b.internals();
  internals.reserve(2 * (sizeof(uint64_t) + word.size()) + 1);
  uint64ToPersistent(internals.buffer(), objectId);
  internals.buffer().append(word.data(), word.length());
  // no sperator byte, so we match all suffixes

  internals.separate();

  uint64ToPersistent(internals.buffer(), objectId);
  internals.buffer().append(word.data(), word.length());
  internals.push_back(static_cast<char>(0xFFU));
  // 0xFF is higher than any valud utf-8 character
  return b;
}

RocksDBKeyBounds RocksDBKeyBounds::FulltextIndexComplete(
    uint64_t indexId, std::string_view word) {
  return RocksDBKeyBounds(RocksDBEntryType::FulltextIndexValue, indexId, word);
}

RocksDBKeyBounds RocksDBKeyBounds::LogRange(uint64_t objectId) {
  return RocksDBKeyBounds(RocksDBEntryType::LogEntry, objectId);
}

// ============================ Member Methods ==============================

RocksDBKeyBounds::RocksDBKeyBounds(RocksDBKeyBounds const& other)
    : _type(other._type), _internals(other._internals) {}

RocksDBKeyBounds::RocksDBKeyBounds(RocksDBKeyBounds&& other) noexcept
    : _type(other._type), _internals(std::move(other._internals)) {}

RocksDBKeyBounds& RocksDBKeyBounds::operator=(RocksDBKeyBounds const& other) {
  if (this != &other) {
    _type = other._type;
    _internals = other._internals;
  }

  return *this;
}

RocksDBKeyBounds& RocksDBKeyBounds::operator=(
    RocksDBKeyBounds&& other) noexcept {
  if (this != &other) {
    _type = other._type;
    _internals = std::move(other._internals);
  }

  return *this;
}

uint64_t RocksDBKeyBounds::objectId() const {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  switch (_type) {
    case RocksDBEntryType::Document:
    case RocksDBEntryType::PrimaryIndexValue:
    case RocksDBEntryType::EdgeIndexValue:
    case RocksDBEntryType::VPackIndexValue:
    case RocksDBEntryType::UniqueVPackIndexValue:
    case RocksDBEntryType::LegacyGeoIndexValue:
    case RocksDBEntryType::GeoIndexValue:
    case RocksDBEntryType::FulltextIndexValue: {
      TRI_ASSERT(_internals.buffer().size() > sizeof(uint64_t));
      return uint64FromPersistent(_internals.buffer().data());
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_TYPE_ERROR);
  }
#else
  return uint64FromPersistent(_internals.buffer().data());
#endif
}

rocksdb::ColumnFamilyHandle* RocksDBKeyBounds::columnFamily() const {
  switch (_type) {
    case RocksDBEntryType::Placeholder:
      return RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::Invalid);
    case RocksDBEntryType::Document:
      return RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::Documents);
    case RocksDBEntryType::PrimaryIndexValue:
      return RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::PrimaryIndex);
    case RocksDBEntryType::EdgeIndexValue:
      return RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::EdgeIndex);
    case RocksDBEntryType::VPackIndexValue:
    case RocksDBEntryType::UniqueVPackIndexValue:
      return RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::VPackIndex);
    case RocksDBEntryType::FulltextIndexValue:
      return RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::FulltextIndex);
    case RocksDBEntryType::LegacyGeoIndexValue:
    case RocksDBEntryType::GeoIndexValue:
    case RocksDBEntryType::UniqueZkdIndexValue:
      return RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::GeoIndex);
    case RocksDBEntryType::ZkdIndexValue:
      return RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::ZkdIndex);
    case RocksDBEntryType::LogEntry:
      return RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::ReplicatedLogs);
    case RocksDBEntryType::Database:
    case RocksDBEntryType::Collection:
    case RocksDBEntryType::CounterValue:
    case RocksDBEntryType::SettingsValue:
    case RocksDBEntryType::ReplicationApplierConfig:
    case RocksDBEntryType::IndexEstimateValue:
    case RocksDBEntryType::KeyGeneratorValue:
    case RocksDBEntryType::RevisionTreeValue:
    case RocksDBEntryType::View:
    case RocksDBEntryType::ReplicatedState:
      return RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::Definitions);
  }
  THROW_ARANGO_EXCEPTION(TRI_ERROR_TYPE_ERROR);
}

/// bounds to iterate over specified word or edge
RocksDBKeyBounds::RocksDBKeyBounds(RocksDBEntryType type, uint64_t id,
                                   std::string_view lower,
                                   std::string_view upper)
    : _type(type) {
  switch (_type) {
    case RocksDBEntryType::PrimaryIndexValue: {
      // format: id lower id upper
      //         start    end
      _internals.reserve(
          sizeof(id) + (lower.size() + sizeof(kStringSeparator)) + sizeof(id) +
          (upper.size() + sizeof(kStringSeparator)));

      // id - lower
      uint64ToPersistent(_internals.buffer(), id);
      _internals.buffer().append(lower.data(), lower.length());
      _internals.push_back(kStringSeparator);

      // set separator
      _internals.separate();

      // id - upper
      uint64ToPersistent(_internals.buffer(), id);
      _internals.buffer().append(upper.data(), upper.length());
      _internals.push_back(kStringSeparator);

      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

// constructor for an empty bound. do not use for anything but to
// default-construct a key bound!
RocksDBKeyBounds::RocksDBKeyBounds()
    : _type(RocksDBEntryType::VPackIndexValue) {}

RocksDBKeyBounds::RocksDBKeyBounds(RocksDBEntryType type) : _type(type) {
  switch (_type) {
    case RocksDBEntryType::Database: {
      _internals.reserve(3 * sizeof(char));
      _internals.push_back(static_cast<char>(_type));

      _internals.separate();
      _internals.push_back(static_cast<char>(_type));
      _internals.push_back(static_cast<char>(0xFFU));
      break;
    }
    case RocksDBEntryType::CounterValue:
    case RocksDBEntryType::IndexEstimateValue:
    case RocksDBEntryType::KeyGeneratorValue: {
      _internals.reserve(2 * (sizeof(char) + sizeof(uint64_t)));
      _internals.push_back(static_cast<char>(_type));
      uint64ToPersistent(_internals.buffer(), 0);

      _internals.separate();
      _internals.push_back(static_cast<char>(_type));
      uint64ToPersistent(_internals.buffer(), UINT64_MAX);
      break;
    }
    case RocksDBEntryType::FulltextIndexValue:
      // intentionally empty
      break;

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

/// bounds to iterate over entire index
RocksDBKeyBounds::RocksDBKeyBounds(RocksDBEntryType type, uint64_t first)
    : _type(type) {
  switch (_type) {
    case RocksDBEntryType::Collection:
    case RocksDBEntryType::View:
    case RocksDBEntryType::ReplicatedState: {
      // Collections are stored as follows:
      // Key: 1 + 8-byte ArangoDB database ID + 8-byte ArangoDB collection ID
      _internals.reserve(2 * sizeof(char) + 3 * sizeof(uint64_t));
      _internals.push_back(static_cast<char>(_type));
      uint64ToPersistent(_internals.buffer(), first);
      _internals.separate();
      _internals.push_back(static_cast<char>(_type));
      uint64ToPersistent(_internals.buffer(), first);
      uint64ToPersistent(_internals.buffer(), UINT64_MAX);
      break;
    }
    case RocksDBEntryType::Document:
    case RocksDBEntryType::LogEntry:
    case RocksDBEntryType::LegacyGeoIndexValue:
    case RocksDBEntryType::GeoIndexValue: {
      // Documents are stored as follows:
      // Key: 8-byte object ID of collection + 8-byte document revision ID
      _internals.reserve(3 * sizeof(uint64_t));
      uint64ToPersistent(_internals.buffer(), first);
      _internals.separate();
      uint64ToPersistent(_internals.buffer(), first);
      uint64ToPersistent(_internals.buffer(), UINT64_MAX);
      // 0 - 0xFFFF... no matter the endianess
      break;
    }

    case RocksDBEntryType::PrimaryIndexValue:
    case RocksDBEntryType::EdgeIndexValue:
    case RocksDBEntryType::FulltextIndexValue: {
      size_t length = 2 * sizeof(uint64_t) + 4 * sizeof(char);
      _internals.reserve(length);
      uint64ToPersistent(_internals.buffer(), first);
      if (type == RocksDBEntryType::EdgeIndexValue) {
        _internals.push_back('\0');
        _internals.push_back(kStringSeparator);
      }

      _internals.separate();

      if (type == RocksDBEntryType::PrimaryIndexValue &&
          rocksDBEndianness == RocksDBEndianness::Big) {
        // if we are in big-endian mode, we can cheat a bit...
        // for the upper bound we can use the object id + 1, which will always
        // compare higher in a bytewise comparison
        rocksutils::uintToPersistentBigEndian<uint64_t>(_internals.buffer(),
                                                        first + 1);
        _internals.push_back(0x00U);  // lower/equal to any ascii char
      } else {
        uint64ToPersistent(_internals.buffer(), first);
        _internals.push_back(0xFFU);  // higher than any ascii char
        if (type == RocksDBEntryType::EdgeIndexValue) {
          _internals.push_back(kStringSeparator);
        }
      }
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

RocksDBKeyBounds::RocksDBKeyBounds(RocksDBEntryType type, uint64_t first,
                                   bool second)
    : _type(type) {
  switch (_type) {
    case RocksDBEntryType::ZkdIndexValue:
    case RocksDBEntryType::VPackIndexValue:
    case RocksDBEntryType::UniqueVPackIndexValue: {
      uint8_t const maxSlice[] = {0x02, 0x03, 0x1f};
      VPackSlice max(maxSlice);

      _internals.reserve(2 * sizeof(uint64_t) + (second ? max.byteSize() : 0));
      uint64ToPersistent(_internals.buffer(), first);
      _internals.separate();

      if (second) {
        // in case of reverse iteration, this is our starting point, so it must
        // be in the same prefix, otherwise we'll get no results; so here we
        // use the same objectId and the max vpack slice to make sure we find
        // everything
        uint64ToPersistent(_internals.buffer(), first);
        _internals.buffer().append((char const*)(max.begin()), max.byteSize());
      } else {
        // in case of forward iteration, we can use the next objectId as a quick
        // termination case, as it will be in the next prefix
        uint64ToPersistent(_internals.buffer(), first + 1);
      }
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

/// bounds to iterate over specified word or edge
RocksDBKeyBounds::RocksDBKeyBounds(RocksDBEntryType type, uint64_t first,
                                   std::string_view second)
    : _type(type) {
  switch (_type) {
    case RocksDBEntryType::FulltextIndexValue:
    case RocksDBEntryType::EdgeIndexValue: {
      _internals.reserve(2 * (sizeof(uint64_t) + second.size() + 2) + 1);
      uint64ToPersistent(_internals.buffer(), first);
      _internals.buffer().append(second.data(), second.length());
      _internals.push_back(kStringSeparator);

      _internals.separate();

      uint64ToPersistent(_internals.buffer(), first);
      _internals.buffer().append(second.data(), second.length());
      _internals.push_back(kStringSeparator);
      uint64ToPersistent(_internals.buffer(), UINT64_MAX);
      if (type == RocksDBEntryType::EdgeIndexValue) {
        _internals.push_back(0xFFU);  // high-byte for prefix extractor
      }
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

/// point lookups for unique velocypack indexes
RocksDBKeyBounds::RocksDBKeyBounds(RocksDBEntryType type, uint64_t first,
                                   VPackSlice second)
    : _type(type) {
  switch (_type) {
    case RocksDBEntryType::UniqueVPackIndexValue: {
      size_t startLength =
          sizeof(uint64_t) + static_cast<size_t>(second.byteSize());

      _internals.reserve(startLength);
      uint64ToPersistent(_internals.buffer(), first);
      _internals.buffer().append(reinterpret_cast<char const*>(second.begin()),
                                 static_cast<size_t>(second.byteSize()));

      _internals.separate();
      // second bound is intentionally left empty!
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

/// iterate over the specified bounds of the velocypack index
RocksDBKeyBounds::RocksDBKeyBounds(RocksDBEntryType type, uint64_t first,
                                   VPackSlice second, VPackSlice third)
    : _type(type) {
  fill(type, first, second, third);
}

void RocksDBKeyBounds::fill(RocksDBEntryType type, uint64_t first,
                            VPackSlice second, VPackSlice third) {
  clear();
  _type = type;
  switch (_type) {
    case RocksDBEntryType::VPackIndexValue:
    case RocksDBEntryType::UniqueVPackIndexValue: {
      size_t startLength =
          sizeof(uint64_t) + static_cast<size_t>(second.byteSize());
      size_t endLength =
          2 * sizeof(uint64_t) + static_cast<size_t>(third.byteSize());

      _internals.reserve(startLength + endLength);
      uint64ToPersistent(_internals.buffer(), first);
      _internals.buffer().append(reinterpret_cast<char const*>(second.begin()),
                                 static_cast<size_t>(second.byteSize()));

      _internals.separate();

      uint64ToPersistent(_internals.buffer(), first);
      _internals.buffer().append(reinterpret_cast<char const*>(third.begin()),
                                 static_cast<size_t>(third.byteSize()));
      uint64ToPersistent(_internals.buffer(), UINT64_MAX);
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

RocksDBKeyBounds::RocksDBKeyBounds(RocksDBEntryType type, uint64_t first,
                                   uint64_t second, uint64_t third)
    : _type(type) {
  switch (_type) {
    case RocksDBEntryType::Document: {
      // Documents are stored as follows:
      // Key: 8-byte object ID of collection + 8-byte document revision ID
      _internals.reserve(4 * sizeof(uint64_t));
      uint64ToPersistent(_internals.buffer(), first);   // objectid
      uint64ToPersistent(_internals.buffer(), second);  // min revision
      _internals.separate();
      uint64ToPersistent(_internals.buffer(), first);  // objectid
      uint64ToPersistent(_internals.buffer(), third);  // max revision
      break;
    }
    case RocksDBEntryType::GeoIndexValue: {
      _internals.reserve(sizeof(uint64_t) * 3 * 2);
      uint64ToPersistent(_internals.buffer(), first);
      uintToPersistentBigEndian<uint64_t>(_internals.buffer(), second);
      _internals.separate();
      uint64ToPersistent(_internals.buffer(), first);
      uintToPersistentBigEndian<uint64_t>(_internals.buffer(), third);
      uint64ToPersistent(_internals.buffer(), UINT64_MAX);
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

namespace arangodb {

std::ostream& operator<<(std::ostream& stream, RocksDBKeyBounds const& bounds) {
  stream << "[bounds cf: "
         << RocksDBColumnFamilyManager::name(bounds.columnFamily())
         << " type: " << arangodb::rocksDBEntryTypeName(bounds.type()) << " ";

  auto dump = [&stream](rocksdb::Slice const& slice) {
    size_t const n = slice.size();

    for (size_t i = 0; i < n; ++i) {
      stream << "0x";

      uint8_t const value = static_cast<uint8_t>(slice[i]);
      uint8_t x = value / 16;
      stream << static_cast<char>((x < 10 ? ('0' + x) : ('a' + x - 10)));
      x = value % 16;
      stream << static_cast<char>(x < 10 ? ('0' + x) : ('a' + x - 10));

      if (i + 1 != n) {
        stream << " ";
      }
    }
  };

  dump(bounds.start());
  stream << " - ";
  dump(bounds.end());
  stream << "]";

  return stream;
}
}  // namespace arangodb
