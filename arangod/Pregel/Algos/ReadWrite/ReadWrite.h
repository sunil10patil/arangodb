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
/// @author Roman Rabinovich
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <velocypack/Slice.h>
#include "Pregel/Algorithm.h"

namespace arangodb::pregel::algos {

struct ReadWriteType {
  using Vertex = float;
  using Edge = uint8_t;
  using Message = float;
};

using V = float;  // need to simulate MaxAggregator
using E = uint8_t;

struct ReadWrite : public SimpleAlgorithm<V, E, V> {
  explicit ReadWrite(arangodb::velocypack::Slice const& params);

  [[nodiscard]] auto name() const -> std::string_view override {
    return "readwrite";
  };

  [[nodiscard]] std::shared_ptr<GraphFormat<V, E> const> inputFormat()
      const override;

  [[nodiscard]] MessageFormat<V>* messageFormat() const override {
    return new NumberMessageFormat<V>();
  }
  [[nodiscard]] auto messageFormatUnique() const
      -> std::unique_ptr<message_format> override {
    return std::make_unique<NumberMessageFormat<V>>();
  }

  [[nodiscard]] MessageCombiner<V>* messageCombiner() const override {
    return new SumCombiner<V>();
  }
  [[nodiscard]] auto messageCombinerUnique() const
      -> std::unique_ptr<message_combiner> override {
    return std::make_unique<SumCombiner<V>>();
  }

  VertexComputation<V, E, V>* createComputation(
      std::shared_ptr<WorkerConfig const>) const override;

  [[nodiscard]] auto workerContext(
      std::unique_ptr<AggregatorHandler> readAggregators,
      std::unique_ptr<AggregatorHandler> writeAggregators,
      velocypack::Slice userParams) const -> WorkerContext* override;
  [[nodiscard]] auto workerContextUnique(
      std::unique_ptr<AggregatorHandler> readAggregators,
      std::unique_ptr<AggregatorHandler> writeAggregators,
      velocypack::Slice userParams) const
      -> std::unique_ptr<WorkerContext> override;

  [[nodiscard]] auto masterContext(
      std::unique_ptr<AggregatorHandler> aggregators,
      arangodb::velocypack::Slice userParams) const -> MasterContext* override;
  [[nodiscard]] auto masterContextUnique(
      uint64_t vertexCount, uint64_t edgeCount,
      std::unique_ptr<AggregatorHandler> aggregators,
      arangodb::velocypack::Slice userParams) const
      -> std::unique_ptr<MasterContext> override;

  [[nodiscard]] IAggregator* aggregator(std::string const& name) const override;
};
}  // namespace arangodb::pregel::algos
