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
/// @author Heiko Kernbach
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Graph/Cache/RefactoredTraverserCache.h"
#include "Graph/Cursors/RefactoredSingleServerEdgeCursor.h"
#include "Graph/EdgeDocumentToken.h"
#include "Graph/Providers/BaseProviderOptions.h"
#include "Graph/Providers/BaseStep.h"
#include "Graph/Providers/TypeAliases.h"

#include "Aql/TraversalStats.h"
#include "Basics/ResourceUsage.h"

namespace arangodb {
struct ResourceMonitor;

namespace futures {
template<typename T>
class Future;
}

namespace aql {
class QueryContext;
}

namespace velocypack {
class Builder;
class HashedStringRef;
}  // namespace velocypack

namespace graph {

// TODO: we need to control from the outside if and which parts of the vertex -
// (will be implemented in the future via template parameters) data should be
// returned. This is most-likely done via a template parameter like this:
// template<ProduceVertexData>
template<class StepType>
class SingleServerProvider {
 public:
  using Options = SingleServerBaseProviderOptions;
  using Step = StepType;

 public:
  SingleServerProvider(arangodb::aql::QueryContext& queryContext, Options opts,
                       arangodb::ResourceMonitor& resourceMonitor);
  SingleServerProvider(SingleServerProvider const&) = delete;
  SingleServerProvider(SingleServerProvider&&) = default;
  ~SingleServerProvider() = default;

  SingleServerProvider& operator=(SingleServerProvider const&) = delete;

  auto startVertex(VertexType vertex, size_t depth = 0, double weight = 0.0)
      -> Step;
  auto fetchVertices(std::vector<Step*> const& looseEnds)
      -> futures::Future<std::vector<Step*>>;
  // dummy function, needed for OneSidedEnumerator::Provider
  auto fetchEdges(const std::vector<Step*>& fetchedVertices) -> Result;
  auto fetch(std::vector<Step*> const& looseEnds)
      -> futures::Future<std::vector<Step*>>;  // rocks
  auto expand(Step const& from, size_t previous,
              std::function<void(Step)> const& callback) -> void;  // index
  auto clear() -> void;

  void insertEdgeIntoResult(EdgeDocumentToken edge,
                            arangodb::velocypack::Builder& builder);
  void insertEdgeIdIntoResult(EdgeDocumentToken edge,
                              arangodb::velocypack::Builder& builder);

  std::string getEdgeId(typename Step::Edge const& edge);
  EdgeType getEdgeIdRef(typename Step::Edge const& edge);

  void addVertexToBuilder(typename Step::Vertex const& vertex,
                          arangodb::velocypack::Builder& builder,
                          bool writeIdIfNotFound = false);
  void addEdgeToBuilder(typename Step::Edge const& edge,
                        arangodb::velocypack::Builder& builder);

  // [GraphRefactor] TODO: Temporary method - will be needed until we've
  // finished the full graph refactor.
  EdgeDocumentToken getEdgeDocumentToken(typename Step::Edge const& edge);

  void addEdgeIDToBuilder(typename Step::Edge const& edge,
                          arangodb::velocypack::Builder& builder);

  /**
   * Adds the given Edge into the given builder, which is required to
   * be an open Object.
   * We will then add a key value pair:
   * `edgeId`: edgeData
   *
   * @param edge The edge to insert
   * @param builder The output builder, required to be an openObject
   */
  void addEdgeToLookupMap(typename Step::Edge const& edge,
                          arangodb::velocypack::Builder& builder);

  void destroyEngines() {}

  [[nodiscard]] ResourceMonitor& monitor();
  [[nodiscard]] transaction::Methods* trx();
  [[nodiscard]] TRI_vocbase_t const& vocbase() const;

  aql::TraversalStats stealStats();

  void prepareIndexExpressions(aql::Ast* ast);

  void prepareContext(aql::InputAqlItemRow input);
  void unPrepareContext();
  /**
   * Return true if the vertex whose id is stored in the class (in _vertex) has
   * its data on this DB-server.
   */
  bool isResponsible(Step const& step) const;
  [[nodiscard]] bool hasDepthSpecificLookup(uint64_t depth) const noexcept;

 private:
  void activateCache(bool enableDocumentCache);

  std::unique_ptr<RefactoredSingleServerEdgeCursor<Step>> buildCursor(
      arangodb::aql::FixedVarExpressionContext& expressionContext);

 private:
  ResourceMonitor& _monitor;
  // Unique_ptr to have this class movable, and to keep reference of trx()
  // alive - Note: _trx must be first here because it is used in _cursor
  std::unique_ptr<arangodb::transaction::Methods> _trx;

  std::unique_ptr<RefactoredSingleServerEdgeCursor<Step>> _cursor;

  SingleServerBaseProviderOptions _opts;

  RefactoredTraverserCache _cache;

  arangodb::aql::TraversalStats _stats;
};
}  // namespace graph
}  // namespace arangodb
