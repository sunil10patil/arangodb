/* global AQL_EXECUTE */

'use strict';

// //////////////////////////////////////////////////////////////////////////////
// DISCLAIMER
//
// Copyright 2010-2013 triAGENS GmbH, Cologne, Germany
// Copyright 2016 ArangoDB GmbH, Cologne, Germany
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Copyright holder is ArangoDB GmbH, Cologne, Germany
//
// @author Michael Hackstein
// @author Heiko Kernbach
// @author Alan Plum
// //////////////////////////////////////////////////////////////////////////////

const joi = require('joi');
const dd = require('dedent');
const internal = require('internal');
const { db, query } = require('@arangodb');
const actions = require('@arangodb/actions');
const errors = require('@arangodb').errors;
const ArangoError = require('@arangodb').ArangoError;
const examples = require('@arangodb/graph-examples/example-graph');
const createRouter = require('@arangodb/foxx/router');
const users = require('@arangodb/users');
const cluster = require('@arangodb/cluster');
const generalGraph = require('@arangodb/general-graph');
const request = require('@arangodb/request');
const isEnterprise = require('internal').isEnterprise();
const explainer = require('@arangodb/aql/explainer');
const replication = require('@arangodb/replication');
const fs = require('fs');
const _ = require('lodash');

const ERROR_USER_NOT_FOUND = errors.ERROR_USER_NOT_FOUND.code;

const router = createRouter();
module.exports = router;

router.get('/index.html', (req, res) => {
  let encoding = req.headers['accept-encoding'];
  if (encoding && encoding.indexOf('gzip') >= 0) {
    // gzip-encode?
    res.set('Content-Encoding', 'gzip');
    res.sendFile(module.context.fileName('react/build/index.html.gz'));
  } else {
    res.sendFile(module.context.fileName('react/build/index.html'));
  }
  res.set('Content-Type', 'text/html; charset=utf-8');
  res.set('X-Frame-Options', 'DENY');
  res.set('X-XSS-Protection', '1; mode=block');
})
.response(['text/html']);

router.get('/config.js', function (req, res) {
  const scriptName = req.get('x-script-name');
  const basePath = req.trustProxy && scriptName || '';
  const isEnterprise = internal.isEnterprise();
  let ldapEnabled = false;
  if (isEnterprise) {
    if (internal.ldapEnabled()) {
      ldapEnabled = true;
    }
  }
  res.send(
    `var frontendConfig = ${JSON.stringify({
      basePath: basePath,
      db: req.database,
      isEnterprise: isEnterprise,
      authenticationEnabled: internal.authenticationEnabled(),
      ldapEnabled: ldapEnabled,
      isCluster: cluster.isCluster(),
      engine: db._engine().name,
      statisticsEnabled: internal.enabledStatistics(),
      metricsEnabled: internal.enabledMetrics(),
      statisticsInAllDatabases: internal.enabledStatisticsInAllDatabases(),
      foxxStoreEnabled: !internal.isFoxxStoreDisabled(),
      foxxApiEnabled: !internal.isFoxxApiDisabled(),
      foxxAllowInstallFromRemote: internal.foxxAllowInstallFromRemote(),
      clusterApiJwtPolicy: internal.clusterApiJwtPolicy(),
      minReplicationFactor: internal.minReplicationFactor,
      maxReplicationFactor: internal.maxReplicationFactor,
      defaultReplicationFactor: internal.defaultReplicationFactor,
      maxNumberOfShards: internal.maxNumberOfShards,
      extendedNames: internal.extendedNames,
      forceOneShard: internal.forceOneShard,
      sessionTimeout: internal.sessionTimeout,
      showMaintenanceStatus: true
    })}`
  );
})
.response(['text/javascript']);

router.get('/whoAmI', function (req, res) {
  res.json({user: req.arangoUser || null});
})
.summary('Return the current user')
.description(dd`
  Returns the current user or "null" if the user is not authenticated.
  Returns "false" if authentication is disabled.
`);

const authRouter = createRouter();
router.use(authRouter);

authRouter.use((req, res, next) => {
  if (internal.authenticationEnabled()) {
    if (!req.authorized) {
      res.throw('unauthorized');
    }
  }
  next();
});

router.get('/api/*', module.context.apiDocumentation({
  swaggerJson (req, res) {
    const API_DOCS = require(module.context.fileName('api-docs.json'));
    API_DOCS.basePath = `/_db/${encodeURIComponent(db._name())}`;
    res.json(API_DOCS);
  }
}))
.summary('System API documentation')
.description(dd`
  Mounts the system API documentation.
`);

authRouter.post('/query/profile', function (req, res) {
  const bindVars = req.body.bindVars;
  const query = req.body.query;
  const options = req.body.options || {};
  let msg = null;

  try {
    msg = explainer.profileQuery({
      query,
      bindVars: bindVars || {},
      options: {
        ...options,
        colors: false,
        profile: 2
      }
    }, false);
  } catch (e) {
    res.throw('bad request', e.message, {cause: e});
  }

  res.json({msg});
})
.body(joi.object({
  query: joi.string().required(),
  bindVars: joi.object().optional(),
  options: joi.object().optional()
}).required(), 'Query and bindVars to profile.')
.summary('Explains a query')
.description(dd`
  Profiles a query in a more user-friendly
`);

authRouter.post('/query/explain', function (req, res) {
  const bindVars = req.body.bindVars || {};
  const query = req.body.query;
  const id = req.body.id;
  const options = req.body.options || {};
  let msg = null;

  try {
    msg = explainer.explain({
      query: query,
      bindVars: bindVars,
      id: id
    }, {...options, colors: false}, false);
  } catch (e) {
    res.throw('bad request', e.message, {cause: e});
  }

  res.json({msg});
})
.body(joi.object({
  query: joi.string().required(),
  bindVars: joi.object().optional(),
  options: joi.object().optional(),
  batchSize: joi.number().optional(),
  id: joi.string().optional()
}).required(), 'Query and bindVars to explain.')
.summary('Explains a query')
.description(dd`
  Explains a query in a more user-friendly way than the query_api/explain
`);

authRouter.post('/query/debugDump', function (req, res) {
  const bindVars = req.body.bindVars || {};
  const query = req.body.query;
  const tmpDebugFolder = fs.getTempFile();
  const tmpDebugFileName = fs.join(tmpDebugFolder, 'debugDump.json');
  const tmpDebugZipFileName = fs.join(tmpDebugFolder, 'debugDump.zip');

  try {
    fs.makeDirectory(tmpDebugFolder);
  } catch (e) {
    require('console').error(e);
    res.throw('Server error, failed to create temp directory', e.message, {cause: e});
  }
  let options = {};
  if (req.body.examples) {
    options.anonymize = true;
    options.examples = true;
  }

  try {
    explainer.debugDump(tmpDebugFileName, query, bindVars, options);
  } catch (e) {
    res.throw('bad request', e.message, {cause: e});
  }
  try {
    fs.zipFile(tmpDebugZipFileName, tmpDebugFolder, ['debugDump.json']);
  } catch (e) {
    require('console').error(e);
    res.throw('Server error, failed to create zip file', e.message, {cause: e});
  }

  res.download(tmpDebugZipFileName, 'debugDump.zip');
})
.body(joi.object({
  query: joi.string().required(),
  bindVars: joi.object().optional(),
  examples: joi.bool().optional()
}).required(), 'Query and bindVars to generate debug dump output')
.summary('Generate Debug Output for Query')
.description(dd`
  Creates a debug output for the query in a zip file.
  This file includes the query plan and anonymized test data as
  well es collection information required for this query.
  It is extremely helpful for the ArangoDB team to get this archive
  and to reproduce your case. Whenever you submit a query based issue
  please attach this file and the Team can help you much faster with it.
`);

