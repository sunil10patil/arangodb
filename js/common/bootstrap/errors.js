/*jshint maxlen: 240 */

/// auto-generated file generated from errors.dat

(function () {
  "use strict";
  var internal = require("internal");

  internal.errors = {
    "ERROR_NO_ERROR"               : { "code" : 0, "message" : "no error" },
    "ERROR_FAILED"                 : { "code" : 1, "message" : "failed" },
    "ERROR_SYS_ERROR"              : { "code" : 2, "message" : "system error" },
    "ERROR_OUT_OF_MEMORY"          : { "code" : 3, "message" : "out of memory" },
    "ERROR_INTERNAL"               : { "code" : 4, "message" : "internal error" },
    "ERROR_ILLEGAL_NUMBER"         : { "code" : 5, "message" : "illegal number" },
    "ERROR_NUMERIC_OVERFLOW"       : { "code" : 6, "message" : "numeric overflow" },
    "ERROR_ILLEGAL_OPTION"         : { "code" : 7, "message" : "illegal option" },
    "ERROR_DEAD_PID"               : { "code" : 8, "message" : "dead process identifier" },
    "ERROR_NOT_IMPLEMENTED"        : { "code" : 9, "message" : "not implemented" },
    "ERROR_BAD_PARAMETER"          : { "code" : 10, "message" : "bad parameter" },
    "ERROR_FORBIDDEN"              : { "code" : 11, "message" : "forbidden" },
    "ERROR_OUT_OF_MEMORY_MMAP"     : { "code" : 12, "message" : "out of memory in mmap" },
    "ERROR_CORRUPTED_CSV"          : { "code" : 13, "message" : "csv is corrupt" },
    "ERROR_FILE_NOT_FOUND"         : { "code" : 14, "message" : "file not found" },
    "ERROR_CANNOT_WRITE_FILE"      : { "code" : 15, "message" : "cannot write file" },
    "ERROR_CANNOT_OVERWRITE_FILE"  : { "code" : 16, "message" : "cannot overwrite file" },
    "ERROR_TYPE_ERROR"             : { "code" : 17, "message" : "type error" },
    "ERROR_LOCK_TIMEOUT"           : { "code" : 18, "message" : "lock timeout" },
    "ERROR_CANNOT_CREATE_DIRECTORY" : { "code" : 19, "message" : "cannot create directory" },
    "ERROR_CANNOT_CREATE_TEMP_FILE" : { "code" : 20, "message" : "cannot create temporary file" },
    "ERROR_REQUEST_CANCELED"       : { "code" : 21, "message" : "canceled request" },
    "ERROR_DEBUG"                  : { "code" : 22, "message" : "intentional debug error" },
    "ERROR_IP_ADDRESS_INVALID"     : { "code" : 25, "message" : "IP address is invalid" },
    "ERROR_FILE_EXISTS"            : { "code" : 27, "message" : "file exists" },
    "ERROR_LOCKED"                 : { "code" : 28, "message" : "locked" },
    "ERROR_DEADLOCK"               : { "code" : 29, "message" : "deadlock detected" },
    "ERROR_SHUTTING_DOWN"          : { "code" : 30, "message" : "shutdown in progress" },
    "ERROR_ONLY_ENTERPRISE"        : { "code" : 31, "message" : "only enterprise version" },
    "ERROR_RESOURCE_LIMIT"         : { "code" : 32, "message" : "resource limit exceeded" },
    "ERROR_ARANGO_ICU_ERROR"       : { "code" : 33, "message" : "icu error: %s" },
    "ERROR_CANNOT_READ_FILE"       : { "code" : 34, "message" : "cannot read file" },
    "ERROR_INCOMPATIBLE_VERSION"   : { "code" : 35, "message" : "incompatible server version" },
    "ERROR_DISABLED"               : { "code" : 36, "message" : "disabled" },
    "ERROR_MALFORMED_JSON"         : { "code" : 37, "message" : "malformed json" },
    "ERROR_STARTING_UP"            : { "code" : 38, "message" : "startup ongoing" },
    "ERROR_DESERIALIZE"            : { "code" : 39, "message" : "error during deserialization" },
    "ERROR_HTTP_BAD_PARAMETER"     : { "code" : 400, "message" : "bad parameter" },
    "ERROR_HTTP_UNAUTHORIZED"      : { "code" : 401, "message" : "unauthorized" },
    "ERROR_HTTP_FORBIDDEN"         : { "code" : 403, "message" : "forbidden" },
    "ERROR_HTTP_NOT_FOUND"         : { "code" : 404, "message" : "not found" },
    "ERROR_HTTP_METHOD_NOT_ALLOWED" : { "code" : 405, "message" : "method not supported" },
    "ERROR_HTTP_NOT_ACCEPTABLE"    : { "code" : 406, "message" : "request not acceptable" },
    "ERROR_HTTP_REQUEST_TIMEOUT"   : { "code" : 408, "message" : "request timeout" },
    "ERROR_HTTP_CONFLICT"          : { "code" : 409, "message" : "conflict" },
    "ERROR_HTTP_GONE"              : { "code" : 410, "message" : "content permanently deleted" },
    "ERROR_HTTP_PRECONDITION_FAILED" : { "code" : 412, "message" : "precondition failed" },
    "ERROR_HTTP_ENHANCE_YOUR_CALM" : { "code" : 420, "message" : "enhance your calm" },
    "ERROR_HTTP_SERVER_ERROR"      : { "code" : 500, "message" : "internal server error" },
    "ERROR_HTTP_NOT_IMPLEMENTED"   : { "code" : 501, "message" : "not implemented" },
    "ERROR_HTTP_SERVICE_UNAVAILABLE" : { "code" : 503, "message" : "service unavailable" },
    "ERROR_HTTP_GATEWAY_TIMEOUT"   : { "code" : 504, "message" : "gateway timeout" },
    "ERROR_HTTP_CORRUPTED_JSON"    : { "code" : 600, "message" : "invalid JSON object" },
    "ERROR_HTTP_SUPERFLUOUS_SUFFICES" : { "code" : 601, "message" : "superfluous URL suffices" },
    "ERROR_ARANGO_ILLEGAL_STATE"   : { "code" : 1000, "message" : "illegal state" },
    "ERROR_ARANGO_READ_ONLY"       : { "code" : 1004, "message" : "read only" },
    "ERROR_ARANGO_DUPLICATE_IDENTIFIER" : { "code" : 1005, "message" : "duplicate identifier" },
    "ERROR_ARANGO_CORRUPTED_DATAFILE" : { "code" : 1100, "message" : "corrupted datafile" },
    "ERROR_ARANGO_ILLEGAL_PARAMETER_FILE" : { "code" : 1101, "message" : "illegal or unreadable parameter file" },
    "ERROR_ARANGO_CORRUPTED_COLLECTION" : { "code" : 1102, "message" : "corrupted collection" },
    "ERROR_ARANGO_FILESYSTEM_FULL" : { "code" : 1104, "message" : "filesystem full" },
    "ERROR_ARANGO_DATADIR_LOCKED"  : { "code" : 1107, "message" : "database directory is locked" },
    "ERROR_ARANGO_CONFLICT"        : { "code" : 1200, "message" : "conflict" },
    "ERROR_ARANGO_DOCUMENT_NOT_FOUND" : { "code" : 1202, "message" : "document not found" },
    "ERROR_ARANGO_DATA_SOURCE_NOT_FOUND" : { "code" : 1203, "message" : "collection or view not found" },
    "ERROR_ARANGO_COLLECTION_PARAMETER_MISSING" : { "code" : 1204, "message" : "parameter 'collection' not found" },
    "ERROR_ARANGO_DOCUMENT_HANDLE_BAD" : { "code" : 1205, "message" : "illegal document identifier" },
    "ERROR_ARANGO_DUPLICATE_NAME"  : { "code" : 1207, "message" : "duplicate name" },
    "ERROR_ARANGO_ILLEGAL_NAME"    : { "code" : 1208, "message" : "illegal name" },
    "ERROR_ARANGO_NO_INDEX"        : { "code" : 1209, "message" : "no suitable index known" },
    "ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED" : { "code" : 1210, "message" : "unique constraint violated" },
    "ERROR_ARANGO_INDEX_NOT_FOUND" : { "code" : 1212, "message" : "index not found" },
    "ERROR_ARANGO_CROSS_COLLECTION_REQUEST" : { "code" : 1213, "message" : "cross collection request not allowed" },
    "ERROR_ARANGO_INDEX_HANDLE_BAD" : { "code" : 1214, "message" : "illegal index identifier" },
    "ERROR_ARANGO_DOCUMENT_TOO_LARGE" : { "code" : 1216, "message" : "document too large" },
    "ERROR_ARANGO_COLLECTION_TYPE_INVALID" : { "code" : 1218, "message" : "collection type invalid" },
    "ERROR_ARANGO_ATTRIBUTE_PARSER_FAILED" : { "code" : 1220, "message" : "parsing attribute name definition failed" },
    "ERROR_ARANGO_DOCUMENT_KEY_BAD" : { "code" : 1221, "message" : "illegal document key" },
    "ERROR_ARANGO_DOCUMENT_KEY_UNEXPECTED" : { "code" : 1222, "message" : "unexpected document key" },
    "ERROR_ARANGO_DATADIR_NOT_WRITABLE" : { "code" : 1224, "message" : "server database directory not writable" },
    "ERROR_ARANGO_OUT_OF_KEYS"     : { "code" : 1225, "message" : "out of keys" },
    "ERROR_ARANGO_DOCUMENT_KEY_MISSING" : { "code" : 1226, "message" : "missing document key" },
    "ERROR_ARANGO_DOCUMENT_TYPE_INVALID" : { "code" : 1227, "message" : "invalid document type" },
    "ERROR_ARANGO_DATABASE_NOT_FOUND" : { "code" : 1228, "message" : "database not found" },
    "ERROR_ARANGO_DATABASE_NAME_INVALID" : { "code" : 1229, "message" : "database name invalid" },
    "ERROR_ARANGO_USE_SYSTEM_DATABASE" : { "code" : 1230, "message" : "operation only allowed in system database" },
    "ERROR_ARANGO_INVALID_KEY_GENERATOR" : { "code" : 1232, "message" : "invalid key generator" },
    "ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE" : { "code" : 1233, "message" : "edge attribute missing or invalid" },
    "ERROR_ARANGO_INDEX_CREATION_FAILED" : { "code" : 1235, "message" : "index creation failed" },
    "ERROR_ARANGO_COLLECTION_TYPE_MISMATCH" : { "code" : 1237, "message" : "collection type mismatch" },
    "ERROR_ARANGO_COLLECTION_NOT_LOADED" : { "code" : 1238, "message" : "collection not loaded" },
    "ERROR_ARANGO_DOCUMENT_REV_BAD" : { "code" : 1239, "message" : "illegal document revision" },
    "ERROR_ARANGO_INCOMPLETE_READ" : { "code" : 1240, "message" : "incomplete read" },
    "ERROR_ARANGO_EMPTY_DATADIR"   : { "code" : 1301, "message" : "server database directory is empty" },
    "ERROR_ARANGO_TRY_AGAIN"       : { "code" : 1302, "message" : "operation should be tried again" },
    "ERROR_ARANGO_BUSY"            : { "code" : 1303, "message" : "engine is busy" },
    "ERROR_ARANGO_MERGE_IN_PROGRESS" : { "code" : 1304, "message" : "merge in progress" },
    "ERROR_ARANGO_IO_ERROR"        : { "code" : 1305, "message" : "storage engine I/O error" },
    "ERROR_REPLICATION_NO_RESPONSE" : { "code" : 1400, "message" : "no response" },
    "ERROR_REPLICATION_INVALID_RESPONSE" : { "code" : 1401, "message" : "invalid response" },
    "ERROR_REPLICATION_LEADER_ERROR" : { "code" : 1402, "message" : "leader error" },
    "ERROR_REPLICATION_LEADER_INCOMPATIBLE" : { "code" : 1403, "message" : "leader incompatible" },
    "ERROR_REPLICATION_LEADER_CHANGE" : { "code" : 1404, "message" : "leader change" },
    "ERROR_REPLICATION_LOOP"       : { "code" : 1405, "message" : "loop detected" },
    "ERROR_REPLICATION_UNEXPECTED_MARKER" : { "code" : 1406, "message" : "unexpected marker" },
    "ERROR_REPLICATION_INVALID_APPLIER_STATE" : { "code" : 1407, "message" : "invalid applier state" },
    "ERROR_REPLICATION_UNEXPECTED_TRANSACTION" : { "code" : 1408, "message" : "invalid transaction" },
    "ERROR_REPLICATION_SHARD_SYNC_ATTEMPT_TIMEOUT_EXCEEDED" : { "code" : 1409, "message" : "shard synchronization attempt timeout exceeded" },
    "ERROR_REPLICATION_INVALID_APPLIER_CONFIGURATION" : { "code" : 1410, "message" : "invalid replication applier configuration" },
    "ERROR_REPLICATION_RUNNING"    : { "code" : 1411, "message" : "cannot perform operation while applier is running" },
    "ERROR_REPLICATION_APPLIER_STOPPED" : { "code" : 1412, "message" : "replication stopped" },
    "ERROR_REPLICATION_NO_START_TICK" : { "code" : 1413, "message" : "no start tick" },
    "ERROR_REPLICATION_START_TICK_NOT_PRESENT" : { "code" : 1414, "message" : "start tick not present" },
    "ERROR_REPLICATION_WRONG_CHECKSUM" : { "code" : 1416, "message" : "wrong checksum" },
    "ERROR_REPLICATION_SHARD_NONEMPTY" : { "code" : 1417, "message" : "shard not empty" },
    "ERROR_REPLICATION_REPLICATED_LOG_NOT_FOUND" : { "code" : 1418, "message" : "replicated log {} not found" },
    "ERROR_REPLICATION_REPLICATED_LOG_NOT_THE_LEADER" : { "code" : 1419, "message" : "not the log leader" },
    "ERROR_REPLICATION_REPLICATED_LOG_NOT_A_FOLLOWER" : { "code" : 1420, "message" : "not a log follower" },
    "ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED" : { "code" : 1421, "message" : "follower rejected append entries request" },
    "ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED" : { "code" : 1422, "message" : "a resigned leader instance rejected a request" },
    "ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED" : { "code" : 1423, "message" : "a resigned follower instance rejected a request" },
    "ERROR_REPLICATION_REPLICATED_LOG_PARTICIPANT_GONE" : { "code" : 1424, "message" : "the replicated log of the participant is gone" },
    "ERROR_REPLICATION_REPLICATED_LOG_INVALID_TERM" : { "code" : 1425, "message" : "an invalid term was given" },
    "ERROR_REPLICATION_REPLICATED_LOG_UNCONFIGURED" : { "code" : 1426, "message" : "log participant unconfigured" },
    "ERROR_REPLICATION_REPLICATED_STATE_NOT_FOUND" : { "code" : 1427, "message" : "replicated state {id:} of type {type:} not found" },
    "ERROR_REPLICATION_REPLICATED_STATE_NOT_AVAILABLE" : { "code" : 1428, "message" : "replicated state {id:} of type {type:} is unavailable" },
    "ERROR_REPLICATION_WRITE_CONCERN_NOT_FULFILLED" : { "code" : 1429, "message" : "not enough replicas for the configured write-concern are present" },
    "ERROR_REPLICATION_REPLICATED_LOG_SUBSEQUENT_FAULT" : { "code" : 1430, "message" : "operation aborted because a previous operation failed" },
    "ERROR_REPLICATION_REPLICATED_STATE_IMPLEMENTATION_NOT_FOUND" : { "code" : 1431, "message" : "replicated state type {type:} is unavailable" },
    "ERROR_CLUSTER_NOT_FOLLOWER"   : { "code" : 1446, "message" : "not a follower" },
    "ERROR_CLUSTER_FOLLOWER_TRANSACTION_COMMIT_PERFORMED" : { "code" : 1447, "message" : "follower transaction intermediate commit already performed" },
    "ERROR_CLUSTER_CREATE_COLLECTION_PRECONDITION_FAILED" : { "code" : 1448, "message" : "creating collection failed due to precondition" },
    "ERROR_CLUSTER_SERVER_UNKNOWN" : { "code" : 1449, "message" : "got a request from an unknown server" },
    "ERROR_CLUSTER_TOO_MANY_SHARDS" : { "code" : 1450, "message" : "too many shards" },
    "ERROR_CLUSTER_COULD_NOT_CREATE_COLLECTION_IN_PLAN" : { "code" : 1454, "message" : "could not create collection in plan" },
    "ERROR_CLUSTER_COULD_NOT_CREATE_COLLECTION" : { "code" : 1456, "message" : "could not create collection" },
    "ERROR_CLUSTER_TIMEOUT"        : { "code" : 1457, "message" : "timeout in cluster operation" },
    "ERROR_CLUSTER_COULD_NOT_REMOVE_COLLECTION_IN_PLAN" : { "code" : 1458, "message" : "could not remove collection from plan" },
    "ERROR_CLUSTER_COULD_NOT_CREATE_DATABASE_IN_PLAN" : { "code" : 1460, "message" : "could not create database in plan" },
    "ERROR_CLUSTER_COULD_NOT_CREATE_DATABASE" : { "code" : 1461, "message" : "could not create database" },
    "ERROR_CLUSTER_COULD_NOT_REMOVE_DATABASE_IN_PLAN" : { "code" : 1462, "message" : "could not remove database from plan" },
    "ERROR_CLUSTER_COULD_NOT_REMOVE_DATABASE_IN_CURRENT" : { "code" : 1463, "message" : "could not remove database from current" },
    "ERROR_CLUSTER_SHARD_GONE"     : { "code" : 1464, "message" : "no responsible shard found" },
    "ERROR_CLUSTER_CONNECTION_LOST" : { "code" : 1465, "message" : "cluster internal HTTP connection broken" },
    "ERROR_CLUSTER_MUST_NOT_SPECIFY_KEY" : { "code" : 1466, "message" : "must not specify _key for this collection" },
    "ERROR_CLUSTER_GOT_CONTRADICTING_ANSWERS" : { "code" : 1467, "message" : "got contradicting answers from different shards" },
    "ERROR_CLUSTER_NOT_ALL_SHARDING_ATTRIBUTES_GIVEN" : { "code" : 1468, "message" : "not all sharding attributes given" },
    "ERROR_CLUSTER_MUST_NOT_CHANGE_SHARDING_ATTRIBUTES" : { "code" : 1469, "message" : "must not change the value of a shard key attribute" },
    "ERROR_CLUSTER_UNSUPPORTED"    : { "code" : 1470, "message" : "unsupported operation or parameter for clusters" },
    "ERROR_CLUSTER_ONLY_ON_COORDINATOR" : { "code" : 1471, "message" : "this operation is only valid on a coordinator in a cluster" },
    "ERROR_CLUSTER_READING_PLAN_AGENCY" : { "code" : 1472, "message" : "error reading Plan in agency" },
    "ERROR_CLUSTER_AQL_COMMUNICATION" : { "code" : 1474, "message" : "error in cluster internal communication for AQL" },
    "ERROR_CLUSTER_ONLY_ON_DBSERVER" : { "code" : 1477, "message" : "this operation is only valid on a DBserver in a cluster" },
    "ERROR_CLUSTER_BACKEND_UNAVAILABLE" : { "code" : 1478, "message" : "A cluster backend which was required for the operation could not be reached" },
    "ERROR_CLUSTER_AQL_COLLECTION_OUT_OF_SYNC" : { "code" : 1481, "message" : "collection/view is out of sync" },
    "ERROR_CLUSTER_COULD_NOT_CREATE_INDEX_IN_PLAN" : { "code" : 1482, "message" : "could not create index in plan" },
    "ERROR_CLUSTER_COULD_NOT_DROP_INDEX_IN_PLAN" : { "code" : 1483, "message" : "could not drop index in plan" },
    "ERROR_CLUSTER_CHAIN_OF_DISTRIBUTESHARDSLIKE" : { "code" : 1484, "message" : "chain of distributeShardsLike references" },
    "ERROR_CLUSTER_MUST_NOT_DROP_COLL_OTHER_DISTRIBUTESHARDSLIKE" : { "code" : 1485, "message" : "must not drop collection while another has a distributeShardsLike attribute pointing to it" },
    "ERROR_CLUSTER_UNKNOWN_DISTRIBUTESHARDSLIKE" : { "code" : 1486, "message" : "must not have a distributeShardsLike attribute pointing to an unknown collection" },
    "ERROR_CLUSTER_INSUFFICIENT_DBSERVERS" : { "code" : 1487, "message" : "the number of current dbservers is lower than the requested replicationFactor" },
    "ERROR_CLUSTER_COULD_NOT_DROP_FOLLOWER" : { "code" : 1488, "message" : "a follower could not be dropped in agency" },
    "ERROR_CLUSTER_SHARD_LEADER_REFUSES_REPLICATION" : { "code" : 1489, "message" : "a shard leader refuses to perform a replication operation" },
    "ERROR_CLUSTER_SHARD_FOLLOWER_REFUSES_OPERATION" : { "code" : 1490, "message" : "a shard follower refuses to perform an operation" },
    "ERROR_CLUSTER_SHARD_LEADER_RESIGNED" : { "code" : 1491, "message" : "a (former) shard leader refuses to perform an operation, because it has resigned in the meantime" },
    "ERROR_CLUSTER_AGENCY_COMMUNICATION_FAILED" : { "code" : 1492, "message" : "some agency operation failed" },
    "ERROR_CLUSTER_LEADERSHIP_CHALLENGE_ONGOING" : { "code" : 1495, "message" : "leadership challenge is ongoing" },
    "ERROR_CLUSTER_NOT_LEADER"     : { "code" : 1496, "message" : "not a leader" },
    "ERROR_CLUSTER_COULD_NOT_CREATE_VIEW_IN_PLAN" : { "code" : 1497, "message" : "could not create view in plan" },
    "ERROR_CLUSTER_VIEW_ID_EXISTS" : { "code" : 1498, "message" : "view ID already exists" },
    "ERROR_CLUSTER_COULD_NOT_DROP_COLLECTION" : { "code" : 1499, "message" : "could not drop collection in plan" },
    "ERROR_QUERY_KILLED"           : { "code" : 1500, "message" : "query killed" },
    "ERROR_QUERY_PARSE"            : { "code" : 1501, "message" : "%s" },
    "ERROR_QUERY_EMPTY"            : { "code" : 1502, "message" : "query is empty" },
    "ERROR_QUERY_SCRIPT"           : { "code" : 1503, "message" : "runtime error '%s'" },
    "ERROR_QUERY_NUMBER_OUT_OF_RANGE" : { "code" : 1504, "message" : "number out of range" },
    "ERROR_QUERY_INVALID_GEO_VALUE" : { "code" : 1505, "message" : "invalid geo coordinate value" },
    "ERROR_QUERY_VARIABLE_NAME_INVALID" : { "code" : 1510, "message" : "variable name '%s' has an invalid format" },
    "ERROR_QUERY_VARIABLE_REDECLARED" : { "code" : 1511, "message" : "variable '%s' is assigned multiple times" },
    "ERROR_QUERY_VARIABLE_NAME_UNKNOWN" : { "code" : 1512, "message" : "unknown variable '%s'" },
    "ERROR_QUERY_COLLECTION_LOCK_FAILED" : { "code" : 1521, "message" : "unable to read-lock collection %s" },
    "ERROR_QUERY_TOO_MANY_COLLECTIONS" : { "code" : 1522, "message" : "too many collections/shards" },
    "ERROR_QUERY_TOO_MUCH_NESTING" : { "code" : 1524, "message" : "too much nesting or too many objects" },
    "ERROR_QUERY_INVALID_OPTIONS_ATTRIBUTE" : { "code" : 1539, "message" : "unknown OPTIONS attribute used" },
    "ERROR_QUERY_FUNCTION_NAME_UNKNOWN" : { "code" : 1540, "message" : "usage of unknown function '%s()'" },
    "ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH" : { "code" : 1541, "message" : "invalid number of arguments for function '%s()', expected number of arguments: minimum: %d, maximum: %d" },
    "ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH" : { "code" : 1542, "message" : "invalid argument type in call to function '%s()'" },
    "ERROR_QUERY_INVALID_REGEX"    : { "code" : 1543, "message" : "invalid regex value" },
    "ERROR_QUERY_BIND_PARAMETERS_INVALID" : { "code" : 1550, "message" : "invalid structure of bind parameters" },
    "ERROR_QUERY_BIND_PARAMETER_MISSING" : { "code" : 1551, "message" : "no value specified for declared bind parameter '%s'" },
    "ERROR_QUERY_BIND_PARAMETER_UNDECLARED" : { "code" : 1552, "message" : "bind parameter '%s' was not declared in the query" },
    "ERROR_QUERY_BIND_PARAMETER_TYPE" : { "code" : 1553, "message" : "bind parameter '%s' has an invalid value or type" },
    "ERROR_QUERY_INVALID_ARITHMETIC_VALUE" : { "code" : 1561, "message" : "invalid arithmetic value" },
    "ERROR_QUERY_DIVISION_BY_ZERO" : { "code" : 1562, "message" : "division by zero" },
    "ERROR_QUERY_ARRAY_EXPECTED"   : { "code" : 1563, "message" : "array expected" },
    "ERROR_QUERY_COLLECTION_USED_IN_EXPRESSION" : { "code" : 1568, "message" : "collection '%s' used as expression operand" },
    "ERROR_QUERY_FAIL_CALLED"      : { "code" : 1569, "message" : "FAIL(%s) called" },
    "ERROR_QUERY_GEO_INDEX_MISSING" : { "code" : 1570, "message" : "no suitable geo index found for geo restriction on '%s'" },
    "ERROR_QUERY_FULLTEXT_INDEX_MISSING" : { "code" : 1571, "message" : "no suitable fulltext index found for fulltext query on '%s'" },
    "ERROR_QUERY_INVALID_DATE_VALUE" : { "code" : 1572, "message" : "invalid date value" },
    "ERROR_QUERY_MULTI_MODIFY"     : { "code" : 1573, "message" : "multi-modify query" },
    "ERROR_QUERY_INVALID_AGGREGATE_EXPRESSION" : { "code" : 1574, "message" : "invalid aggregate expression" },
    "ERROR_QUERY_COMPILE_TIME_OPTIONS" : { "code" : 1575, "message" : "query options must be readable at query compile time" },
    "ERROR_QUERY_DNF_COMPLEXITY"   : { "code" : 1576, "message" : "FILTER/PRUNE condition complexity is too high" },
    "ERROR_QUERY_FORCED_INDEX_HINT_UNUSABLE" : { "code" : 1577, "message" : "could not use forced index hint" },
    "ERROR_QUERY_DISALLOWED_DYNAMIC_CALL" : { "code" : 1578, "message" : "disallowed dynamic call to '%s'" },
    "ERROR_QUERY_ACCESS_AFTER_MODIFICATION" : { "code" : 1579, "message" : "access after data-modification by %s" },
    "ERROR_QUERY_FUNCTION_INVALID_NAME" : { "code" : 1580, "message" : "invalid user function name" },
    "ERROR_QUERY_FUNCTION_INVALID_CODE" : { "code" : 1581, "message" : "invalid user function code" },
    "ERROR_QUERY_FUNCTION_NOT_FOUND" : { "code" : 1582, "message" : "user function '%s()' not found" },
    "ERROR_QUERY_FUNCTION_RUNTIME_ERROR" : { "code" : 1583, "message" : "user function runtime error: %s" },
    "ERROR_QUERY_BAD_JSON_PLAN"    : { "code" : 1590, "message" : "bad execution plan JSON" },
    "ERROR_QUERY_NOT_FOUND"        : { "code" : 1591, "message" : "query ID not found" },
    "ERROR_QUERY_USER_ASSERT"      : { "code" : 1593, "message" : "%s" },
    "ERROR_QUERY_USER_WARN"        : { "code" : 1594, "message" : "%s" },
    "ERROR_QUERY_WINDOW_AFTER_MODIFICATION" : { "code" : 1595, "message" : "window operation after data-modification" },
    "ERROR_CURSOR_NOT_FOUND"       : { "code" : 1600, "message" : "cursor not found" },
    "ERROR_CURSOR_BUSY"            : { "code" : 1601, "message" : "cursor is busy" },
    "ERROR_VALIDATION_FAILED"      : { "code" : 1620, "message" : "schema validation failed" },
    "ERROR_VALIDATION_BAD_PARAMETER" : { "code" : 1621, "message" : "invalid schema validation parameter" },
    "ERROR_TRANSACTION_INTERNAL"   : { "code" : 1650, "message" : "internal transaction error" },
    "ERROR_TRANSACTION_NESTED"     : { "code" : 1651, "message" : "nested transactions detected" },
    "ERROR_TRANSACTION_UNREGISTERED_COLLECTION" : { "code" : 1652, "message" : "unregistered collection used in transaction" },
    "ERROR_TRANSACTION_DISALLOWED_OPERATION" : { "code" : 1653, "message" : "disallowed operation inside transaction" },
    "ERROR_TRANSACTION_ABORTED"    : { "code" : 1654, "message" : "transaction aborted" },
    "ERROR_TRANSACTION_NOT_FOUND"  : { "code" : 1655, "message" : "transaction not found" },
    "ERROR_USER_INVALID_NAME"      : { "code" : 1700, "message" : "invalid user name" },
    "ERROR_USER_DUPLICATE"         : { "code" : 1702, "message" : "duplicate user" },
    "ERROR_USER_NOT_FOUND"         : { "code" : 1703, "message" : "user not found" },
    "ERROR_USER_EXTERNAL"          : { "code" : 1705, "message" : "user is external" },
    "ERROR_SERVICE_DOWNLOAD_FAILED" : { "code" : 1752, "message" : "service download failed" },
    "ERROR_SERVICE_UPLOAD_FAILED"  : { "code" : 1753, "message" : "service upload failed" },
    "ERROR_LDAP_CANNOT_INIT"       : { "code" : 1800, "message" : "cannot init a LDAP connection" },
    "ERROR_LDAP_CANNOT_SET_OPTION" : { "code" : 1801, "message" : "cannot set a LDAP option" },
    "ERROR_LDAP_CANNOT_BIND"       : { "code" : 1802, "message" : "cannot bind to a LDAP server" },
    "ERROR_LDAP_CANNOT_UNBIND"     : { "code" : 1803, "message" : "cannot unbind from a LDAP server" },
    "ERROR_LDAP_CANNOT_SEARCH"     : { "code" : 1804, "message" : "cannot issue a LDAP search" },
    "ERROR_LDAP_CANNOT_START_TLS"  : { "code" : 1805, "message" : "cannot start a TLS LDAP session" },
    "ERROR_LDAP_FOUND_NO_OBJECTS"  : { "code" : 1806, "message" : "LDAP didn't found any objects" },
    "ERROR_LDAP_NOT_ONE_USER_FOUND" : { "code" : 1807, "message" : "LDAP found zero ore more than one user" },
    "ERROR_LDAP_USER_NOT_IDENTIFIED" : { "code" : 1808, "message" : "LDAP found a user, but its not the desired one" },
    "ERROR_LDAP_OPERATIONS_ERROR"  : { "code" : 1809, "message" : "LDAP returned an operations error" },
    "ERROR_LDAP_INVALID_MODE"      : { "code" : 1820, "message" : "invalid ldap mode" },
    "ERROR_TASK_INVALID_ID"        : { "code" : 1850, "message" : "invalid task id" },
    "ERROR_TASK_DUPLICATE_ID"      : { "code" : 1851, "message" : "duplicate task id" },
    "ERROR_TASK_NOT_FOUND"         : { "code" : 1852, "message" : "task not found" },
    "ERROR_GRAPH_INVALID_GRAPH"    : { "code" : 1901, "message" : "invalid graph" },
    "ERROR_GRAPH_INVALID_EDGE"     : { "code" : 1906, "message" : "invalid edge" },
    "ERROR_GRAPH_TOO_MANY_ITERATIONS" : { "code" : 1909, "message" : "too many iterations - try increasing the value of 'maxIterations'" },
    "ERROR_GRAPH_INVALID_FILTER_RESULT" : { "code" : 1910, "message" : "invalid filter result" },
    "ERROR_GRAPH_COLLECTION_MULTI_USE" : { "code" : 1920, "message" : "multi use of edge collection in edge def" },
    "ERROR_GRAPH_COLLECTION_USE_IN_MULTI_GRAPHS" : { "code" : 1921, "message" : "edge collection already used in edge def" },
    "ERROR_GRAPH_CREATE_MISSING_NAME" : { "code" : 1922, "message" : "missing graph name" },
    "ERROR_GRAPH_CREATE_MALFORMED_EDGE_DEFINITION" : { "code" : 1923, "message" : "malformed edge definition" },
    "ERROR_GRAPH_NOT_FOUND"        : { "code" : 1924, "message" : "graph '%s' not found" },
    "ERROR_GRAPH_DUPLICATE"        : { "code" : 1925, "message" : "graph already exists" },
    "ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST" : { "code" : 1926, "message" : "vertex collection does not exist or is not part of the graph" },
    "ERROR_GRAPH_WRONG_COLLECTION_TYPE_VERTEX" : { "code" : 1927, "message" : "collection not a vertex collection" },
    "ERROR_GRAPH_NOT_IN_ORPHAN_COLLECTION" : { "code" : 1928, "message" : "collection is not in list of orphan collections" },
    "ERROR_GRAPH_COLLECTION_USED_IN_EDGE_DEF" : { "code" : 1929, "message" : "collection already used in edge def" },
    "ERROR_GRAPH_EDGE_COLLECTION_NOT_USED" : { "code" : 1930, "message" : "edge collection not used in graph" },
    "ERROR_GRAPH_NO_GRAPH_COLLECTION" : { "code" : 1932, "message" : "collection _graphs does not exist" },
    "ERROR_GRAPH_INVALID_NUMBER_OF_ARGUMENTS" : { "code" : 1935, "message" : "Invalid number of arguments. Expected: " },
    "ERROR_GRAPH_INVALID_PARAMETER" : { "code" : 1936, "message" : "Invalid parameter type." },
    "ERROR_GRAPH_COLLECTION_USED_IN_ORPHANS" : { "code" : 1938, "message" : "collection used in orphans" },
    "ERROR_GRAPH_EDGE_COL_DOES_NOT_EXIST" : { "code" : 1939, "message" : "edge collection does not exist or is not part of the graph" },
    "ERROR_GRAPH_EMPTY"            : { "code" : 1940, "message" : "empty graph" },
    "ERROR_GRAPH_INTERNAL_DATA_CORRUPT" : { "code" : 1941, "message" : "internal graph data corrupt" },
    "ERROR_GRAPH_CREATE_MALFORMED_ORPHAN_LIST" : { "code" : 1943, "message" : "malformed orphan list" },
    "ERROR_GRAPH_EDGE_DEFINITION_IS_DOCUMENT" : { "code" : 1944, "message" : "edge definition collection is a document collection" },
    "ERROR_GRAPH_COLLECTION_IS_INITIAL" : { "code" : 1945, "message" : "initial collection is not allowed to be removed manually" },
    "ERROR_GRAPH_NO_INITIAL_COLLECTION" : { "code" : 1946, "message" : "no valid initial collection found" },
    "ERROR_GRAPH_REFERENCED_VERTEX_COLLECTION_NOT_USED" : { "code" : 1947, "message" : "referenced vertex collection is not part of the graph" },
    "ERROR_GRAPH_NEGATIVE_EDGE_WEIGHT" : { "code" : 1948, "message" : "negative edge weight found" },
    "ERROR_SESSION_UNKNOWN"        : { "code" : 1950, "message" : "unknown session" },
    "ERROR_SESSION_EXPIRED"        : { "code" : 1951, "message" : "session expired" },
    "ERROR_SIMPLE_CLIENT_UNKNOWN_ERROR" : { "code" : 2000, "message" : "unknown client error" },
    "ERROR_SIMPLE_CLIENT_COULD_NOT_CONNECT" : { "code" : 2001, "message" : "could not connect to server" },
    "ERROR_SIMPLE_CLIENT_COULD_NOT_WRITE" : { "code" : 2002, "message" : "could not write to server" },
    "ERROR_SIMPLE_CLIENT_COULD_NOT_READ" : { "code" : 2003, "message" : "could not read from server" },
    "ERROR_WAS_ERLAUBE"            : { "code" : 2019, "message" : "was erlaube?!" },
    "ERROR_INTERNAL_AQL"           : { "code" : 2200, "message" : "General internal AQL error" },
    "ERROR_MALFORMED_MANIFEST_FILE" : { "code" : 3000, "message" : "failed to parse manifest file" },
    "ERROR_INVALID_SERVICE_MANIFEST" : { "code" : 3001, "message" : "manifest file is invalid" },
    "ERROR_SERVICE_FILES_MISSING"  : { "code" : 3002, "message" : "service files missing" },
    "ERROR_SERVICE_FILES_OUTDATED" : { "code" : 3003, "message" : "service files outdated" },
    "ERROR_INVALID_FOXX_OPTIONS"   : { "code" : 3004, "message" : "service options are invalid" },
    "ERROR_INVALID_MOUNTPOINT"     : { "code" : 3007, "message" : "invalid mountpath" },
    "ERROR_SERVICE_NOT_FOUND"      : { "code" : 3009, "message" : "service not found" },
    "ERROR_SERVICE_NEEDS_CONFIGURATION" : { "code" : 3010, "message" : "service needs configuration" },
    "ERROR_SERVICE_MOUNTPOINT_CONFLICT" : { "code" : 3011, "message" : "service already exists" },
    "ERROR_SERVICE_MANIFEST_NOT_FOUND" : { "code" : 3012, "message" : "missing manifest file" },
    "ERROR_SERVICE_OPTIONS_MALFORMED" : { "code" : 3013, "message" : "failed to parse service options" },
    "ERROR_SERVICE_SOURCE_NOT_FOUND" : { "code" : 3014, "message" : "source path not found" },
    "ERROR_SERVICE_SOURCE_ERROR"   : { "code" : 3015, "message" : "error resolving source" },
    "ERROR_SERVICE_UNKNOWN_SCRIPT" : { "code" : 3016, "message" : "unknown script" },
    "ERROR_SERVICE_API_DISABLED"   : { "code" : 3099, "message" : "service api disabled" },
    "ERROR_MODULE_NOT_FOUND"       : { "code" : 3100, "message" : "cannot locate module" },
    "ERROR_MODULE_SYNTAX_ERROR"    : { "code" : 3101, "message" : "syntax error in module" },
    "ERROR_MODULE_FAILURE"         : { "code" : 3103, "message" : "failed to invoke module" },
    "ERROR_NO_SMART_COLLECTION"    : { "code" : 4000, "message" : "collection is not smart" },
    "ERROR_NO_SMART_GRAPH_ATTRIBUTE" : { "code" : 4001, "message" : "smart graph attribute not given" },
    "ERROR_CANNOT_DROP_SMART_COLLECTION" : { "code" : 4002, "message" : "cannot drop this smart collection" },
    "ERROR_KEY_MUST_BE_PREFIXED_WITH_SMART_GRAPH_ATTRIBUTE" : { "code" : 4003, "message" : "in smart vertex collections _key must be a string and prefixed with the value of the smart graph attribute" },
    "ERROR_ILLEGAL_SMART_GRAPH_ATTRIBUTE" : { "code" : 4004, "message" : "attribute cannot be used as smart graph attribute" },
    "ERROR_SMART_GRAPH_ATTRIBUTE_MISMATCH" : { "code" : 4005, "message" : "smart graph attribute mismatch" },
    "ERROR_INVALID_SMART_JOIN_ATTRIBUTE" : { "code" : 4006, "message" : "invalid smart join attribute declaration" },
    "ERROR_KEY_MUST_BE_PREFIXED_WITH_SMART_JOIN_ATTRIBUTE" : { "code" : 4007, "message" : "shard key value must be prefixed with the value of the smart join attribute" },
    "ERROR_NO_SMART_JOIN_ATTRIBUTE" : { "code" : 4008, "message" : "smart join attribute not given or invalid" },
    "ERROR_CLUSTER_MUST_NOT_CHANGE_SMART_JOIN_ATTRIBUTE" : { "code" : 4009, "message" : "must not change the value of the smartJoinAttribute" },
    "ERROR_INVALID_DISJOINT_SMART_EDGE" : { "code" : 4010, "message" : "non disjoint edge found" },
    "ERROR_UNSUPPORTED_CHANGE_IN_SMART_TO_SATELLITE_DISJOINT_EDGE_DIRECTION" : { "code" : 4011, "message" : "Unsupported alternating Smart and Satellite in Disjoint SmartGraph." },
    "ERROR_AGENCY_MALFORMED_GOSSIP_MESSAGE" : { "code" : 20001, "message" : "malformed gossip message" },
    "ERROR_AGENCY_MALFORMED_INQUIRE_REQUEST" : { "code" : 20002, "message" : "malformed inquire request" },
    "ERROR_AGENCY_INFORM_MUST_BE_OBJECT" : { "code" : 20011, "message" : "Inform message must be an object." },
    "ERROR_AGENCY_INFORM_MUST_CONTAIN_TERM" : { "code" : 20012, "message" : "Inform message must contain uint parameter 'term'" },
    "ERROR_AGENCY_INFORM_MUST_CONTAIN_ID" : { "code" : 20013, "message" : "Inform message must contain string parameter 'id'" },
    "ERROR_AGENCY_INFORM_MUST_CONTAIN_ACTIVE" : { "code" : 20014, "message" : "Inform message must contain array 'active'" },
    "ERROR_AGENCY_INFORM_MUST_CONTAIN_POOL" : { "code" : 20015, "message" : "Inform message must contain object 'pool'" },
    "ERROR_AGENCY_INFORM_MUST_CONTAIN_MIN_PING" : { "code" : 20016, "message" : "Inform message must contain object 'min ping'" },
    "ERROR_AGENCY_INFORM_MUST_CONTAIN_MAX_PING" : { "code" : 20017, "message" : "Inform message must contain object 'max ping'" },
    "ERROR_AGENCY_INFORM_MUST_CONTAIN_TIMEOUT_MULT" : { "code" : 20018, "message" : "Inform message must contain object 'timeoutMult'" },
    "ERROR_AGENCY_CANNOT_REBUILD_DBS" : { "code" : 20021, "message" : "Cannot rebuild readDB and spearHead" },
    "ERROR_AGENCY_MALFORMED_TRANSACTION" : { "code" : 20030, "message" : "malformed agency transaction" },
    "ERROR_SUPERVISION_GENERAL_FAILURE" : { "code" : 20501, "message" : "general supervision failure" },
    "ERROR_QUEUE_FULL"             : { "code" : 21003, "message" : "queue is full" },
    "ERROR_QUEUE_TIME_REQUIREMENT_VIOLATED" : { "code" : 21004, "message" : "queue time violated" },
    "ERROR_ACTION_UNFINISHED"      : { "code" : 6003, "message" : "maintenance action still processing" },
    "ERROR_HOT_BACKUP_INTERNAL"    : { "code" : 7001, "message" : "internal hot backup error" },
    "ERROR_HOT_RESTORE_INTERNAL"   : { "code" : 7002, "message" : "internal hot restore error" },
    "ERROR_BACKUP_TOPOLOGY"        : { "code" : 7003, "message" : "backup does not match this topology" },
    "ERROR_NO_SPACE_LEFT_ON_DEVICE" : { "code" : 7004, "message" : "no space left on device" },
    "ERROR_FAILED_TO_UPLOAD_BACKUP" : { "code" : 7005, "message" : "failed to upload hot backup set to remote target" },
    "ERROR_FAILED_TO_DOWNLOAD_BACKUP" : { "code" : 7006, "message" : "failed to download hot backup set from remote source" },
    "ERROR_NO_SUCH_HOT_BACKUP"     : { "code" : 7007, "message" : "no such hot backup set can be found" },
    "ERROR_REMOTE_REPOSITORY_CONFIG_BAD" : { "code" : 7008, "message" : "remote hotback repository configuration error" },
    "ERROR_LOCAL_LOCK_FAILED"      : { "code" : 7009, "message" : "some db servers cannot be reached for transaction locks" },
    "ERROR_LOCAL_LOCK_RETRY"       : { "code" : 7010, "message" : "some db servers cannot be reached for transaction locks" },
    "ERROR_HOT_BACKUP_CONFLICT"    : { "code" : 7011, "message" : "hot backup conflict" },
    "ERROR_HOT_BACKUP_DBSERVERS_AWOL" : { "code" : 7012, "message" : "hot backup not all db servers reachable" },
    "ERROR_CLUSTER_COULD_NOT_MODIFY_ANALYZERS_IN_PLAN" : { "code" : 7021, "message" : "analyzers in plan could not be modified" },
    "ERROR_LICENSE_EXPIRED_OR_INVALID" : { "code" : 9001, "message" : "license has expired or is invalid" },
    "ERROR_LICENSE_SIGNATURE_VERIFICATION" : { "code" : 9002, "message" : "license verification failed" },
    "ERROR_LICENSE_NON_MATCHING_ID" : { "code" : 9003, "message" : "non-matching license id" },
    "ERROR_LICENSE_FEATURE_NOT_ENABLED" : { "code" : 9004, "message" : "feature is not enabled by the license" },
    "ERROR_LICENSE_RESOURCE_EXHAUSTED" : { "code" : 9005, "message" : "the resource is exhausted" },
    "ERROR_LICENSE_INVALID"        : { "code" : 9006, "message" : "invalid license" },
    "ERROR_LICENSE_CONFLICT"       : { "code" : 9007, "message" : "conflicting license" },
    "ERROR_LICENSE_VALIDATION_FAILED" : { "code" : 9008, "message" : "failed to validate license signature" }
  };

  // For compatibility with <= 3.3
  internal.errors.ERROR_ARANGO_COLLECTION_NOT_FOUND = internal.errors.ERROR_ARANGO_DATA_SOURCE_NOT_FOUND;
}());

