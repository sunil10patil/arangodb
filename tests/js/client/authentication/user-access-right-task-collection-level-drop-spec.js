/* jshint globalstrict:true, strict:true, maxlen: 5000 */
/* global describe, before, after, it */

// //////////////////////////////////////////////////////////////////////////////
// / @brief tests for user access rights
// /
// / @file
// /
// / DISCLAIMER
// /
// / Copyright 2017 ArangoDB GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License");
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
// / Copyright holder is ArangoDB GmbH, Cologne, Germany
// /
// / @author Michael Hackstein
// / @author Mark Vollmary
// / @author Copyright 2017, ArangoDB GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

'use strict';

const expect = require('chai').expect;
const users = require('@arangodb/users');
const helper = require('@arangodb/testutils/user-helper');
const tasks = require('@arangodb/tasks');
const pu = require('@arangodb/testutils/process-utils');
const download = require('internal').download;
const dbName = helper.dbName;
const colName = helper.colName;
const rightLevels = helper.rightLevels;
const errors = require('@arangodb').errors;
const keySpaceId = 'task_collection_level_drop_keyspace';

const userSet = helper.userSet;
const systemLevel = helper.systemLevel;
const dbLevel = helper.dbLevel;
const colLevel = helper.colLevel;

const arango = require('internal').arango;
const db = require('internal').db;
for (let l of rightLevels) {
  systemLevel[l] = new Set();
  dbLevel[l] = new Set();
  colLevel[l] = new Set();
}

const wait = (keySpaceId, key) => {
  for (let i = 0; i < 200; i++) {
    if (getKey(keySpaceId, key)) break;
    require('internal').wait(0.1);
  }
};

const createKeySpace = (keySpaceId) => {
  return executeJS(`return global.KEYSPACE_CREATE('${keySpaceId}', 128, true);`).body === 'true';
};

const dropKeySpace = (keySpaceId) => {
  executeJS(`global.KEYSPACE_DROP('${keySpaceId}');`);
};

const getKey = (keySpaceId, key) => {
  return executeJS(`return global.KEY_GET('${keySpaceId}', '${key}');`).body === 'true';
};

const setKey = (keySpaceId, name) => {
  return executeJS(`global.KEY_SET('${keySpaceId}', '${name}', false);`);
};

const executeJS = (code) => {
  let httpOptions = pu.makeAuthorizationHeaders({
    username: 'root',
    password: ''
  }, {});
  httpOptions.method = 'POST';
  httpOptions.timeout = 1800;
  httpOptions.returnBodyOnError = true;
  return download(arango.getEndpoint().replace('tcp', 'http') + `/_db/${dbName}/_admin/execute?returnAsJSON=true`,
    code,
    httpOptions);
};

helper.switchUser('root', '_system');
helper.removeAllUsers();
helper.generateAllUsers();

