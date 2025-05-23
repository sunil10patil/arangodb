////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Dan Larkin-York
/// @author Copyright 2017, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include <cstdint>
#include <string>
#include <memory>

#include "Basics/debugging.h"
#include "Cache/BinaryKeyHasher.h"
#include "Cache/TransactionalBucket.h"

using namespace arangodb::cache;

TEST(CacheTransactionalBucketTest, test_locking_behavior) {
  auto bucket = std::make_unique<TransactionalBucket>();
  bool success;

  // check lock without contention
  ASSERT_FALSE(bucket->isLocked());
  success = bucket->lock(-1LL);
  ASSERT_TRUE(success);
  ASSERT_TRUE(bucket->isLocked());

  // check lock with contention
  success = bucket->lock(10LL);
  ASSERT_FALSE(success);
  ASSERT_TRUE(bucket->isLocked());

  // check unlock
  bucket->unlock();
  ASSERT_FALSE(bucket->isLocked());

  // check that bnished term is updated appropriately
  ASSERT_EQ(0ULL, bucket->_banishTerm);
  bucket->lock(-1LL);
  bucket->updateBanishTerm(1ULL);
  ASSERT_EQ(1ULL, bucket->_banishTerm);
  bucket->unlock();
  ASSERT_EQ(1ULL, bucket->_banishTerm);
}

