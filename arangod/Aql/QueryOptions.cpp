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

#include "QueryOptions.h"

#include "Aql/QueryCache.h"
#include "Aql/QueryRegistry.h"
#include "Basics/StaticStrings.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

using namespace arangodb::aql;

size_t QueryOptions::defaultMemoryLimit = 0U;
size_t QueryOptions::defaultMaxNumberOfPlans = 128U;
#ifdef __APPLE__
// On OSX the default stack size for worker threads (non-main thread) is 512kb
// which is rather low, so we have to use a lower default
size_t QueryOptions::defaultMaxNodesPerCallstack = 150U;
#else
size_t QueryOptions::defaultMaxNodesPerCallstack = 250U;
#endif
size_t QueryOptions::defaultSpillOverThresholdNumRows = 5000000ULL;
size_t QueryOptions::defaultSpillOverThresholdMemoryUsage =
    134217728ULL;                                                // 128 MB
size_t QueryOptions::defaultMaxDNFConditionMembers = 786432ULL;  // 768K
double QueryOptions::defaultMaxRuntime = 0.0;
double QueryOptions::defaultTtl;
bool QueryOptions::defaultFailOnWarning = false;
bool QueryOptions::allowMemoryLimitOverride = true;

QueryOptions::QueryOptions()
    : memoryLimit(0),
      maxNumberOfPlans(QueryOptions::defaultMaxNumberOfPlans),
      maxWarningCount(10),
      maxNodesPerCallstack(QueryOptions::defaultMaxNodesPerCallstack),
      spillOverThresholdNumRows(QueryOptions::defaultSpillOverThresholdNumRows),
      spillOverThresholdMemoryUsage(
          QueryOptions::defaultSpillOverThresholdMemoryUsage),
      maxDNFConditionMembers(QueryOptions::defaultMaxDNFConditionMembers),
      maxRuntime(0.0),
      satelliteSyncWait(std::chrono::seconds(60)),
      ttl(QueryOptions::defaultTtl),  // get global default ttl
      profile(ProfileLevel::None),
      traversalProfile(TraversalProfileLevel::None),
      allPlans(false),
      verbosePlans(false),
      explainInternals(true),
      stream(false),
      retriable(false),
      silent(false),
      failOnWarning(
          QueryOptions::defaultFailOnWarning),  // use global "failOnWarning"
                                                // value
      cache(false),
      fullCount(false),
      count(false),
      skipAudit(false),
      explainRegisters(ExplainRegisterPlan::No) {
  // now set some default values from server configuration options
  {
    // use global memory limit value
    uint64_t globalLimit = QueryOptions::defaultMemoryLimit;
    if (globalLimit > 0) {
      memoryLimit = globalLimit;
    }
  }

  {
    // use global max runtime value
    double globalLimit = QueryOptions::defaultMaxRuntime;
    if (globalLimit > 0.0) {
      maxRuntime = globalLimit;
    }
  }

  // "cache" only defaults to true if query cache is turned on
  auto queryCacheMode = QueryCache::instance()->mode();
  cache = (queryCacheMode == CACHE_ALWAYS_ON);

  TRI_ASSERT(maxNumberOfPlans > 0);
}

QueryOptions::QueryOptions(velocypack::Slice slice) : QueryOptions() {
  this->fromVelocyPack(slice);
}

