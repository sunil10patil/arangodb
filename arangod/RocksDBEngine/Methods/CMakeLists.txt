target_sources(arango_rocksdb PRIVATE
  RocksDBBatchedMethods.cpp
  RocksDBBatchedWithIndexMethods.cpp
  RocksDBReadOnlyBaseMethods.cpp
  RocksDBReadOnlyMethods.cpp
  RocksDBSingleOperationReadOnlyMethods.cpp
  RocksDBSingleOperationTrxMethods.cpp
  RocksDBSstFileMethods.cpp
  RocksDBTrxBaseMethods.cpp
  RocksDBTrxMethods.cpp)
