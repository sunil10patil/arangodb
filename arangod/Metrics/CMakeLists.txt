add_library(arango_metrics_base OBJECT
  Builder.cpp
  Counter.cpp
  Metric.cpp)

target_include_directories(arango_metrics_base PRIVATE
  "${PROJECT_SOURCE_DIR}/arangod"
  "${PROJECT_SOURCE_DIR}/${ENTERPRISE_INCLUDE_DIR}")

target_link_libraries(arango_metrics_base PRIVATE
  arango_lightweight)

add_library(arango_metrics STATIC
  MetricsFeature.cpp
  ClusterMetricsFeature.cpp
  ${PROJECT_SOURCE_DIR}/arangod/RestHandler/RestMetricsHandler.cpp)

target_link_libraries(arango_metrics
  arango_metrics_base
  arango
  arango::validation
  arango_replication2
  immer
  rocksdb_interface)

target_include_directories(arango_metrics PRIVATE
  "${PROJECT_SOURCE_DIR}/arangod"
  "${PROJECT_SOURCE_DIR}/${ENTERPRISE_INCLUDE_DIR}")