void QueryOptions::fromVelocyPack(VPackSlice slice) {
  if (!slice.isObject()) {
    return;
  }

  VPackSlice value;

  // use global memory limit value first
  if (QueryOptions::defaultMemoryLimit > 0) {
    memoryLimit = QueryOptions::defaultMemoryLimit;
  }

  // numeric options
  value = slice.get("memoryLimit");
  if (value.isNumber()) {
    size_t v = value.getNumber<size_t>();
    if (allowMemoryLimitOverride) {
      memoryLimit = v;
    } else if (v > 0 && v < memoryLimit) {
      // only allow increasing the memory limit if the respective startup option
      // is set. and if it is not set, only allow decreasing the memory limit
      memoryLimit = v;
    }
  }

  value = slice.get("maxNumberOfPlans");
  if (value.isNumber()) {
    maxNumberOfPlans = value.getNumber<size_t>();
    if (maxNumberOfPlans == 0) {
      maxNumberOfPlans = 1;
    }
  }

  value = slice.get("maxWarningCount");
  if (value.isNumber()) {
    maxWarningCount = value.getNumber<size_t>();
  }

  value = slice.get("maxNodesPerCallstack");
  if (value.isNumber()) {
    maxNodesPerCallstack = value.getNumber<size_t>();
  }

  value = slice.get("spillOverThresholdNumRows");
  if (value.isNumber()) {
    spillOverThresholdNumRows = value.getNumber<size_t>();
  }

  value = slice.get("spillOverThresholdMemoryUsage");
  if (value.isNumber()) {
    spillOverThresholdMemoryUsage = value.getNumber<size_t>();
  }

  value = slice.get("maxDNFConditionMembers");
  if (value.isNumber()) {
    maxDNFConditionMembers = value.getNumber<size_t>();
  }

  value = slice.get("maxRuntime");
  if (value.isNumber()) {
    maxRuntime = value.getNumber<double>();
  }

  value = slice.get("satelliteSyncWait");
  if (value.isNumber()) {
    satelliteSyncWait =
        std::chrono::duration<double>(value.getNumber<double>());
  }

  value = slice.get("ttl");
  if (value.isNumber()) {
    ttl = value.getNumber<double>();
  }

  // boolean options
  value = slice.get("profile");
  if (value.isBool()) {
    profile = value.getBool() ? ProfileLevel::Basic : ProfileLevel::None;
  } else if (value.isNumber()) {
    profile = static_cast<ProfileLevel>(value.getNumber<uint16_t>());
  }

  value = slice.get(StaticStrings::GraphTraversalProfileLevel);
  if (value.isBool()) {
    traversalProfile = value.getBool() ? TraversalProfileLevel::Basic
                                       : TraversalProfileLevel::None;
  } else if (value.isNumber()) {
    traversalProfile =
        static_cast<TraversalProfileLevel>(value.getNumber<uint16_t>());
  }

  if (value = slice.get("allPlans"); value.isBool()) {
    allPlans = value.getBool();
  }
  if (value = slice.get("verbosePlans"); value.isBool()) {
    verbosePlans = value.getBool();
  }
  if (value = slice.get("explainInternals"); value.isBool()) {
    explainInternals = value.getBool();
  }
  if (value = slice.get("stream"); value.isBool()) {
    stream = value.getBool();
  }
  if (value = slice.get("allowRetry"); value.isBool()) {
    retriable = value.isTrue();
  }
  if (value = slice.get("silent"); value.isBool()) {
    silent = value.getBool();
  }
  if (value = slice.get("failOnWarning"); value.isBool()) {
    failOnWarning = value.getBool();
  }
  if (value = slice.get("cache"); value.isBool()) {
    cache = value.getBool();
  }
  if (value = slice.get("fullCount"); value.isBool()) {
    fullCount = value.getBool();
  }
  if (value = slice.get("count"); value.isBool()) {
    count = value.getBool();
  }
  if (value = slice.get("explainRegisters"); value.isBool()) {
    explainRegisters =
        value.getBool() ? ExplainRegisterPlan::Yes : ExplainRegisterPlan::No;
  }

  // note: skipAudit is intentionally not read here.
  // the end user cannot override this setting

  if (value = slice.get(StaticStrings::ForceOneShardAttributeValue);
      value.isString()) {
    forceOneShardAttributeValue = value.copyString();
  }

  VPackSlice optimizer = slice.get("optimizer");
  if (optimizer.isObject()) {
    value = optimizer.get("rules");
    if (value.isArray()) {
      for (auto const& rule : VPackArrayIterator(value)) {
        if (rule.isString()) {
          optimizerRules.emplace_back(rule.copyString());
        }
      }
    }
  }
  value = slice.get("shardIds");
  if (value.isArray()) {
    VPackArrayIterator it(value);
    while (it.valid()) {
      value = it.value();
      if (value.isString()) {
        restrictToShards.emplace(value.copyString());
      }
      it.next();
    }
  }

#ifdef USE_ENTERPRISE
  value = slice.get("inaccessibleCollections");
  if (value.isArray()) {
    VPackArrayIterator it(value);
    while (it.valid()) {
      value = it.value();
      if (value.isString()) {
        inaccessibleCollections.emplace(value.copyString());
      }
      it.next();
    }
  }
#endif

  // also handle transaction options
  transactionOptions.fromVelocyPack(slice);
}

