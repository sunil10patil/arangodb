/* jshint globalstrict:true, strict:true, maxlen: 5000 */
/* global assertTrue, assertFalse, assertEqual, assertMatch */

////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Copyright 2018, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

'use strict';

const jsunity = require("jsunity");

const db = require("internal").db;
const request = require("@arangodb/request");
const url = require('url');
const _ = require("lodash");
const errors = require("internal").errors;
const getCoordinatorEndpoints = require('@arangodb/test-helper').getCoordinatorEndpoints;

const servers = getCoordinatorEndpoints();

function QueriesSuite () {
  'use strict';
  let coordinators = [];

  function sendRequest(method, endpoint, body, usePrimary, headers) {
    let res;
    const i = usePrimary ? 0 : 1;
    try {
      const envelope = {
        json: true,
        method,
        url: `${coordinators[i]}${endpoint}`,
        headers
      };
      if (method !== 'GET') {
        envelope.body = body;
      }
      res = request(envelope);
    } catch(err) {
      console.error(`Exception processing ${method} ${endpoint}`, err.stack);
      return {};
    }

    if (typeof res.body === "string") {
      if (res.body === "") {
        res.body = {};
      } else {
        res.body = JSON.parse(res.body);
      }
    }
    return res;
  }

  let isInList = function (list, query) {
    return list.filter(function(data) { return data.query.indexOf(query) !== -1; }).length > 0;
  };
  
  return {
    setUp: function() {
      coordinators = getCoordinatorEndpoints();
      if (coordinators.length < 2) {
        throw new Error('Expecting at least two coordinators');
      }
    },

    tearDown: function() {
      coordinators = [];
    },
    
    testKillQueriesWrongId: function() {
      const query = "RETURN SLEEP(100000)";
      // start background query
      let result = sendRequest('POST', "/_api/cursor", { query }, true, { "x-arango-async" : "true" });
      assertEqual(result.status, 202);

      // wait for query to appear in list of running queries
      let tries = 0;
      while (++tries < 60) {
        result = sendRequest('GET', "/_api/query/current", null, true);
        assertEqual(result.status, 200);
        if (isInList(result.body, query)) {
          break;
        }
        require("internal").sleep(0.5);
      }

      result = sendRequest('GET', "/_api/query/current", null, true);
      assertEqual(result.status, 200);
      assertTrue(isInList(result.body, query));
      
      let queries = result.body.filter(function(data) { return data.query.indexOf(query) !== -1; });
      assertEqual(1, queries.length);

      let id = queries[0].id;
      assertEqual("string", typeof id);
      try {
        // kill the query, but with wrong id
        result = sendRequest('DELETE', "/_api/query/" + "12345" + id, null, true);
        assertEqual(result.status, 404);
        assertEqual(errors.ERROR_QUERY_NOT_FOUND.code, result.body.errorNum);
        assertMatch(/cannot find target server/, result.body.errorMessage);
        
        // kill the query, but with wrong id, on different server
        result = sendRequest('DELETE', "/_api/query/" + "12345" + id, null, false);
        assertEqual(result.status, 404);
        assertEqual(errors.ERROR_QUERY_NOT_FOUND.code, result.body.errorNum);
        assertMatch(/cannot find target server/, result.body.errorMessage);
      
        // query must not vanish from list of currently running queries
        result = sendRequest('GET', "/_api/query/current", null, true);
        assertEqual(result.status, 200);
        assertTrue(isInList(result.body, query));
        
        result = sendRequest('GET', "/_api/query/current", null, false);
        assertEqual(result.status, 200);
        assertTrue(isInList(result.body, query));
      } finally {
        // finally kill it
        sendRequest('DELETE', "/_api/query/" + id, null, true);
  
        // wait until the query is fully gone
        let tries = 0;
        while (++tries < 60) {
          result = sendRequest('GET', "/_api/query/current", null, true);
          assertEqual(result.status, 200);
          if (!isInList(result.body, query)) {
            break;
          }
          require("internal").sleep(0.5);
        }
      }
    },
    
    testCurrentQueriesCoordinator: function() {
      const query = "RETURN SLEEP(100000)";
      // start background query
      let result = sendRequest('POST', "/_api/cursor", { query }, true, { "x-arango-async" : "true" });
      assertEqual(result.status, 202);

      // wait for query to appear in list of running queries
      let tries = 0;
      while (++tries < 60) {
        result = sendRequest('GET', "/_api/query/current", null, true);
        assertEqual(result.status, 200);
        if (isInList(result.body, query)) {
          break;
        }
        require("internal").sleep(0.5);
      }

      result = sendRequest('GET', "/_api/query/current", null, true);
      assertEqual(result.status, 200);
      assertTrue(isInList(result.body, query));
      
      result = sendRequest('GET', "/_api/query/current", null, false);
      assertEqual(result.status, 200);
      assertTrue(isInList(result.body, query));

      let queries = result.body.filter(function(data) { return data.query.indexOf(query) !== -1; });
      assertEqual(1, queries.length);

      let id = queries[0].id;
      assertEqual("string", typeof id);

      // kill the query
      result = sendRequest('DELETE', "/_api/query/" + id, null, false);
      assertEqual(result.status, 200);
    
      // query must vanish from list of currently running queries
      tries = 0;
      while (++tries < 60) {
        result = sendRequest('GET', "/_api/query/current", null, true);
        assertEqual(result.status, 200);
        if (!isInList(result.body, query)) {
          break;
        }
        require("internal").sleep(0.5);
      }

      result = sendRequest('GET', "/_api/query/current", null, true);
      assertEqual(result.status, 200);
      assertFalse(isInList(result.body, query));
      
      result = sendRequest('GET', "/_api/query/current", null, false);
      assertEqual(result.status, 200);
      assertFalse(isInList(result.body, query));
    },
    
    testCurrentQueriesDBServer: function() {
      const cn = "UnitTestsCollection";
      db._drop(cn);
      let c = db._create(cn, { numberOfShards: 5 });
      try {
        const query = "FOR i IN 1..10000000 INSERT {} INTO " + cn;
        // start background query
        let result = sendRequest('POST', "/_api/cursor", { query }, true, { "x-arango-async" : "true" });
        assertEqual(result.status, 202);

        // wait for query to appear in list of running queries
        let tries = 0;
        while (++tries < 60) {
          result = sendRequest('GET', "/_api/query/current", null, true);
          assertEqual(result.status, 200);
          if (isInList(result.body, query)) {
            break;
          }
          require("internal").sleep(0.5);
        }

        result = sendRequest('GET', "/_api/query/current", null, true);
        assertEqual(result.status, 200);
        assertTrue(isInList(result.body, query));
        
        result = sendRequest('GET', "/_api/query/current", null, false);
        assertEqual(result.status, 200);
        assertTrue(isInList(result.body, query));

        let queries = result.body.filter(function(data) { return data.query.indexOf(query) !== -1; });
        assertEqual(1, queries.length);

        let id = queries[0].id;
        assertEqual("string", typeof id);

        // kill the query
        result = sendRequest('DELETE', "/_api/query/" + id, null, false);
        assertEqual(result.status, 200);
      
        // query must vanish from list of currently running queries
        tries = 0;
        while (++tries < 60) {
          result = sendRequest('GET', "/_api/query/current", null, true);
          assertEqual(result.status, 200);
          if (!isInList(result.body, query)) {
            break;
          }
          require("internal").sleep(0.5);
        }

        result = sendRequest('GET', "/_api/query/current", null, true);
        assertEqual(result.status, 200);
        assertFalse(isInList(result.body, query));
        
        result = sendRequest('GET', "/_api/query/current", null, false);
        assertEqual(result.status, 200);
        assertFalse(isInList(result.body, query));
      } catch (err) {
      } finally {
        db._drop(cn);
      }
    },

    testSlowQueries: function() {
      const query = "RETURN SLEEP(11)";
      db._query(query); 

      let result = sendRequest('GET', "/_api/query/slow", null, true);
      assertEqual(result.status, 200);
      assertTrue(isInList(result.body, query));
     
      result = sendRequest('GET', "/_api/query/slow", null, false);
      assertEqual(result.status, 200);
      assertTrue(isInList(result.body, query));
      
      // clear list of slow queries
      result = sendRequest('DELETE', "/_api/query/slow", null, false);
      assertEqual(result.status, 200);
      
      result = sendRequest('GET', "/_api/query/slow", null, true);
      assertEqual(result.status, 200);
      assertFalse(isInList(result.body, query));
     
      result = sendRequest('GET', "/_api/query/slow", null, false);
      assertEqual(result.status, 200);
      assertFalse(isInList(result.body, query));
    },
    
  };
}

jsunity.run(QueriesSuite);

return jsunity.done();