authRouter.post('/query/upload/:user', function (req, res) {
  let user = req.pathParams.user;

  try {
    user = users.document(user);
  } catch (e) {
    if (!e.isArangoError || e.errorNum !== ERROR_USER_NOT_FOUND) {
      throw e;
    }
    res.throw('not found');
  }

  if (!user.extra.queries) {
    user.extra.queries = [];
  }

  const existingQueries = user.extra.queries
  .map(query => query.name);

  for (const query of req.body) {
    if (existingQueries.indexOf(query.name) === -1) {
      existingQueries.push(query.name);
      user.extra.queries.push(query);
    }
  }

  users.update(user.user, undefined, undefined, user.extra);
  res.json(user.extra.queries);
})
.pathParam('user', joi.string().required(), 'Username. Ignored if authentication is enabled.')
.body(joi.array().items(joi.object({
  name: joi.string().required(),
  parameter: joi.any().optional(),
  value: joi.any().optional()
}).required()).required(), 'User query array to import.')
.error('not found', 'User does not exist.')
.summary('Upload user queries')
.description(dd`
  This function uploads all given user queries.
`);

authRouter.get('/query/download/:user', function (req, res) {
  let user = req.pathParams.user;

  try {
    user = users.document(user);
  } catch (e) {
    if (!e.isArangoError || e.errorNum !== ERROR_USER_NOT_FOUND) {
      throw e;
    }
    res.throw('not found');
  }

  const namePart = `${db._name()}-${user.user}`.replace(/[^-_a-z0-9]/gi, "_");
  res.attachment(`queries-${namePart}.json`);
  res.json(user.extra.queries || []);
})
.pathParam('user', joi.string().required(), 'Username. Ignored if authentication is enabled.')
.error('not found', 'User does not exist.')
.summary('Download stored queries')
.description(dd`
  Download and export all queries from the given username.
`);

authRouter.post('/query/result/download', function (req, res) {
   const result = db._query(req.body.query, req.body.bindVars).toArray();
   const namePart = `${db._name()}`.replace(/[^-_a-z0-9]/gi, "_");
   res.attachment(`results-${namePart}.json`);
   res.json(result);
 })
.body(joi.object({
  query: joi.string().required(),
  bindVars: joi.object().optional()
})
.required(), 'Query and bindVars to download.')
.error('bad request', 'The query is invalid or malformed.')
.summary('Download the result of a query')
.description(dd`
  This function downloads the result of a user query.
`);

authRouter.post('/graph-examples/create/:name', function (req, res) {
  const name = req.pathParams.name;

  if (['knows_graph', 'social', 'routeplanner', 'traversalGraph', 'kShortestPathsGraph', 'mps_graph', 'worldCountry', 'connectedComponentsGraph'].indexOf(name) === -1) {
    res.throw('not found');
  }
  if (generalGraph._list().indexOf(name) !== -1) {
    const error = new ArangoError({errorNum: errors.ERROR_GRAPH_DUPLICATE.code, errorMessage: errors.ERROR_GRAPH_DUPLICATE.message});
    res.throw(409, error);
  }
  let g = false;
  try {
    g = examples.loadGraph(name);
  } catch (e) {
    const error = new ArangoError({errorNum: e.errorNum, errorMessage: e.errorMessage});
    res.throw(actions.arangoErrorToHttpCode(e.errorNum), error);
  }
  res.json({error: !g});
})
.pathParam('name', joi.string().required(), 'Name of the example graph.')
.summary('Create example graphs')
.description(dd`
  Create one of the given example graphs.
`);

authRouter.post('/job', function (req, res) {
  let frontend = db._collection('_frontend');
  frontend.save(Object.assign(req.body, {model: 'job'}));
  res.json(true);
})
.body(joi.object({
  id: joi.any().required(),
  collection: joi.any().required(),
  type: joi.any().required(),
  desc: joi.any().required()
}).required())
.summary('Store job id of a running job')
.description(dd`
  Create a new job id entry in a specific system database with a given id.
`);

authRouter.delete('/job', function (req, res) {
  let arr = [];
  let frontend = db._collection('_frontend');

  if (frontend) {
    // get all job results and return before deletion
    _.each(frontend.all().toArray(), function (job) {
      let resp = request.put({
        url: '/_api/job/' + encodeURIComponent(job.id),
        json: true,
        headers: {
          'Authorization': req.headers.authorization
        }
      }).body;
      try {
        arr.push(JSON.parse(resp));
      } catch (ignore) {
      }
    });

    // actual deletion
    frontend.removeByExample({model: 'job'}, false);
  }
  res.json({result: arr});
})
.summary('Delete all jobs')
.description(dd`
  Delete all jobs in a specific system database with a given id.
`);

authRouter.delete('/job/:id', function (req, res) {
  let frontend = db._collection('_frontend');
  let toReturn = {};
  if (frontend) {
    // get the job result and return before deletion
    let resp = request.put({
      url: '/_db/' + encodeURIComponent(db._name()) + '/_api/job/' + encodeURIComponent(req.pathParams.id),
      json: true,
      headers: {
        'Authorization': req.headers.authorization
      }
    }).body;
    try {
      toReturn = JSON.parse(resp);
    } catch (ignore) {
    }

    // actual deletion
    frontend.removeByExample({id: req.pathParams.id}, false);
  }
  res.json(toReturn);
})
.summary('Delete a job id')
.description(dd`
  Delete an existing job id entry in a specific system database with a given id.
`);

authRouter.get('/job', function (req, res) {
  try {
    const result = db._frontend.all().toArray();
    res.json(result);
  } catch (err) {
    // collection not (yet) available
    res.json([]);
  }
})
.summary('Return all job ids.')
.description(dd`
  This function returns the job ids of all currently running jobs.
`);