TEST(CacheTransactionalBucketTest, verify_eviction_behavior) {
  auto bucket = std::make_unique<TransactionalBucket>();

  std::uint32_t hashes[8] = {
      1, 2, 3, 4,
      5, 6, 7, 8};  // don't have to be real, but should be unique and non-zero
  std::uint64_t keys[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  std::uint64_t values[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  CachedValue* ptrs[8];
  for (std::size_t i = 0; i < 8; i++) {
    ptrs[i] = CachedValue::construct(&(keys[i]), sizeof(std::uint64_t),
                                     &(values[i]), sizeof(std::uint64_t));
    ASSERT_NE(ptrs[i], nullptr);
  }

  bool success = bucket->lock(-1LL);
  ASSERT_TRUE(success);

  ASSERT_FALSE(bucket->isFull());
  ASSERT_EQ(nullptr, bucket->evictionCandidate());

  // fill bucket
  for (std::size_t i = 0; i < 8; i++) {
    ASSERT_FALSE(bucket->isFull());
    bucket->insert(hashes[i], ptrs[i]);
    ASSERT_EQ(ptrs[0], bucket->evictionCandidate());
  }
  ASSERT_TRUE(bucket->isFull());

  for (std::size_t i = 0; i < 8; i++) {
    CachedValue* res = bucket->find<BinaryKeyHasher>(
        hashes[i], ptrs[i]->key(), ptrs[i]->keySize(), /*moveToFront*/ false);
    ASSERT_EQ(res, ptrs[i]);
  }

  for (std::size_t i = 0; i < 8; i++) {
    ASSERT_EQ(ptrs[i], bucket->evictionCandidate());
    std::uint64_t expected = ptrs[i]->size();
    std::uint64_t reclaimed = bucket->evictCandidate();
    ASSERT_EQ(reclaimed, expected);
    ptrs[i] = nullptr;

    for (std::size_t j = 0; j < 8; ++j) {
      CachedValue* res = bucket->find<BinaryKeyHasher>(
          hashes[j], &keys[j], /*keySize*/ sizeof(std::uint64_t),
          /*moveToFront*/ false);
      if (j > i) {
        ASSERT_NE(nullptr, ptrs[j]);
        ASSERT_EQ(res, ptrs[j]);
      } else {
        ASSERT_EQ(res, nullptr);
      }
    }
  }
  ASSERT_EQ(nullptr, bucket->evictionCandidate());
  bucket->unlock();
}

TEST(CacheTransactionalBucketTest, verify_that_insertion_works_as_expected) {
  auto bucket = std::make_unique<TransactionalBucket>();
  bool success;

  std::uint32_t hashes[9] = {
      1, 2, 3, 4, 5,
      6, 7, 8, 9};  // don't have to be real, but should be unique and non-zero
  std::uint64_t keys[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  std::uint64_t values[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  CachedValue* ptrs[9];
  for (std::size_t i = 0; i < 9; i++) {
    ptrs[i] = CachedValue::construct(&(keys[i]), sizeof(std::uint64_t),
                                     &(values[i]), sizeof(std::uint64_t));
    TRI_ASSERT(ptrs[i] != nullptr);
  }

  success = bucket->lock(-1LL);
  ASSERT_TRUE(success);

  // fill bucket
  ASSERT_FALSE(bucket->isFull());
  for (std::size_t i = 0; i < 8; i++) {
    bucket->insert(hashes[i], ptrs[i]);
    if (i < 7) {
      ASSERT_FALSE(bucket->isFull());
    } else {
      ASSERT_TRUE(bucket->isFull());
    }
  }
  for (std::size_t i = 0; i < 7; i++) {
    CachedValue* res = bucket->find<BinaryKeyHasher>(hashes[i], ptrs[i]->key(),
                                                     ptrs[i]->keySize());
    ASSERT_EQ(res, ptrs[i]);
  }

  // check that insert is ignored if full
  bucket->insert(hashes[8], ptrs[8]);
  CachedValue* res = bucket->find<BinaryKeyHasher>(hashes[8], ptrs[8]->key(),
                                                   ptrs[8]->keySize());
  ASSERT_EQ(nullptr, res);

  bucket->unlock();

  // cleanup
  for (std::size_t i = 0; i < 9; i++) {
    delete ptrs[i];
  }
}

TEST(CacheTransactionalBucketTest, verify_that_removal_works_as_expected) {
  auto bucket = std::make_unique<TransactionalBucket>();
  bool success;

  std::uint32_t hashes[3] = {
      1, 2, 3};  // don't have to be real, but should be unique and non-zero
  std::uint64_t keys[3] = {0, 1, 2};
  std::uint64_t values[3] = {0, 1, 2};
  CachedValue* ptrs[3];
  for (std::size_t i = 0; i < 3; i++) {
    ptrs[i] = CachedValue::construct(&(keys[i]), sizeof(std::uint64_t),
                                     &(values[i]), sizeof(std::uint64_t));
    TRI_ASSERT(ptrs[i] != nullptr);
  }

  success = bucket->lock(-1LL);
  ASSERT_TRUE(success);

  for (std::size_t i = 0; i < 3; i++) {
    bucket->insert(hashes[i], ptrs[i]);
  }
  for (std::size_t i = 0; i < 3; i++) {
    CachedValue* res = bucket->find<BinaryKeyHasher>(hashes[i], ptrs[i]->key(),
                                                     ptrs[i]->keySize());
    ASSERT_EQ(res, ptrs[i]);
  }

  CachedValue* res;
  res = bucket->remove<BinaryKeyHasher>(hashes[1], ptrs[1]->key(),
                                        ptrs[1]->keySize());
  ASSERT_EQ(res, ptrs[1]);
  res = bucket->find<BinaryKeyHasher>(hashes[1], ptrs[1]->key(),
                                      ptrs[1]->keySize());
  ASSERT_EQ(nullptr, res);
  res = bucket->remove<BinaryKeyHasher>(hashes[0], ptrs[0]->key(),
                                        ptrs[0]->keySize());
  ASSERT_EQ(res, ptrs[0]);
  res = bucket->find<BinaryKeyHasher>(hashes[0], ptrs[0]->key(),
                                      ptrs[0]->keySize());
  ASSERT_EQ(nullptr, res);
  res = bucket->remove<BinaryKeyHasher>(hashes[2], ptrs[2]->key(),
                                        ptrs[2]->keySize());
  ASSERT_EQ(res, ptrs[2]);
  res = bucket->find<BinaryKeyHasher>(hashes[2], ptrs[2]->key(),
                                      ptrs[2]->keySize());
  ASSERT_EQ(nullptr, res);

  bucket->unlock();

  // cleanup
  for (std::size_t i = 0; i < 3; i++) {
    delete ptrs[i];
  }
}

TEST(CacheTransactionalBucketTest, verify_that_eviction_works_as_expected) {
  auto bucket = std::make_unique<TransactionalBucket>();
  bool success;

  std::uint32_t hashes[9] = {
      1, 2, 3, 4, 5,
      6, 7, 8, 9};  // don't have to be real, but should be unique and non-zero
  std::uint64_t keys[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  std::uint64_t values[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  CachedValue* ptrs[9];
  for (std::size_t i = 0; i < 9; i++) {
    ptrs[i] = CachedValue::construct(&(keys[i]), sizeof(std::uint64_t),
                                     &(values[i]), sizeof(std::uint64_t));
    TRI_ASSERT(ptrs[i] != nullptr);
  }

  success = bucket->lock(-1LL);
  ASSERT_TRUE(success);

  // insert three to fill
  ASSERT_FALSE(bucket->isFull());
  for (std::size_t i = 0; i < 8; i++) {
    bucket->insert(hashes[i], ptrs[i]);
    if (i < 7) {
      ASSERT_FALSE(bucket->isFull());
    } else {
      ASSERT_TRUE(bucket->isFull());
    }
  }
  for (std::size_t i = 0; i < 8; i++) {
    CachedValue* res = bucket->find<BinaryKeyHasher>(hashes[i], ptrs[i]->key(),
                                                     ptrs[i]->keySize());
    ASSERT_EQ(res, ptrs[i]);
  }

  // check that we get proper eviction candidate
  CachedValue* candidate = bucket->evictionCandidate();
  ASSERT_EQ(candidate, ptrs[0]);
  bucket->evict(candidate);
  CachedValue* res = bucket->find<BinaryKeyHasher>(hashes[0], ptrs[0]->key(),
                                                   ptrs[0]->keySize());
  ASSERT_EQ(nullptr, res);
  ASSERT_FALSE(bucket->isFull());

  // check that we still find the right candidate if not full
  candidate = bucket->evictionCandidate();
  ASSERT_EQ(candidate, ptrs[1]);
  bucket->evict(candidate);
  res = bucket->find<BinaryKeyHasher>(hashes[1], ptrs[1]->key(),
                                      ptrs[1]->keySize());
  ASSERT_EQ(nullptr, res);
  ASSERT_FALSE(bucket->isFull());

  // check that we can insert now after eviction optimized for insertion
  bucket->insert(hashes[8], ptrs[8]);
  res = bucket->find<BinaryKeyHasher>(hashes[8], ptrs[8]->key(),
                                      ptrs[8]->keySize());
  ASSERT_EQ(res, ptrs[8]);

  bucket->unlock();

  // cleanup
  for (std::size_t i = 0; i < 9; i++) {
    delete ptrs[i];
  }
}

TEST(CacheTransactionalBucketTest, verify_that_banishing_works_as_expected) {
  auto bucket = std::make_unique<TransactionalBucket>();
  bool success;
  CachedValue* res;

  std::uint32_t hashes[8] = {
      1, 1, 2, 3, 4, 5, 6, 7};  // don't have to be real, want some overlap
  std::uint64_t keys[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  std::uint64_t values[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  CachedValue* ptrs[8];
  for (std::size_t i = 0; i < 8; i++) {
    ptrs[i] = CachedValue::construct(&(keys[i]), sizeof(std::uint64_t),
                                     &(values[i]), sizeof(std::uint64_t));
    TRI_ASSERT(ptrs[i] != nullptr);
  }

  success = bucket->lock(-1LL);
  bucket->updateBanishTerm(1ULL);
  ASSERT_TRUE(success);

  // insert eight to fill
  ASSERT_FALSE(bucket->isFull());
  for (std::size_t i = 0; i < 8; i++) {
    bucket->insert(hashes[i], ptrs[i]);
    if (i < 7) {
      ASSERT_FALSE(bucket->isFull());
    } else {
      ASSERT_TRUE(bucket->isFull());
    }
  }
  for (std::size_t i = 0; i < 8; i++) {
    res = bucket->find<BinaryKeyHasher>(hashes[i], ptrs[i]->key(),
                                        ptrs[i]->keySize());
    ASSERT_EQ(res, ptrs[i]);
  }

  // banish 1-5 to fill banish list
  for (std::size_t i = 1; i < 6; i++) {
    bucket->banish<BinaryKeyHasher>(hashes[i], ptrs[i]->key(),
                                    ptrs[i]->keySize());
  }
  for (std::size_t i = 1; i < 6; i++) {
    ASSERT_TRUE(bucket->isBanished(hashes[i]));
    res = bucket->find<BinaryKeyHasher>(hashes[i], ptrs[i]->key(),
                                        ptrs[i]->keySize());
    ASSERT_EQ(nullptr, res);
  }
  // verify actually not fully banished
  ASSERT_FALSE(bucket->isFullyBanished());
  ASSERT_FALSE(bucket->isBanished(hashes[6]));
  // verify it didn't remove matching hash with non-matching key
  res = bucket->find<BinaryKeyHasher>(hashes[0], ptrs[0]->key(),
                                      ptrs[0]->keySize());
  ASSERT_EQ(res, ptrs[0]);

  // proceed to fully banish
  bucket->banish<BinaryKeyHasher>(hashes[6], ptrs[6]->key(),
                                  ptrs[6]->keySize());
  ASSERT_TRUE(bucket->isBanished(hashes[6]));
  res = bucket->find<BinaryKeyHasher>(hashes[6], ptrs[6]->key(),
                                      ptrs[6]->keySize());
  ASSERT_EQ(nullptr, res);
  // make sure it still didn't remove non-matching key
  res = bucket->find<BinaryKeyHasher>(hashes[0], ptrs[0]->key(),
                                      ptrs[0]->keySize());
  ASSERT_EQ(ptrs[0], res);
  // make sure it's fully banished
  ASSERT_TRUE(bucket->isFullyBanished());
  ASSERT_TRUE(bucket->isBanished(hashes[7]));

  bucket->unlock();

  // check that updating banish list term clears banished list
  bucket->lock(-1LL);
  bucket->updateBanishTerm(2ULL);
  ASSERT_FALSE(bucket->isFullyBanished());
  for (std::size_t i = 0; i < 7; i++) {
    ASSERT_FALSE(bucket->isBanished(hashes[i]));
  }
  bucket->unlock();

  // cleanup
  for (std::size_t i = 0; i < 8; i++) {
    delete ptrs[i];
  }
}
