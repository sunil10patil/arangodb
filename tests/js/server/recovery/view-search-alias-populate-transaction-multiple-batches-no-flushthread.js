/* jshint globalstrict:false, strict:false, unused : false */
/* global assertEqual, assertTrue, assertFalse, assertNull, fail, AQL_EXECUTE */
////////////////////////////////////////////////////////////////////////////////
/// @brief recovery tests for views
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2022 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License")
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Andrey Abramov
/// @author Copyright 2022, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var db = require('@arangodb').db;
var internal = require('internal');
var jsunity = require('jsunity');

function runSetup () {
  'use strict';
  internal.debugClearFailAt();

  db._drop('UnitTestsRecoveryDummy');
  var c = db._create('UnitTestsRecoveryDummy');
  c.ensureIndex({ type: "inverted", name: "pupa", includeAllFields:true });

  var meta = { indexes: [ { collection: "UnitTestsRecoveryDummy", index: "pupa" } ] };
  db._dropView('UnitTestsRecoveryView');
  db._createView('UnitTestsRecoveryView', 'search-alias', meta);

  internal.wal.flush(true, true);
  internal.debugSetFailAt("RocksDBBackgroundThread::run");
  internal.wait(2); // make sure failure point takes effect

  var tx = {
    collections: {
      write: ['UnitTestsRecoveryDummy']
    },
    action: function() {
      const db = require('internal').db;
      var c = db.UnitTestsRecoveryDummy;
      for (let j = 0; j < 100; j++) {
        var values = [];
        for (let i = 0; i < 100; i++) {
          values.push({ a: "foo_" + i, b: "bar_" + i, c: i });
        }
        c.save(values);
      }
    },
    waitForSync: true
  };

  db._executeTransaction(tx);

  internal.debugTerminate('crashing server');
}

function recoverySuite () {
  'use strict';
  jsunity.jsUnity.attachAssertions();

  return {
    setUp: function () {},
    tearDown: function () {},

    testIResearchLinkPopulateTransactionNoFlushThread: function () {
      let checkView = function(viewName, indexName) {
        let v = db._view(viewName);
        assertEqual(v.name(), viewName);
        assertEqual(v.type(), 'search-alias');
        let indexes = v.properties().indexes;
        assertEqual(1, indexes.length);
        assertEqual(indexName, indexes[0].index);
        assertEqual("UnitTestsRecoveryDummy", indexes[0].collection);
      };
      checkView("UnitTestsRecoveryView", "pupa");

      var result = db._query("FOR doc IN UnitTestsRecoveryView SEARCH doc.c >= 0 OPTIONS {waitForSync: true} COLLECT WITH COUNT INTO length RETURN length").toArray();
      var expectedResult = db._query("FOR doc IN UnitTestsRecoveryDummy FILTER doc.c >= 0 COLLECT WITH COUNT INTO length RETURN length").toArray();
      assertEqual(expectedResult[0], result[0]);
    }
  };
}

function main (argv) {
  'use strict';
  if (argv[1] === 'setup') {
    runSetup();
    return 0;
  } else {
    jsunity.run(recoverySuite);
    return jsunity.writeDone().status ? 0 : 1;
  }
}