//  * mode (server modes, numeric e.g.
//    0: No active replication found.
//    1: Replication per Database found.
//    2: Replication per Server found.
//    3: Active-Failover replication found.
authRouter.get('/replication/mode', function (req, res) {
  // this method is only allowed from within the _system database
  if (req.database !== '_system') {
    res.throw('not allowed');
  }

  let endpoints;
  let bearer;

  if (internal.authenticationEnabled()) {
    bearer = req.headers.authorization;
    endpoints = request.get('/_api/cluster/endpoints', {
      headers: {
        'Authorization': bearer
      }
    });
  } else {
    endpoints = request.get('/_api/cluster/endpoints');
  }

  let mode = 0;
  let role = null;
  // active failover
  if (endpoints.statusCode === 200 && endpoints.json.endpoints.length) {
    mode = 3;
    role = 'leader';
  } else {
    // check if global applier (ga) is running
    // if that is true, this node is replicating from another arangodb instance
    // (all databases)
    const globalApplierRunning = replication.globalApplier.state().state.running;
    let singleAppliers = [];

    if (globalApplierRunning) {
      mode = 2;
      role = 'follower';
    } else {
      // if ga is not running, check if a single applier is running (per each db)
      // if that is true,
      const allSingleAppliers = replication.applier.stateAll();
      _.each(allSingleAppliers, function (applier) {
        if (applier.state.running) {
          singleAppliers.push(db);
        }
      });

      if (singleAppliers.length > 0) {
        // some per db-level pulling replication was found
        mode = 1;
        role = 'follower';
      } else {
        // at this point, no active pulling replication settings were found at all
        // now checking logger state of this node
        if (replication.logger.state().clients.length > 0) {
          // found clients
          // currently one logger instance contains all dbs
          // there is currently no global logger state defined
          mode = 1; // TODO - how to find exactly out?
          role = 'leader';
        } else {
          // no clients found
          // no replication detected
          mode = 0;
          role = null;
        }
      }
    }
  }

  const result = {
    mode: mode,
    role: role
  };
  res.json(result);
})
.summary('Return the replication mode.')
.description(dd`
  This function returns the job ids of all currently running jobs.
`);

