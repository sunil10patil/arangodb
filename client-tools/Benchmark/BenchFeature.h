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

#pragma once

#include <atomic>

#include "ApplicationFeatures/ApplicationFeature.h"
#include "Benchmark/arangobench.h"
#include "Benchmark/BenchmarkThread.h"
#include "Benchmark/BenchmarkStats.h"

namespace arangodb {
namespace arangobench {
struct BenchmarkStats;
}

class ClientFeature;

struct BenchRunResult {
  double _time;
  uint64_t _failures;
  uint64_t _incomplete;
  double _requestTime;

  void update(double time, uint64_t failures, uint64_t incomplete,
              double requestTime) {
    _time = time;
    _failures = failures;
    _incomplete = incomplete;
    _requestTime = requestTime;
  }
};

class BenchFeature final : public ArangoBenchFeature {
 public:
  static constexpr std::string_view name() noexcept { return "Bench"; }

  BenchFeature(Server& server, int* result);

  void collectOptions(std::shared_ptr<options::ProgramOptions>) override;
  void start() override final;

  bool async() const { return _async; }
  uint64_t threadCount() const { return _threadCount; }
  uint64_t operations() const { return _operations; }
  uint64_t batchSize() const { return _batchSize; }
  bool createCollection() const { return _createCollection; }
  bool keepAlive() const { return _keepAlive; }
  std::string const& collection() const { return _collection; }
  std::string const& testCase() const { return _testCase; }
  uint64_t complexity() const { return _complexity; }
  bool delay() const { return _delay; }
  bool progress() const { return _progress; }
  bool verbose() const { return _verbose; }
  bool quiet() const { return _quiet; }
  uint64_t runs() const { return _runs; }
  std::string const& junitReportFile() const { return _junitReportFile; }
  uint64_t replicationFactor() const { return _replicationFactor; }
  uint64_t numberOfShards() const { return _numberOfShards; }
  bool waitForSync() const { return _waitForSync; }
  void validateOptions(std::shared_ptr<options::ProgramOptions>) override final;

  std::string const& customQuery() const { return _customQuery; }
  std::string const& customQueryFile() const { return _customQueryFile; }
  std::shared_ptr<VPackBuilder> customQueryBindVars() const {
    return _customQueryBindVarsBuilder;
  }

 private:
  void status(std::string const& value);
  void report(ClientFeature& client, std::vector<BenchRunResult> const& results,
              arangobench::BenchmarkStats const& stats,
              std::string const& histogram, VPackBuilder& builder);
  void printResult(BenchRunResult const& result, VPackBuilder& builder);
  bool writeJunitReport(BenchRunResult const& result);
  void setupHistogram(std::stringstream& pp);
  void updateStatsValues(
      std::stringstream& pp, VPackBuilder& builder,
      std::vector<
          std::unique_ptr<arangodb::arangobench::BenchmarkThread>> const&
          threads,
      arangodb::arangobench::BenchmarkStats& totalStats);

  uint64_t _threadCount;
  uint64_t _operations;
  uint64_t _realOperations;
  uint64_t _batchSize;
  uint64_t _duration;
  std::string _collection;
  std::string _testCase;
  uint64_t _complexity;
  bool _async;
  bool _keepAlive;
  bool _createDatabase;
  bool _createCollection{true};
  bool _delay;
  bool _progress;
  bool _verbose;
  bool _quiet;
  bool _waitForSync;
  bool _generateHistogram;  // don't generate histogram by default
  uint64_t _runs;
  std::string _junitReportFile;
  std::string _jsonReportFile;
  uint64_t _replicationFactor;
  uint64_t _numberOfShards;

  std::string _customQuery;
  std::string _customQueryFile;
  std::string _customQueryBindVars;
  std::shared_ptr<VPackBuilder> _customQueryBindVarsBuilder;

  int* _result;

  uint64_t _histogramNumIntervals;
  double _histogramIntervalSize;
  std::vector<double> _percentiles;

  static void updateStartCounter();
  static int getStartCounter();

  static std::atomic<int> _started;
};

}  // namespace arangodb
