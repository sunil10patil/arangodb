/*jshint strict: false */
/* global print */
// //////////////////////////////////////////////////////////////////////////////
// / @brief helper for JavaScript Tests
// /
// / @file
// /
// / DISCLAIMER
// /
// / Copyright 2010-2012 triagens GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License")
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     http://www.apache.org/licenses/LICENSE-2.0
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is triAGENS GmbH, Cologne, Germany
// /
// / @author Wilfried Goesgens
// / @author Copyright 2011-2012, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const internal = require('internal'); // OK: processCsvFile
const {
  runWithRetry,
  helper,
  deriveTestSuite,
  deriveTestSuiteWithnamespace,
  typeName,
  isEqual,
  compareStringIds,
  endpointToURL,
  versionHas,
  isEnterprise,
} = require('@arangodb/test-helper-common');
const fs = require('fs');
const _ = require('lodash');
const inst = require('@arangodb/testutils/instance');
const request = require('@arangodb/request');
const arangosh = require('@arangodb/arangosh');
const jsunity = require('jsunity');
const arango = internal.arango;
const db = internal.db;
const {assertTrue, assertFalse, assertEqual} = jsunity.jsUnity.assertions;
const isServer = require("@arangodb").isServer;

exports.runWithRetry = runWithRetry;
exports.isEnterprise = isEnterprise;
exports.versionHas = versionHas;
exports.helper = helper;
exports.deriveTestSuite = deriveTestSuite;
exports.deriveTestSuiteWithnamespace = deriveTestSuiteWithnamespace;
exports.typeName = typeName;
exports.isEqual = isEqual;
exports.compareStringIds = compareStringIds;

let instanceInfo = null;

exports.flushInstanceInfo = () => {
  instanceInfo = null;
};

function getInstanceInfo() {
  if (global.hasOwnProperty('instanceManger')) {
    return global.instanceManger;
  }
  if (instanceInfo === null) {
    instanceInfo = JSON.parse(internal.env.INSTANCEINFO);
    if (instanceInfo.arangods.length > 2) {
      instanceInfo.arangods.forEach(arangod => {
        arangod.id = fs.readFileSync(fs.join(arangod.dataDir, 'UUID')).toString();
      });
    }
  }
  return instanceInfo;
}

let reconnectRetry = exports.reconnectRetry = require('@arangodb/replication-common').reconnectRetry;

exports.clearAllFailurePoints = function () {
  const old = db._name();
  try {
    for (const server of exports.getDBServers()) {
      exports.debugClearFailAt(exports.getEndpointById(server.id));
    }
    for (const server of exports.getCoordinators()) {
      exports.debugClearFailAt(exports.getEndpointById(server.id));
    }
  } finally {
    // need to restore original database, as debugFailAt() can 
    // change into a different database...
    db._useDatabase(old);
  }
};

/// @brief set failure point
exports.debugCanUseFailAt = function (endpoint) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    
    let res = arango.GET_RAW('/_admin/debug/failat');
    return res.code === 200;
  } finally {
    reconnectRetry(primaryEndpoint, "_system", "root", "");
  }
};

/// @brief set failure point
exports.debugSetFailAt = function (endpoint, failAt) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let res = arango.PUT_RAW('/_admin/debug/failat/' + failAt, {});
    if (res.parsedBody !== true) {
      throw `Error setting failure point on ${endpoint}: "${res}"`;
    }
    return true;
  } finally {
    reconnectRetry(primaryEndpoint, "_system", "root", "");
  }
};

exports.debugResetRaceControl = function (endpoint) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let res = arango.DELETE_RAW('/_admin/debug/raceControl');
    if (res.code !== 200) {
      throw "Error resetting race control.";
    }
    return false;
  } finally {
    reconnectRetry(primaryEndpoint, "_system", "root", "");
  }
};

/// @brief remove failure point
exports.debugRemoveFailAt = function (endpoint, failAt) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let res = arango.DELETE_RAW('/_admin/debug/failat/' + failAt);
    if (res.code !== 200) {
      throw "Error removing failure point";
    }
    return true;
  } finally {
    reconnectRetry(primaryEndpoint, "_system", "root", "");
  }
};

exports.debugClearFailAt = function (endpoint) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let res = arango.DELETE_RAW('/_admin/debug/failat');
    if (res.code !== 200) {
      throw "Error removing failure points";
    }
    return true;
  } finally {
    reconnectRetry(primaryEndpoint, "_system", "root", "");
  }
};

exports.debugGetFailurePoints = function (endpoint) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let haveFailAt = arango.GET("/_admin/debug/failat") === true;
    if (haveFailAt) {
      let res = arango.GET_RAW('/_admin/debug/failat/all');
      if (res.code !== 200) {
        throw "Error checking failure points = " + JSON.stringify(res);
      }
      return res.parsedBody;
    }
  } finally {
    reconnectRetry(primaryEndpoint, "_system", "root", "");
  }
  return [];
};