authRouter.get('/graph/:name', function (req, res) {
  var name = req.pathParams.name;
  var gm;
  if (isEnterprise) {
    gm = require('@arangodb/smart-graph');
  } else {
    gm = require('@arangodb/general-graph');
  }
  var notFoundString = "(attribute not found)";
  var colors = {
    default: [
      '#68BDF6',
      '#6DCE9E',
      '#FF756E',
      '#DE9BF9',
      '#FB95AF',
      '#FFD86E',
      '#A5ABB6'
    ],
    jans: ['rgba(166, 109, 161, 1)', 'rgba(64, 74, 83, 1)', 'rgba(90, 147, 189, 1)', 'rgba(153,63,0,1)', 'rgba(76,0,92,1)', 'rgba(25,25,25,1)', 'rgba(0,92,49,1)', 'rgba(43,206,72,1)', 'rgba(255,204,153,1)', 'rgba(128,128,128,1)', 'rgba(148,255,181,1)', 'rgba(143,124,0,1)', 'rgba(157,204,0,1)', 'rgba(194,0,136,1)', 'rgba(0,51,128,1)', 'rgba(255,164,5,1)', 'rgba(255,168,187,1)', 'rgba(66,102,0,1)', 'rgba(255,0,16,1)', 'rgba(94,241,242,1)', 'rgba(0,153,143,1)', 'rgba(224,255,102,1)', 'rgba(116,10,255,1)', 'rgba(153,0,0,1)', 'rgba(255,255,128,1)', 'rgba(255,255,0,1)', 'rgba(255,80,5,1)'],
    highContrast: [
      '#EACD3F',
      '#6F308A',
      '#DA6927',
      '#98CDE5',
      '#B81F34',
      '#C0BC82',
      '#7F7E80',
      '#61A547',
      '#60A446',
      '#D285B0',
      '#4477B3',
      '#DD8465',
      '#473896',
      '#E0A02F',
      '#8F2689',
      '#E7E655',
      '#7C1514',
      '#93AD3C',
      '#6D3312',
      '#D02C26',
      '#2A3415'
    ]
  };

  var graph;
  try {
    graph = gm._graph(name);
  } catch (e) {
    res.throw('bad request', e.errorMessage);
  }

  var verticesCollections = graph._vertexCollections();
  if (!verticesCollections || verticesCollections.length === 0) {
    res.throw('bad request', 'no vertex collections found for graph');
  }

  var vertexCollections = [];

  _.each(graph._vertexCollections(), function (vertex) {
    vertexCollections.push({
      name: vertex.name(),
      id: vertex._id
    });
  });

  var config;

  try {
    config = req.queryParams;
  } catch (e) {
    res.throw('bad request', e.message, {cause: e});
  }

  var getPseudoRandomStartVertex = function () {
    for (var i = 0; i < graph._vertexCollections().length; i++) {
      var vertexCollection = graph._vertexCollections()[i];
      let maxDoc =  db._collection(vertexCollection.name()).count();

      if (maxDoc === 0) {
        continue;
      }

      if (maxDoc > 1000) {
        maxDoc = 1000;
      }

      let randDoc = Math.floor(Math.random() * maxDoc);

      let potentialVertex = db._query(
        'FOR vertex IN @@vertexCollection LIMIT @skipN, 1 RETURN vertex',
        {
          '@vertexCollection': vertexCollection.name(),
          'skipN': randDoc
        }
      ).toArray()[0];

      if (potentialVertex) {
        return potentialVertex;
      }
    }

    return null;
  };

  var multipleIds;
  var startVertex; // will be "randomly" chosen if no start vertex is specified

  if (config.nodeStart) {
    if (config.nodeStart.indexOf(' ') > -1) {
      multipleIds = config.nodeStart.split(' ');
    } else {
      try {
        startVertex = db._document(config.nodeStart);
      } catch (e) {
        res.throw('bad request', e.message, {cause: e});
      }
      if (!startVertex) {
        startVertex = getPseudoRandomStartVertex();
      }
    }
  } else {
    startVertex = getPseudoRandomStartVertex();
  }

  var limit = 0;
  if (config.limit !== undefined) {
    if (config.limit.length > 0 && config.limit !== '0') {
      limit = parseInt(config.limit);
    }
  }

  var toReturn;
  if (startVertex === null) {
    toReturn = {
      empty: true,
      msg: 'Your graph is empty. We did not find a document in any available vertex collection.',
      settings: {
        vertexCollections: vertexCollections
      }
    };
    if (isEnterprise) {
      if (graph.__isSmart) {
        toReturn.settings.isSmart = graph.__isSmart;
        toReturn.settings.smartGraphAttribute = graph.__smartGraphAttribute;
      }
    }
  } else {
    var aqlQuery;
    var aqlQueries = [];

    let depth = parseInt(config.depth);

    if (config.query) {
      aqlQuery = config.query;
    } else {
      if (multipleIds) {
        /* TODO: uncomment after #75 fix
          aqlQuery =
            'FOR x IN ' + JSON.stringify(multipleIds) + ' ' +
            'FOR v, e, p IN 1..' + (depth || '2') + ' ANY x GRAPH "' + name + '"';
        */
        _.each(multipleIds, function (nodeid) {
          aqlQuery =
            'FOR v, e, p IN 1..' + (depth || '2') + ' ANY ' + JSON.stringify(nodeid) + ' GRAPH ' + JSON.stringify(name);
          if (limit !== 0) {
            aqlQuery += ' LIMIT ' + limit;
          }
          aqlQuery += ' RETURN p';
          aqlQueries.push(aqlQuery);
        });
      } else {
        aqlQuery =
          'FOR v, e, p IN 1..' + (depth || '2') + ' ANY ' + JSON.stringify(startVertex._id) + ' GRAPH ' + JSON.stringify(name);
        if (limit !== 0) {
          aqlQuery += ' LIMIT ' + limit;
        }
        aqlQuery += ' RETURN p';
      }
    }

    var getAttributeByKey = function (o, s) {
      s = s.replace(/\[(\w+)\]/g, '.$1');
      s = s.replace(/^\./, '');
      var a = s.split('.');
      for (var i = 0, n = a.length; i < n; ++i) {
        var k = a[i];
        if (k in o) {
          o = o[k];
        } else {
          return;
        }
      }
      return o;
    };

    var cursor;
    // get all nodes and edges, even if they are not connected
    // atm there is no server side function, so we need to get all docs
    // and edges of all related collections until the given limit is reached.
    if (config.mode === 'all') {
      var insertedEdges = 0;
      var insertedNodes = 0;
      var tmpEdges, tmpNodes;
      cursor = {
        json: [{
          vertices: [],
          edges: []
        }]
      };

      // get all nodes
      _.each(graph._vertexCollections(), function (node) {
        if (insertedNodes < limit || limit === 0) {
          tmpNodes = node.all().limit(limit).toArray();
          _.each(tmpNodes, function (n) {
            cursor.json[0].vertices.push(n);
          });
          insertedNodes += tmpNodes.length;
        }
      });
      // get all edges
      _.each(graph._edgeCollections(), function (edge) {
        if (insertedEdges < limit || limit === 0) {
          tmpEdges = edge.all().limit(limit).toArray();
          _.each(tmpEdges, function (e) {
            cursor.json[0].edges.push(e);
          });
          insertedEdges += tmpEdges.length;
        }
      });
    } else {
      // get all nodes and edges which are connected to the given start node
      try {
        if (aqlQueries.length === 0) {
          cursor = AQL_EXECUTE(aqlQuery);
        } else {
          var x;
          cursor = AQL_EXECUTE(aqlQueries[0]);
          for (var k = 1; k < aqlQueries.length; k++) {
            x = AQL_EXECUTE(aqlQueries[k]);
            _.each(x.json, function (val) {
              cursor.json.push(val);
            });
          }
        }
      } catch (e) {
        const error = new ArangoError({errorNum: e.errorNum, errorMessage: e.errorMessage});
        res.throw(actions.arangoErrorToHttpCode(e.errorNum), error);
      }
    }

    var nodesObj = {};
    var nodesArr = [];
    var nodeNames = {};
    var edgesObj = {};
    var edgesArr = [];
    var nodeEdgesCount = {};
    var handledEdges = {};

    var tmpObjEdges = {};
    var tmpObjNodes = {};

    _.each(cursor.json, function (obj) {
      var edgeLabel = '';
      var edgeObj;
      _.each(obj.edges, function (edge) {
        if (edge._to && edge._from) {
          if (config.edgeLabel && config.edgeLabel.length > 0) {
            // configure edge labels

            if (config.edgeLabel.indexOf('.') > -1) {
              edgeLabel = getAttributeByKey(edge, config.edgeLabel);
              if (nodeLabel === undefined || nodeLabel === '') {
                edgeLabel = edgeLabel._id;
              }
            } else {
              if (edge[config.edgeLabel] !== undefined) {
                if (typeof edge[config.edgeLabel] === 'string') {
                  edgeLabel = edge[config.edgeLabel];
                } else {
                  // in case we do not have a string here, we need to stringify it
                  // otherwise we might end up sending not displayable values.
                  edgeLabel = JSON.stringify(edge[config.edgeLabel]);
                }
              } else {
                // in case the document does not have the edgeLabel in it, return fallback string
                edgeLabel = notFoundString;
              }
            }

            if (typeof edgeLabel !== 'string') {
              edgeLabel = JSON.stringify(edgeLabel);
            }
            if (config.edgeLabelByCollection === 'true') {
              edgeLabel += ' - ' + edge._id.split('/')[0];
            }
          } else {
            if (config.edgeLabelByCollection === 'true') {
              edgeLabel = edge._id.split('/')[0];
            }
          }

          if (config.nodeSizeByEdges === 'true') {
            if (handledEdges[edge._id] === undefined) {
              handledEdges[edge._id] = true;

              if (nodeEdgesCount[edge._from] === undefined) {
                nodeEdgesCount[edge._from] = 1;
              } else {
                nodeEdgesCount[edge._from] += 1;
              }

              if (nodeEdgesCount[edge._to] === undefined) {
                nodeEdgesCount[edge._to] = 1;
              } else {
                nodeEdgesCount[edge._to] += 1;
              }
            }
          }

          edgeObj = {
            id: edge._id,
            source: edge._from,
            label: edgeLabel,
            color: config.edgeColor || '#cccccc',
            target: edge._to
          };

          if (config.edgeEditable === 'true') {
            edgeObj.size = 1;
          } else {
            edgeObj.size = 1;
          }

          if (config.edgeColorByCollection === 'true') {
            var coll = edge._id.split('/')[0];
            if (tmpObjEdges.hasOwnProperty(coll)) {
              edgeObj.color = tmpObjEdges[coll];
            } else {
              tmpObjEdges[coll] = colors.jans[Object.keys(tmpObjEdges).length];
              edgeObj.color = tmpObjEdges[coll];
            }
          } else if (config.edgeColorAttribute !== '') {
            var attr = edge[config.edgeColorAttribute];
            if (attr) {
              if (tmpObjEdges.hasOwnProperty(attr)) {
                edgeObj.color = tmpObjEdges[attr];
              } else {
                tmpObjEdges[attr] = colors.jans[Object.keys(tmpObjEdges).length];
                edgeObj.color = tmpObjEdges[attr];
              }
            }
          }
        }
        edgeObj.sortColor = edgeObj.color;
        edgesObj[edge._id] = edgeObj;
      });

      var nodeLabel;
      var nodeSize;
      var nodeObj;
      _.each(obj.vertices, function (node) {
        if (node !== null) {
          nodeNames[node._id] = true;

          if (config.nodeLabel) {
            if (config.nodeLabel.indexOf('.') > -1) {
              nodeLabel = getAttributeByKey(node, config.nodeLabel);
              if (nodeLabel === undefined || nodeLabel === '') {
                nodeLabel = node._id;
              }
            } else {
              if (node[config.nodeLabel] !== undefined) {
                if (typeof node[config.nodeLabel] === 'string') {
                  nodeLabel = node[config.nodeLabel];
                } else {
                  // in case we do not have a string here, we need to stringify it
                  // otherwise we might end up sending not displayable values.
                  nodeLabel = JSON.stringify(node[config.nodeLabel]);
                }
              } else {
                // in case the document does not have the nodeLabel in it, return fallback string
                nodeLabel = notFoundString;
              }
            }
          } else {
            nodeLabel = node._key;
          }

          if (config.nodeLabelByCollection === 'true') {
            nodeLabel += ' - ' + node._id.split('/')[0];
          }
          if (typeof nodeLabel === 'number') {
            nodeLabel = JSON.stringify(nodeLabel);
          }
          if (config.nodeSize && config.nodeSizeByEdges === 'false') {
            nodeSize = node[config.nodeSize];
          }

          nodeObj = {
            id: node._id,
            label: nodeLabel,
            size: nodeSize || 3,
            color: config.nodeColor || '#2ecc71',
            sortColor: undefined,
            x: Math.random(),
            y: Math.random()
          };

          if (config.nodeColorByCollection === 'true') {
            var coll = node._id.split('/')[0];
            if (tmpObjNodes.hasOwnProperty(coll)) {
              nodeObj.color = tmpObjNodes[coll];
            } else {
              tmpObjNodes[coll] = colors.jans[Object.keys(tmpObjNodes).length];
              nodeObj.color = tmpObjNodes[coll];
            }
          } else if (config.nodeColorAttribute !== '') {
            var attr = node[config.nodeColorAttribute];
            if (attr !== undefined && attr !== null) {
                nodeObj['nodeColorAttributeKey'] = config.nodeColorAttribute;
                nodeObj['nodeColorAttributeValue'] = attr;
                nodeObj.color = tmpObjNodes[attr];
            }
          }

          nodeObj.sortColor = nodeObj.color;
          nodesObj[node._id] = nodeObj;
        }
      });
    });

    _.each(nodesObj, function (node) {
      if (config.nodeSizeByEdges === 'true') {
        // + 10 visual adjustment sigma
        node.size = nodeEdgesCount[node.id] + 10;

        // if a node without edges is found, use def. size 10
        if (Number.isNaN(node.size)) {
          node.size = 10;
        }
      }
      nodesArr.push(node);
    });

    var nodeNamesArr = [];
    _.each(nodeNames, function (found, key) {
      nodeNamesArr.push(key);
    });

    // array format for sigma.js
    _.each(edgesObj, function (edge) {
      if (nodeNamesArr.indexOf(edge.source) > -1 && nodeNamesArr.indexOf(edge.target) > -1) {
        edgesArr.push(edge);
      }
    });
    toReturn = {
      nodes: nodesArr,
      edges: edgesArr,
      settings: {
        vertexCollections: vertexCollections,
        startVertex: startVertex,
        nodeColorAttribute: config.nodeColorAttribute
      }
    };
    if (isEnterprise) {
      if (graph.__isSmart) {
        toReturn.settings.isSmart = graph.__isSmart;
        toReturn.settings.smartGraphAttribute = graph.__smartGraphAttribute;
      }
    }
  }

  res.json(toReturn);
})
.summary('Return vertices and edges of a graph.')
.description(dd`
  This function returns vertices and edges for a specific graph.
`);