void QueryOptions::toVelocyPack(VPackBuilder& builder,
                                bool disableOptimizerRules) const {
  builder.openObject();

  builder.add("memoryLimit", VPackValue(memoryLimit));
  builder.add("maxNumberOfPlans", VPackValue(maxNumberOfPlans));
  builder.add("maxWarningCount", VPackValue(maxWarningCount));
  builder.add("maxNodesPerCallstack", VPackValue(maxNodesPerCallstack));
  builder.add("spillOverThresholdNumRows",
              VPackValue(spillOverThresholdNumRows));
  builder.add("spillOverThresholdMemoryUsage",
              VPackValue(spillOverThresholdMemoryUsage));
  builder.add("maxDNFConditionMembers", VPackValue(maxDNFConditionMembers));
  builder.add("maxRuntime", VPackValue(maxRuntime));
  builder.add("satelliteSyncWait", VPackValue(satelliteSyncWait.count()));
  builder.add("ttl", VPackValue(ttl));
  builder.add("profile", VPackValue(static_cast<uint32_t>(profile)));
  builder.add(StaticStrings::GraphTraversalProfileLevel,
              VPackValue(static_cast<uint32_t>(traversalProfile)));
  builder.add("allPlans", VPackValue(allPlans));
  builder.add("verbosePlans", VPackValue(verbosePlans));
  builder.add("explainInternals", VPackValue(explainInternals));
  builder.add("stream", VPackValue(stream));
  builder.add("allowRetry", VPackValue(retriable));
  builder.add("silent", VPackValue(silent));
  builder.add("failOnWarning", VPackValue(failOnWarning));
  builder.add("cache", VPackValue(cache));
  builder.add("fullCount", VPackValue(fullCount));
  builder.add("count", VPackValue(count));
  if (!forceOneShardAttributeValue.empty()) {
    builder.add(StaticStrings::ForceOneShardAttributeValue,
                VPackValue(forceOneShardAttributeValue));
  }

  // note: skipAudit is intentionally not serialized here.
  // the end user cannot override this setting anyway.

  builder.add("optimizer", VPackValue(VPackValueType::Object));
  // hard-coded since 3.8, option will be removed in the future
  builder.add("inspectSimplePlans", VPackValue(true));
  if (!optimizerRules.empty() || disableOptimizerRules) {
    builder.add("rules", VPackValue(VPackValueType::Array));
    if (disableOptimizerRules) {
      // turn off all optimizer rules
      builder.add(VPackValue("-all"));
    } else {
      for (auto const& it : optimizerRules) {
        builder.add(VPackValue(it));
      }
    }
    builder.close();  // optimizer.rules
  }
  builder.close();  // optimizer

  if (!restrictToShards.empty()) {
    builder.add("shardIds", VPackValue(VPackValueType::Array));
    for (auto const& it : restrictToShards) {
      builder.add(VPackValue(it));
    }
    builder.close();  // shardIds
  }

#ifdef USE_ENTERPRISE
  if (!inaccessibleCollections.empty()) {
    builder.add("inaccessibleCollections", VPackValue(VPackValueType::Array));
    for (auto const& it : inaccessibleCollections) {
      builder.add(VPackValue(it));
    }
    builder.close();  // inaccessibleCollections
  }
#endif

  // also handle transaction options
  transactionOptions.toVelocyPack(builder);

  builder.close();
}
