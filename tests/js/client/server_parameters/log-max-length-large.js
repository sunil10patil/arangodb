/*jshint globalstrict:false, strict:false */
/* global getOptions, assertTrue, arango, assertMatch, assertEqual */

////////////////////////////////////////////////////////////////////////////////
/// @brief test for server startup options
///
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
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
/// Copyright holder is ArangoDB Inc, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2019, ArangoDB Inc, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

const fs = require('fs');

if (getOptions === true) {
  return {
    'log.max-entry-length': '1048576', 
    'log.output': 'file://' + fs.getTempFile() + '.$PID',
    'log.foreground-tty': 'false',
  };
}

const jsunity = require('jsunity');

function LoggerSuite() {
  'use strict';

  let oldLogLevel;

  return {
    setUpAll : function() {
      oldLogLevel = arango.GET("/_admin/log/level").general;
      arango.PUT("/_admin/log/level", { general: "info" });
    },

    tearDownAll : function () {
      // restore previous log level for "general" topic;
      arango.PUT("/_admin/log/level", { general: oldLogLevel });
    },

    testLogEntries: function() {
      let res = arango.POST("/_admin/execute?returnBodyAsJSON=true", `
require('console').log("testmann: start"); 
require('console').log("testmann: " + Array(32768).join("x")); 
require('console').log("testmann: " + Array(65536).join("y")); 
require('console').log("testmann: " + Array(1048576).join("z")); 
require('console').log("testmann: done"); 
return require('internal').options()["log.output"];
`);

      assertTrue(Array.isArray(res));
      assertTrue(res.length > 0);

      let logfile = res[res.length - 1].replace(/^file:\/\//, '');

      // log is buffered, so give it a few tries until the log messages appear
      let tries = 0;
      let filtered = [];
      while (++tries < 60) {
        let content = fs.readFileSync(logfile, 'ascii');
        let lines = content.split('\n');

        filtered = lines.filter((line) => {
          return line.match(/testmann: /);
        });

        if (filtered.length === 5) {
          break;
        }

        require("internal").sleep(0.5);
      }
      assertEqual(5, filtered.length);
          
      assertTrue(filtered[0].match(/testmann: start/));
      assertTrue(filtered[1].match(/testmann: xxxxxxxx/));
      assertTrue(filtered[2].match(/testmann: yyyyyyyy/));
      assertTrue(filtered[3].match(/testmann: zzzzzzzz/));
      assertTrue(filtered[4].match(/testmann: done/));

      assertTrue(filtered[1].length >= 32768, filtered[1].length);
      assertTrue(filtered[2].length >= 65536, filtered[2].length);
      // this line must have been shortened
      assertTrue(filtered[3].length === 1048576 + '...'.length, filtered[3].length);
    },

  };
}

jsunity.run(LoggerSuite);
return jsunity.done();