authRouter.get('/graphs-v2/:name', function (req, res) {
  var name = req.pathParams.name;
  var gm;
  if (isEnterprise) {
    gm = require('@arangodb/smart-graph');
  } else {
    gm = require('@arangodb/general-graph');
  }
  var colors = {
    default: [
      '#68BDF6',
      '#6DCE9E',
      '#FF756E',
      '#DE9BF9',
      '#FB95AF',
      '#FFD86E',
      '#A5ABB6'
    ],
    jans: ['rgba(166, 109, 161, 1)', 'rgba(64, 74, 83, 1)', 'rgba(90, 147, 189, 1)', 'rgba(153,63,0,1)', 'rgba(76,0,92,1)', 'rgba(25,25,25,1)', 'rgba(0,92,49,1)', 'rgba(43,206,72,1)', 'rgba(255,204,153,1)', 'rgba(128,128,128,1)', 'rgba(148,255,181,1)', 'rgba(143,124,0,1)', 'rgba(157,204,0,1)', 'rgba(194,0,136,1)', 'rgba(0,51,128,1)', 'rgba(255,164,5,1)', 'rgba(255,168,187,1)', 'rgba(66,102,0,1)', 'rgba(255,0,16,1)', 'rgba(94,241,242,1)', 'rgba(0,153,143,1)', 'rgba(224,255,102,1)', 'rgba(116,10,255,1)', 'rgba(153,0,0,1)', 'rgba(255,255,128,1)', 'rgba(255,255,0,1)', 'rgba(255,80,5,1)'],
    highContrast: [
      '#EACD3F',
      '#6F308A',
      '#DA6927',
      '#98CDE5',
      '#B81F34',
      '#C0BC82',
      '#7F7E80',
      '#61A547',
      '#60A446',
      '#D285B0',
      '#4477B3',
      '#DD8465',
      '#473896',
      '#E0A02F',
      '#8F2689',
      '#E7E655',
      '#7C1514',
      '#93AD3C',
      '#6D3312',
      '#D02C26',
      '#2A3415'
    ]
  };

  var graph;
  try {
    graph = gm._graph(name);
  } catch (e) {
    res.throw('bad request', e.errorMessage);
  }

  var edgesCollections = [];

  _.each(graph._edgeCollections(), function (edge) {
    edgesCollections.push({
      name: edge.name(),
      id: edge._id
    });
  });

  var graphVertexCollections = graph._vertexCollections();
  if (!graphVertexCollections || graphVertexCollections.length === 0) {
    res.throw('404 NOT FOUND', 'no vertex collections found for graph');
  }

  var vertexCollections = [];

  _.each(graphVertexCollections, function (vertex) {
    vertexCollections.push({
      name: vertex.name(),
      id: vertex._id
    });
  });

  var config;

  try {
    config = req.queryParams;
  } catch (e) {
    res.throw('bad request', e.message, {cause: e});
  }

  var getPseudoRandomStartVertex = function () {
    var vertexCandidates = [];
    for (var i = 0; i < graph._vertexCollections().length; i++) {
      var vertexCollection = graph._vertexCollections()[i];
      if (db._collection(vertexCollection.name()).count()) {
        let randomVertex = db._query(
          'FOR vertex IN @@vertexCollection SORT rand() LIMIT 1 RETURN vertex',
          {
            '@vertexCollection': vertexCollection.name()
          }
        ).next();
        
        vertexCandidates.push(randomVertex);
      }
    }
    
    if (vertexCandidates.length) {
      return _.sample(vertexCandidates);
    }
    return null;
  };

  var multipleIds;
  var startVertex; // will be "randomly" chosen if no start vertex is specified

  if (config.nodeStart) {
    if (config.nodeStart.indexOf(' ') > -1) {
      multipleIds = config.nodeStart.split(' ');
    } else {
      try {
        startVertex = db._document(config.nodeStart);
      } catch (e) {
        res.throw('bad request', e.message, {cause: e});
      }
      if (!startVertex) {
        startVertex = getPseudoRandomStartVertex();
      }
    }
  } else {
    startVertex = getPseudoRandomStartVertex();
  }

  var limit = 0;
  if (config.limit !== undefined) {
    if (config.limit.length > 0 && config.limit !== '0') {
      limit = parseInt(config.limit);
    }
  }

  var toReturn;
  // if we have multiple start nodes than startVertex === undefined
  if (startVertex === null) {
    toReturn = {
      empty: true,
      msg: 'Your graph is empty. We did not find a document in any available vertex collection.',
      settings: {
        vertexCollections: vertexCollections
      }
    };
    if (isEnterprise) {
      if (graph.__isSmart) {
        toReturn.settings.isSmart = graph.__isSmart;
        toReturn.settings.smartGraphAttribute = graph.__smartGraphAttribute;
      }
    }
  } else {
    var aqlQuery;
    var aqlQueries = [];

    let depth = parseInt(config.depth);

    if (config.query) {
      aqlQuery = config.query;
    } else {
      if (multipleIds) {
        /* TODO: uncomment after #75 fix
          aqlQuery =
            'FOR x IN ' + JSON.stringify(multipleIds) + ' ' +
            'FOR v, e, p IN 1..' + (depth || '2') + ' ANY x GRAPH "' + name + '"';
        */
        _.each(multipleIds, function (nodeid) {
          aqlQuery =
            'FOR v, e, p IN 0..' + (depth || '2') + ' ANY ' + JSON.stringify(nodeid) + ' GRAPH ' + JSON.stringify(name);
          if (limit !== 0) {
            aqlQuery += ' LIMIT ' + limit;
          }
          aqlQuery += ' RETURN p';
          aqlQueries.push(aqlQuery);
        });
      } else {
        aqlQuery =
          'FOR v, e, p IN 0..' + (depth || '2') + ' ANY ' + JSON.stringify(startVertex._id) + ' GRAPH ' + JSON.stringify(name);
        if (limit !== 0) {
          aqlQuery += ' LIMIT ' + limit;
        }
        aqlQuery += ' RETURN p';
      }
    }

    var getAttributeByKey = function (o, s) {
      s = s.replace(/\[(\w+)\]/g, '.$1');
      s = s.replace(/^\./, '');
      var a = s.split('.');
      for (var i = 0, n = a.length; i < n; ++i) {
        var k = a[i];
        if (k in o) {
          o = o[k];
        } else {
          return;
        }
      }
      return o;
    };

    var cursor;
    // get all nodes and edges, even if they are not connected
    // atm there is no server side function, so we need to get all docs
    // and edges of all related collections until the given limit is reached.
    if (config.mode === 'all') {
      var insertedEdges = 0;
      var insertedNodes = 0;
      var tmpEdges, tmpNodes;
      cursor = {
        json: [{
          vertices: [],
          edges: []
        }]
      };

      // get all nodes
      _.each(graph._vertexCollections(), function (node) {
        if (insertedNodes < limit || limit === 0) {
          tmpNodes = node.all().limit(limit).toArray();
          _.each(tmpNodes, function (n) {
            cursor.json[0].vertices.push(n);
          });
          insertedNodes += tmpNodes.length;
        }
      });
      // get all edges
      _.each(graph._edgeCollections(), function (edge) {
        if (insertedEdges < limit || limit === 0) {
          tmpEdges = edge.all().limit(limit).toArray();
          _.each(tmpEdges, function (e) {
            cursor.json[0].edges.push(e);
          });
          insertedEdges += tmpEdges.length;
        }
      });
    } else {
      // get all nodes and edges which are connected to the given start node
      try {
        if (aqlQueries.length === 0) {
          cursor = AQL_EXECUTE(aqlQuery);
        } else {
          var x;
          cursor = AQL_EXECUTE(aqlQueries[0]);
          for (var k = 1; k < aqlQueries.length; k++) {
            x = AQL_EXECUTE(aqlQueries[k]);
            _.each(x.json, function (val) {
              cursor.json.push(val);
            });
          }
        }
      } catch (e) {
        const error = new ArangoError({errorNum: e.errorNum, errorMessage: e.errorMessage});
        res.throw(actions.arangoErrorToHttpCode(e.errorNum), error);
      }
    }

    var nodesObj = {};
    var nodesArr = [];
    var edgesColorAttributes = [];
    if(config.edgesColorAttributes) {
      edgesColorAttributes = JSON.parse(config.edgesColorAttributes);
    }
    var nodesColorAttributes = [];
    if(config.nodesColorAttributes) {
      nodesColorAttributes = JSON.parse(config.nodesColorAttributes);
    }
    var nodesSizeValues = [];
    var nodesSizeMinMax = [];
    var connectionsCounts = [];
    var connectionsMinMax = [];

    var nodeNames = {};
    var edgesObj = {};
    var edgesArr = [];
    var nodeEdgesCount = {};
    var handledEdges = {};

    var tmpObjEdges = {};
    var tmpObjNodes = {};
    var nodeLabel;
    var nodeSize;
    var sizeCategory;
    var nodeObj;
    var notFoundString = "(attribute not found)";

    const truncate = (str, n) => {
      return (str.length > n) ? str.slice(0, n-1) + '...' : str;
    };
    
    const generateNodeObject = (node) => {
      nodeNames[node._id] = true;
      var label = "";
      var tooltipText = "";

      if (config.nodeLabel) {
        var nodeLabelArr = config.nodeLabel.trim().split(" ");
        // in case multiple node labels are given
        if (nodeLabelArr.length > 1) {
          _.each(nodeLabelArr, function (attr) {

            var attrVal = getAttributeByKey(node, attr);
            if (attrVal !== undefined) {
              if (typeof attrVal === 'string') {
                tooltipText += attr + ": " + attrVal + "\n";
              } else {
                // in case we do not have a string here, we need to stringify it
                // otherwise we might end up sending not displayable values.
                tooltipText += attr + ": " + JSON.stringify(attrVal) + "\n";
              }
            } else {
              label += attr + ": " + notFoundString;
              tooltipText += attr + ": " + notFoundString + "\n";
            }
            
          });
          // in case of multiple node labels just display the first one in the graph
          // and the others in the tooltip
          var firstAttrVal = getAttributeByKey(node, nodeLabelArr[0]);
          if (firstAttrVal !== undefined) {
            if (typeof firstAttrVal === 'string') {
              label = nodeLabelArr[0] + ": " + truncate(firstAttrVal, 16) + " ...";
            } else {
              label = nodeLabelArr[0] + ": " + truncate(JSON.stringify(firstAttrVal), 16) + " ...";
            }
          } else {
            label = nodeLabelArr[0] + ": " + notFoundString + " ...";
          }
        } else {
          // in case of single node attribute given
          var singleAttrVal = getAttributeByKey(node, nodeLabelArr[0]);
          if (singleAttrVal !== undefined) {
            if (typeof singleAttrVal === 'string') {
              label = nodeLabelArr[0] + ": " + truncate(singleAttrVal, 16);
              tooltipText = nodeLabelArr[0] + ": " + singleAttrVal;
            } else {
              label = nodeLabelArr[0] + ": " + truncate(JSON.stringify(singleAttrVal), 16);
              tooltipText = nodeLabelArr[0] + ": " + truncate(JSON.stringify(singleAttrVal), 16);
            }
          } else {
            label = nodeLabelArr[0] + ": " + notFoundString;
            tooltipText = nodeLabelArr[0] + ": " + notFoundString;
          }
        }
      } else {
        label = node._key || node._id;
        tooltipText = node._key || node._id;
      }

      if (config.nodeLabelByCollection === 'true') {
        label += ' - ' + node._id.split('/')[0];
      }
      if (typeof label === 'number') {
        label = JSON.stringify(label);
      }
      let sizeAttributeFound;
      if (config.nodeSize && config.nodeSizeByEdges === 'false') {
        nodeSize = 20;
        if (Number.isInteger(node[config.nodeSize])) {
          nodeSize = node[config.nodeSize];  
          sizeAttributeFound = true;
        } else {
          sizeAttributeFound = false;
        }
        
        sizeCategory = node[config.nodeSize] || '';
        nodesSizeValues.push(node[config.nodeSize]);
      }
      var calculatedNodeColor = '#48BB78';
      if (config.nodeColor !== undefined) {
        if(!config.nodeColor.startsWith('#')) {
          calculatedNodeColor = '#' + config.nodeColor;
        } else {
          calculatedNodeColor = config.nodeColor;
        }
      }
        
      nodeObj = {
        id: node._id,
        label: label,
        size: nodeSize || 20,
        value: nodeSize || 20,
        sizeCategory: sizeCategory || '',
        shape: "dot",
        color: calculatedNodeColor,
        font: {
          multi: 'html',
          strokeWidth: 2,
          strokeColor: '#ffffff',
          vadjust: -7
        },
        title: tooltipText,
        sizeAttributeFound
      };

      if (config.nodeColorByCollection === 'true') {
        var coll = node._id.split('/')[0];
        nodeObj.group = coll;
        nodeObj.color = "";
      } else if (config.nodeColorAttribute !== '') {
        var attr = node[config.nodeColorAttribute]
        if (attr) {
          nodeObj.group = JSON.stringify(attr);
          nodeObj.color = "";
          nodeObj.colorAttributeFound = true;
        } else {
          nodeObj.colorAttributeFound = false;
        }
      }

      nodeObj.sortColor = nodeObj.color;
      return nodeObj;
    }


    

    _.each(cursor.json, function (obj) {
      var edgeLabel = '';
      var edgeObj;
      _.each(obj.edges, function (edge) {
        if (edge._to && edge._from) {
          if (config.edgeLabel && config.edgeLabel.length > 0) {
            // configure edge labels

            if (config.edgeLabel.indexOf('.') > -1) {
              edgeLabel = getAttributeByKey(edge, config.edgeLabel);
              if (nodeLabel === undefined || nodeLabel === '') {
                edgeLabel = edgeLabel._id;
              }
            } else {
              if (edge[config.edgeLabel] !== undefined) {
                if (typeof edge[config.edgeLabel] === 'string') {
                  edgeLabel = edge[config.edgeLabel];
                } else {
                  // in case we do not have a string here, we need to stringify it
                  // otherwise we might end up sending not displayable values.
                  edgeLabel = JSON.stringify(edge[config.edgeLabel]);
                }
              } else {
                // in case the document does not have the edgeLabel in it, return fallback string
                edgeLabel = notFoundString;
              }
            }

            if (typeof edgeLabel !== 'string') {
              edgeLabel = JSON.stringify(edgeLabel);
            }
            if (config.edgeLabelByCollection === 'true') {
              edgeLabel += ' - ' + edge._id.split('/')[0];
            }
          } else {
            if (config.edgeLabelByCollection === 'true') {
              edgeLabel = edge._id.split('/')[0];
            }
          }

          if (config.nodeSizeByEdges === 'true') {
            if (handledEdges[edge._id] === undefined) {
              handledEdges[edge._id] = true;

              if (nodeEdgesCount[edge._from] === undefined) {
                nodeEdgesCount[edge._from] = 1;
              } else {
                nodeEdgesCount[edge._from] += 1;
              }

              if (nodeEdgesCount[edge._to] === undefined) {
                nodeEdgesCount[edge._to] = 1;
              } else {
                nodeEdgesCount[edge._to] += 1;
              }
            }
          }

          var calculatedEdgeColor = '#1D2A12';
          if (config.edgeColor !== undefined) {
            calculatedEdgeColor = '#' + config.edgeColor;
          }

          var edgestyle = {};
          if(config.edgeType !== undefined) {
            if(config.edgeType === 'dashed') {
              edgestyle = {
                dashes: [5, 5]
              };
            } else if(config.edgeType === 'dotted') {
              edgestyle = {
                dashes: [1, 3]
              };
            }
          }
          edgeObj = {
            id: edge._id,
            source: edge._from,
            from: edge._from,
            label: edgeLabel,
            target: edge._to,
            to: edge._to,
            color: calculatedEdgeColor,
            font: {
              strokeWidth: 2,
              strokeColor: '#ffffff',
              align: 'top'
            },
            length: 500,
            ...edgestyle
          };

          if (config.edgeEditable === 'true') {
            edgeObj.size = 1;
          } else {
            edgeObj.size = 1;
          }

          if (config.edgeColorByCollection === 'true') {
            var coll = edge._id.split('/')[0];

            if (tmpObjEdges.hasOwnProperty(coll)) {
              edgeObj.colorfromedges = tmpObjEdges[coll];
              edgeObj.color = tmpObjEdges[coll] || '#1D2A12'
            } else {
              tmpObjEdges[coll] = colors.jans[Object.keys(tmpObjEdges).length];
              edgeObj.color = tmpObjEdges[coll];
            }
          } else if (config.edgeColorAttribute !== '') {
            if(edge[config.edgeColorAttribute]) {
              edgeObj.colorCategory = edge[config.edgeColorAttribute] || '';
              const tempEdgeColor = Math.floor(Math.random()*16777215).toString(16).substring(1, 3) + Math.floor(Math.random()*16777215).toString(16).substring(1, 3) + Math.floor(Math.random()*16777215).toString(16).substring(1, 3);
              
              const edgeColorObj = {
                'name': edge[config.edgeColorAttribute] || '',
                'color': tempEdgeColor
              };

              const edgesColorAttributeIndex = edgesColorAttributes.findIndex(object => object.name === edgeColorObj.name);
              if (edgesColorAttributeIndex === -1) {
                edgesColorAttributes.push(edgeColorObj);
              }
            }

            var attr = edge[config.edgeColorAttribute];
            if (attr) {
              if (tmpObjEdges.hasOwnProperty(attr)) {
                edgeObj.color = '#' + edgesColorAttributes.find(obj => obj.name === attr).color;
                //edgeObj.style.fill = '#' + edgesColorAttributes.find(obj => obj.name === attr).color;
                edgeObj[config.edgeColorAttribute] = attr;
                edgeObj.attributeColor = tmpObjEdges[attr];
              } else {
                tmpObjEdges[attr] = colors.jans[Object.keys(tmpObjEdges).length];
                edgeObj.color = tmpObjEdges[attr];
                //edgeObj.style.fill = tmpObjEdges[attr] || '#ff0'; 
              }
              edgeObj.colorAttributeFound = true;
            } else {
              edgeObj.colorAttributeFound = false;
            }
          }
        }
        edgeObj.sortColor = edgeObj.color;
        edgesObj[edge._id] = edgeObj;
      });

      _.each(obj.vertices, function (node) {
        if (node !== null) {
          nodesObj[node._id] = generateNodeObject(node);
        }
      });
    });

    // In case our AQL query did not deliver any nodes, we will put the "startVertex" into the "nodes" list
    // as well (to be able to display at least the starting point of our graph)
    if (Object.keys(nodesObj).length === 0 && startVertex) {
      nodesObj[startVertex._id] = generateNodeObject(startVertex);
    }

    let nodeColorAttributeFound;
    let nodeSizeAttributeFound;
    
    _.each(nodesObj, function (node) {
      if (config.nodeSizeByEdges === 'true') {
        // + 10 visual adjustment sigma
        node.nodeEdgesCount = nodeEdgesCount[node.id];
        node.value = nodeEdgesCount[node.id];
        connectionsCounts.push(nodeEdgesCount[node.id]);

        // if a node without edges is found, use def. size 10
        if (Number.isNaN(node.size)) {
          node.size = 10;
        }
      }

      if(multipleIds !== undefined) {
        // mark every starting node
        if(multipleIds.includes(node.id)) {
          node.borderWidth = 4;
          node.shadow = {
            enabled: true,
            color: 'rgba(0,0,0,0.5)',
            size: 16,
            x: 0,
            y: 0
          };
          node.shapeProperties = {
            borderDashes: [10, 15]
          };
        }
      } else {
        // mark the one starting node
        if(node.id === startVertex._id) {
          node.borderWidth = 4;
          node.shadow = {
            enabled: true,
            color: 'rgba(0,0,0,0.5)',
            size: 16,
            x: 0,
            y: 0
          };
          node.shapeProperties = {
            borderDashes: [10, 15]
          };
        }
      }
      if (node.colorAttributeFound) {
          nodeColorAttributeFound = true;
      }
      if (node.sizeAttributeFound) {
          nodeSizeAttributeFound = true;
      }
      nodesArr.push(node);
    });

    var nodeNamesArr = [];
    _.each(nodeNames, function (found, key) {
      nodeNamesArr.push(key);
    });

    let edgeColorAttributeFound;
    // array format for sigma.js
    _.each(edgesObj, function (edge) {
      if (nodeNamesArr.indexOf(edge.source) > -1 && nodeNamesArr.indexOf(edge.target) > -1) {
        edgesArr.push(edge);
      }
      if(edge.colorAttributeFound) {
        edgeColorAttributeFound = true;
      }
    });

    const interactionOptions = {
      dragNodes: true,
      dragView: true,
      hideEdgesOnDrag: false,
      hideNodesOnDrag: false,
      hover: true,
      hoverConnectedEdges: false,
      keyboard: {
        enabled: false,
        speed: {
          x: 3,
          y: 3,
          zoom: 0.02
        },
        bindToWindow: false
      },
      multiselect: false,
      navigationButtons: false,
      selectable: true,
      selectConnectedEdges: false,
      tooltipDelay: 300,
      zoomSpeed: 0.25,
      zoomView: true
    };

    const barnesHutOptions = {
      interaction: interactionOptions,
      layout: {
          randomSeed: 0,
          hierarchical: false
      },
      edges: {
        smooth: { type:"dynamic" },
        arrows: {
          to: {
            enabled: (config.edgeDirection === "true"),
            type: "arrow",
            scaleFactor: 0.5
          },
        },
      },
      physics: {
          barnesHut: {
              gravitationalConstant: -2250,
              centralGravity: 0.4,
              springLength: 76,
              damping: 0.095
          },
          solver: "barnesHut"
      }
    };

    const hierarchicalOptions = {
      interaction: interactionOptions,
        layout: {
          randomSeed: 0,
          hierarchical: {
            levelSeparation: 150,
            nodeSpacing: 300,
            direction: "UD"
          },
        },
        edges: {
          smooth: { type:"dynamic" },
          arrows: {
            to: {
              enabled: (config.edgeDirection === "true"),
              type: "arrow",
              scaleFactor: 0.5
            },
          },
        },
        physics: {
          barnesHut: {
            gravitationalConstant: -2250,
            centralGravity: 0.4,
            damping: 0.095
          },
          solver: "barnesHut"
        }
    };

    const forceAtlas2BasedOptions = {
      interaction: interactionOptions,
          layout: {
              randomSeed: 0,
              hierarchical: false
          },
          edges: {
            smooth: { type:"dynamic" },
            arrows: {
              to: {
                enabled: (config.edgeDirection === "true"),
                type: "arrow",
                scaleFactor: 0.5
              },
            },
          },
          physics: {
            forceAtlas2Based: {
              springLength: 10,
              springConstant: 1.5,
              gravitationalConstant: -500
            },
            minVelocity: 0.75,
            solver: "forceAtlas2Based"
          }
      };

      let layoutObject = hierarchicalOptions;
      
      switch (config.layout) {
        case 'forceAtlas2':
          layoutObject = forceAtlas2BasedOptions;
          break;
        case 'barnesHut':
          layoutObject = barnesHutOptions;
          break;
        case 'hierarchical':
          layoutObject = hierarchicalOptions;
          break;
        default:
          layoutObject = barnesHutOptions;
      }
    const nodeSizeAttributeMessage = 
      !nodeSizeAttributeFound && config.nodeSize
        ? "Invalid attribute specified"
        : "";
    const nodeColorAttributeMessage = 
      !nodeColorAttributeFound && config.nodeColorAttribute
        ? "Invalid attribute specified"
        : "";
    const edgeColorAttributeMessage = 
      !edgeColorAttributeFound && config.edgeColorAttribute
        ? "Invalid attribute specified"
        : "";
    toReturn = {
      nodes: nodesArr,
      edges: edgesArr,
      settings: {
        nodeColorAttributeMessage,
        nodeSizeAttributeMessage,
        edgeColorAttributeMessage,
        configlayout: config.layout,
        layout: layoutObject,
        vertexCollections: vertexCollections,
        edgesCollections: edgesCollections,
        startVertex: startVertex,
        nodesColorAttributes: nodesColorAttributes,
        edgesColorAttributes: edgesColorAttributes,
        nodesSizeValues: nodesSizeValues,
        nodesSizeMinMax: [Math.min(...nodesSizeValues), Math.max(...nodesSizeValues)],
        connectionsCounts: connectionsCounts,
        connectionsMinMax: [Math.min(...connectionsCounts), Math.max(...connectionsCounts)]
      }
    };
    if (isEnterprise) {
      if (graph.__isSmart) {
        toReturn.settings.isSmart = graph.__isSmart;
        toReturn.settings.smartGraphAttribute = graph.__smartGraphAttribute;
      }
    }
  }

  res.json(toReturn);
})
.summary('Return vertices and edges of a graph.')
.description(dd`
  This function returns vertices and edges for a specific graph.
`);