describe('User Rights Management', () => {
  it('should check if all users are created', () => {
    helper.switchUser('root', '_system');
    expect(userSet.size).to.be.greaterThan(0); 
    expect(userSet.size).to.equal(helper.userCount);
    for (let name of userSet) {
      expect(users.document(name), `Could not find user: ${name}`).to.not.be.undefined;
    }
  });

  it('should test rights for', () => {
    expect(userSet.size).to.be.greaterThan(0); 
    for (let name of userSet) {
      let canUse = false;
      try {
        helper.switchUser(name, dbName);
        canUse = true;
      } catch (e) {
        canUse = false;
      }

      if (canUse) {
        describe(`user ${name}`, () => {
          before(() => {
            helper.switchUser(name, dbName);
            expect(createKeySpace(keySpaceId)).to.equal(true, 'keySpace creation failed!');
          });

          after(() => {
            dropKeySpace(keySpaceId);
          });

          describe('update on collection level', () => {
            const rootTestCollection = (switchBack = true) => {
              helper.switchUser('root', dbName);
              let col = db._collection(colName);
              if (switchBack) {
                helper.switchUser(name, dbName);
              }
              return col !== null;
            };

            const rootPrepareCollection = () => {
              if (rootTestCollection(false)) {
                db._collection(colName).truncate({ compact: false });
                db._collection(colName).save({_key: '123'});
                db._collection(colName).save({_key: '456'});
              }
              helper.switchUser(name, dbName);
            };

            describe('remove a document', () => {
              before(() => {
                db._useDatabase(dbName);
                rootPrepareCollection();
              });

              it('by key', () => {
                expect(rootTestCollection()).to.equal(true, 'Precondition failed, the collection does not exist.');
                setKey(keySpaceId, name);
                const taskId = 'task_collection_level_drop_by_key' + name;
                const task = {
                  id: taskId,
                  name: taskId,
                  command: `(function (params) {
                    try {
                      const db = require('@arangodb').db;
                      db._collection('${colName}').remove('123');
                      global.KEY_SET('${keySpaceId}', '${name}_status', true);
                    } catch (e) {
                      global.KEY_SET('${keySpaceId}', '${name}_status', false);
                    } finally {
                      global.KEY_SET('${keySpaceId}', '${name}', true);
                    }
                  })(params);`
                };
                if (dbLevel['rw'].has(name)) {
                  if ((dbLevel['rw'].has(name) || dbLevel['ro'].has(name)) &&
                    colLevel['rw'].has(name)) {
                    let col = db._collection(colName);
                    expect(col.document('123')._key).to.equal('123', 'Precondition failed, document does not exist.');
                    tasks.register(task);
                    wait(keySpaceId, name);
                    expect(getKey(keySpaceId, `${name}_status`)).to.equal(true, `${name} could not drop the document with sufficient rights`);
                    try {
                      col.document('123');
                      expect(true).to.equal(false, `${name} could not drop the document with sufficient rights`);
                    } catch (e) {}
                  } else {
                    let hasReadAccess = ((dbLevel['rw'].has(name) || dbLevel['ro'].has(name)) &&
                      (colLevel['rw'].has(name) || colLevel['ro'].has(name)));
                    if (hasReadAccess) {
                      let col = db._collection(colName);
                      expect(col.document('123')._key).to.equal('123', 'Precondition failed, document does not exist.');
                    }
                    tasks.register(task);
                    wait(keySpaceId, name);
                    expect(getKey(keySpaceId, `${name}_status`)).to.not.equal(true, `${name} managed to remove the document with insufficient rights`);
                    if (hasReadAccess) {
                      let col = db._collection(colName);
                      try {
                        expect(col.document('123')._key).to.equal('123', `${name} managed to remove the document with insufficient rights`);
                      } catch (e) {}
                    }
                  }
                } else {
                  try {
                    tasks.register(task);
                    expect(false).to.equal(true, `${name} managed to register a task with insufficient rights`);
                  } catch (e) {
                    expect(e.errorNum).to.equal(errors.ERROR_FORBIDDEN.code);
                  }
                }
              });

              it('by aql', () => {
                expect(rootTestCollection()).to.equal(true, 'Precondition failed, the collection does not exist');
                let q = `REMOVE '456' IN ${colName} RETURN OLD`;
                setKey(keySpaceId, name);
                const taskId = 'task_collection_level_drop_by_key' + name;
                const task = {
                  id: taskId,
                  name: taskId,
                  command: `(function (params) {
                    try {
                      const db = require('@arangodb').db;
                      db._query("${q}");
                      global.KEY_SET('${keySpaceId}', '${name}_status', true);
                    } catch (e) {
                      global.KEY_SET('${keySpaceId}', '${name}_status', false);
                    }finally {
                      global.KEY_SET('${keySpaceId}', '${name}', true);
                    }
                  })(params);`
                };
                if (dbLevel['rw'].has(name)) {
                  if ((dbLevel['rw'].has(name) || dbLevel['ro'].has(name)) &&
                    colLevel['rw'].has(name)) {
                    let col = db._collection(colName);
                    tasks.register(task);
                    wait(keySpaceId, name);
                    expect(getKey(keySpaceId, `${name}_status`)).to.equal(true, `${name} could not drop the document with sufficient rights`);
                    try {
                      col.document('456');
                      expect(true).to.equal(false, 'Document still in collection after remove');
                    } catch (e) {}
                  } else {
                    let hasReadAccess = ((dbLevel['rw'].has(name) || dbLevel['ro'].has(name)) &&
                      (colLevel['rw'].has(name) || colLevel['ro'].has(name)));
                    tasks.register(task);
                    wait(keySpaceId, name);
                    expect(getKey(keySpaceId, `${name}_status`)).to.not.equal(true, `${name} managed to remove the document with insufficient rights`);
                    if (hasReadAccess) {
                      let col = db._collection(colName);
                      try {
                        expect(col.document('456')._key).to.equal('456', `${name} managed to remove the document with insufficient rights`);
                      } catch (e) {}
                    }
                  }
                } else {
                  try {
                    tasks.register(task);
                    expect(false).to.equal(true, `${name} managed to register a task with insufficient rights`);
                  } catch (e) {
                    expect(e.errorNum).to.equal(errors.ERROR_FORBIDDEN.code);
                  }
                }
              });
            });
          });
        });
      }
    }
  });
});