exports.getChecksum = function (endpoint, name) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let res = arango.GET_RAW('/_api/collection/' + name + '/checksum');
    if (res.code !== 200) {
      throw "Error getting collection checksum";
    }
    return res.parsedBody.checksum;
  } finally {
    reconnectRetry(primaryEndpoint, "_system", "root", "");
  }
};

exports.getRawMetric = function (endpoint, tags) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    if (endpoint !== primaryEndpoint) {
      reconnectRetry(endpoint, db._name(), "root", "");
    }
    return arango.GET_RAW('/_admin/metrics' + tags);
  } finally {
    if (endpoint !== primaryEndpoint) {
      reconnectRetry(primaryEndpoint, db._name(), "root", "");
    }
  }
};

exports.getAllMetric = function (endpoint, tags) {
  let res = exports.getRawMetric(endpoint, tags);
  if (res.code !== 200) {
    throw "error fetching metric";
  }
  return res.body;
};

function getMetricName(text, name) {
  let re = new RegExp("^" + name);
  let matches = text.split('\n').filter((line) => !line.match(/^#/)).filter((line) => line.match(re));
  if (!matches.length) {
    throw "Metric " + name + " not found";
  }
  return Number(matches[0].replace(/^.*?(\{.*?\})?\s*([0-9.]+)$/, "$2"));
}

exports.getMetric = function (endpoint, name) {
  let text = exports.getAllMetric(endpoint, '');
  return getMetricName(text, name);
};

exports.getMetricSingle = function (name) {
  let res = arango.GET_RAW("/_admin/metrics");
  if (res.code !== 200) {
    throw "error fetching metric";
  }
  return getMetricName(res.body, name);
};

const debug = function (text) {
  console.warn(text);
};

const runShell = function(args, prefix) {
  let options = internal.options();

  let endpoint = arango.getEndpoint().replace(/\+vpp/, '').replace(/^http:/, 'tcp:').replace(/^https:/, 'ssl:').replace(/^vst:/, 'tcp:').replace(/^h2:/, 'tcp:');
  let moreArgs = {
    'javascript.startup-directory': options['javascript.startup-directory'],
    'server.endpoint': endpoint,
    'server.database': arango.getDatabaseName(),
    'server.username': arango.connectedUser(),
    'server.password': '',
    'log.foreground-tty': 'false',
    'log.output': 'file://' + prefix + '.log'
  };
  _.assign(args, moreArgs);
  let argv = internal.toArgv(args);

  for (let o in options['javascript.module-directory']) {
    argv.push('--javascript.module-directory');
    argv.push(options['javascript.module-directory'][o]);
  }

  let result = internal.executeExternal(global.ARANGOSH_BIN, argv, false /*usePipes*/);
  assertTrue(result.hasOwnProperty('pid'));
  let status = internal.statusExternal(result.pid);
  assertEqual(status.status, "RUNNING");
  return result.pid;
};

const buildCode = function(dbname, key, command, cn, duration) {
  let file = fs.getTempFile() + "-" + key;
  fs.write(file, `
(function() {
// For chaos tests additional 10 secs might be not enough, so add 3 minutes buffer
require('internal').SetGlobalExecutionDeadlineTo((${duration} + 180) * 1000);
let tries = 0;
while (true) {
  if (++tries % 3 === 0) {
    try {
      if (db['${cn}'].exists('stop')) {
        break;
      }
    } catch (err) {
      // the operation may actually fail because of failure points
    }
  }
  ${command}
}
let saveTries = 0;
while (++saveTries < 100) {
  try {
    /* saving our status may actually fail because of failure points set */
    db['${cn}'].insert({ _key: "${key}", done: true, iterations: tries });
    break;
  } catch (err) {
    /* try again */
  }
}
})();
  `);

  let args = {'javascript.execute': file};
  args["--server.database"] = dbname;
  let pid = runShell(args, file);
  debug("started client with key '" + key + "', pid " + pid + ", args: " + JSON.stringify(args));
  return { key, file, pid };
};
exports.runShell = runShell;

const abortSignal = 6;

exports.runParallelArangoshTests = function (tests, duration, cn) {
  assertTrue(fs.isFile(global.ARANGOSH_BIN), "arangosh executable not found!");
  
  assertFalse(db[cn].exists("stop"));
  let clients = [];
  debug("starting " + tests.length + " test clients");
  try {
    tests.forEach(function (test) {
      let key = test[0];
      let code = test[1];
      let client = buildCode(db._name(), key, code, cn, duration);
      client.done = false;
      client.failed = true; // assume the worst
      clients.push(client);
    });

    debug("running test for " + duration + " s...");

    for (let count = 0; count < duration; count ++) {
      internal.sleep(1);
      clients.forEach(function (client) {
        if (!client.done) {
          let status = internal.statusExternal(client.pid, false);
          if (status.status !== 'RUNNING') {
            client.done = true;
            client.failed = true;
            debug(`Client ${client.pid} exited before the duration end. Aborting tests: ${JSON.stringify(status)}`);
            count = duration + 10;
          }
        }
      });
      if (count >= duration + 10) {
        clients.forEach(function (client) {
          if (!client.done) {
            debug(`force terminating ${client.pid} since we're aborting the tests`);
            internal.killExternal(client.pid, abortSignal);
            internal.statusExternal(client.pid, false);
            client.failed = true;
          }
        });
      }
    }

    // clear failure points
    debug("clearing failure points");
    exports.clearAllFailurePoints();
  
    debug("stopping all test clients");
    // broad cast stop signal
    assertFalse(db[cn].exists("stop"));
    let saveTries = 0;
    while (++saveTries < 100) {
      try {
        // saving our stop signal may actually fail because of failure points set
        db[cn].insert({ _key: "stop" }, { overwriteMode: "ignore" });
        break;
      } catch (err) {
        // try again
      }
    }
    let tries = 0;
    const allClientsDone = () => clients.every(client => client.done);
    while (++tries < 120) {
      clients.forEach(function (client) {
        if (!client.done) {
          let status = internal.statusExternal(client.pid);
          if (status.status === 'NOT-FOUND' || status.status === 'TERMINATED') {
            client.done = true;
          }
          if (status.status === 'TERMINATED' && status.exit === 0) {
            client.failed = false;
          }
        }
      });

      if (allClientsDone()) {
        break;
      }

      internal.sleep(0.5);
    }

    if (!allClientsDone()) {
      console.warn("Not all shells could be joined!");
    }
  } finally {
    clients.forEach(function(client) {
      try {
        if (!client.failed) {
          fs.remove(client.file);
        }
      } catch (err) { }

      const logfile = client.file + '.log';
      if (client.failed) {
        if (fs.exists(logfile)) {
          debug("test client with pid " + client.pid + " has failed and wrote logfile: " + fs.readFileSync(logfile).toString());
        } else {
          debug("test client with pid " + client.pid + " has failed and did not write a logfile");
        }
      }
      try {
        if (!client.failed) {
          fs.remove(logfile);
        }
      } catch (err) { }

      if (!client.done) {
        // hard-kill all running instances
        try {
          let status = internal.statusExternal(client.pid).status;
          if (status === 'RUNNING') {
            debug("forcefully killing test client with pid " + client.pid);
            internal.killExternal(client.pid, 9 /*SIGKILL*/);
          }
        } catch (err) { }
      }
    });
  }
  return clients;
};

exports.waitForShardsInSync = function (cn, timeout, minimumRequiredFollowers = 0) {
  if (!timeout) {
    timeout = 300;
  }
  let start = internal.time();
  while (true) {
    if (internal.time() - start > timeout) {
      print(Date() + " Shards were not getting in sync in time, giving up!");
      assertTrue(false, "Shards were not getting in sync in time, giving up!");
      return;
    }
    let shardDistribution = arango.GET("/_admin/cluster/shardDistribution");
    assertFalse(shardDistribution.error);
    assertEqual(200, shardDistribution.code);
    let collInfo = shardDistribution.results[cn];
    let shards = Object.keys(collInfo.Plan);
    let insync = 0;
    for (let s of shards) {
      if (collInfo.Plan[s].followers.length === collInfo.Current[s].followers.length
        && minimumRequiredFollowers <= collInfo.Plan[s].followers.length) {
        ++insync;
      }
    }
    if (insync === shards.length) {
      return;
    }
    console.warn("insync=", insync, ", collInfo=", collInfo, internal.time() - start);
    internal.wait(1);
  }
};

exports.getControleableServers = function (role) {
  return global.theInstanceManager.arangods.filter((instance) => instance.isRole(role));
};

// These functions lean on special runners to export the actual instance object into the global namespace.
exports.getCtrlAgents = function() {
  return exports.getControleableServers(inst.instanceRole.agent);
};
exports.getCtrlDBServers = function() {
  return exports.getControleableServers(inst.instanceRole.dbServer);
};
exports.getCtrlCoordinators = function() {
  return exports.getControleableServers(inst.instanceRole.coordinator);
};

exports.getServers = function (role) {
  const instanceInfo = getInstanceInfo();
  let ret = instanceInfo.arangods.filter(inst => inst.instanceRole === role);
  if (ret.length === 0) {
    throw new Error("No instance matched the type " + role);
  }
  return ret;
};

exports.getCoordinators = function () {
  return exports.getServers(inst.instanceRole.coordinator);
};
exports.getDBServers = function () {
  return exports.getServers(inst.instanceRole.dbServer);
};
exports.getAgents = function () {
  return exports.getServers(inst.instanceRole.agent);
};

exports.getServerById = function (id) {
  const instanceInfo = getInstanceInfo();
  return instanceInfo.arangods.find((d) => (d.id === id));
};

exports.getServersByType = function (type) {
  const isType = (d) => (d.instanceRole.toLowerCase() === type);
  const instanceInfo = getInstanceInfo();
  return instanceInfo.arangods.filter(isType);
};

exports.getEndpointById = function (id) {
  const toEndpoint = (d) => (d.endpoint);

  const instanceInfo = getInstanceInfo();
  const instance = instanceInfo.arangods.find(d => d.id === id);
  return endpointToURL(toEndpoint(instance));
};

exports.getUrlById = function (id) {
  const toUrl = (d) => (d.url);
  const instanceInfo = getInstanceInfo();
  return instanceInfo.arangods.filter((d) => (d.id === id))
    .map(toUrl)[0];
};

exports.getEndpointsByType = function (type) {
  const isType = (d) => (d.instanceRole.toLowerCase() === type);
  const toEndpoint = (d) => (d.endpoint);

  const instanceInfo = getInstanceInfo();
  return instanceInfo.arangods.filter(isType)
    .map(toEndpoint)
    .map(endpointToURL);
};

exports.triggerMetrics = function () {
  let coordinators = exports.getEndpointsByType("coordinator");
  exports.getRawMetric(coordinators[0], '?mode=write_global');
  for (let i = 1; i < coordinators.length; i++) {
    let c = coordinators[i];
    exports.getRawMetric(c, '?mode=trigger_global');
  }
  require("internal").sleep(2);
};

exports.getEndpoints = function (role) {
  return exports.getServers(role).map(instance => endpointToURL(instance.endpoint));
};

exports.getCoordinatorEndpoints = function () {
  return exports.getEndpoints(inst.instanceRole.coordinator);
};
exports.getDBServerEndpoints = function () {
  return exports.getEndpoints(inst.instanceRole.dbServer);
};
exports.getAgentEndpoints = function () {
  return exports.getEndpoints(inst.instanceRole.agent);
};

const callAgency = function (operation, body) {
  // Memoize the agents
  const getAgents = (function () {
    let agents;
    return function () {
      if (!agents) {
        agents = exports.getAgentEndpoints();
      }
      return agents;
    };
  }());
  const agents = getAgents();
  assertTrue(agents.length > 0, 'No agents present');
  const res = request.post({
    url: `${agents[0]}/_api/agency/${operation}`,
    body: JSON.stringify(body),
    timeout: 300,
  });
  assertTrue(res instanceof request.Response);
  assertTrue(res.hasOwnProperty('statusCode'), JSON.stringify(res));
  assertEqual(res.statusCode, 200, JSON.stringify(res));
  assertTrue(res.hasOwnProperty('json'));
  return arangosh.checkRequestResult(res.json);
};

// client-side API compatible to global.ArangoAgency
exports.agency = {
  get: function (key) {
    const res = callAgency('read', [[
      `/arango/${key}`,
    ]]);
    return res[0];
  },

  set: function (path, value) {
    callAgency('write', [[{
      [`/arango/${path}`]: {
        'op': 'set',
        'new': value,
      },
    }]]);
  },

  remove: function (path) {
    callAgency('write', [[{
      [`/arango/${path}`]: {
        'op': 'delete'
      },
    }]]);
  },

  call: callAgency,
  transact: (body) => callAgency("transact", body),

  increaseVersion: function (path) {
    callAgency('write', [[{
      [`/arango/${path}`]: {
        'op': 'increment',
      },
    }]]);
  },

  // TODO implement the rest...
};

exports.uniqid = function  () {
  return JSON.parse(db._connection.POST("/_admin/execute?returnAsJSON=true", "return global.ArangoClusterInfo.uniqid()"));
};

exports.AQL_EXPLAIN = function(query, bindVars, options) {
  let stmt = db._createStatement(query);
  if (typeof bindVars === "object") {
    stmt.bind(bindVars);
  }
  if (typeof options === "object") {
    stmt.setOptions(options);
  }
  return stmt.explain();
};

exports.AQL_EXECUTE = function(query, bindVars, options) {
  let cursor = db._query(query, bindVars, options);
  let extra = cursor.getExtra();
  return {
    json: cursor.toArray(),
    stats: extra.stats,
    warnings: extra.warnings,
    profile: extra.profile,
    plan: extra.plan,
    cached: cursor.cached};
};
