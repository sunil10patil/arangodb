(function () {
  'use strict';

  window.ArangoDocuments = window.PaginatedCollection.extend({
    collectionID: 1,

    filters: [],
    checkCursorTimer: undefined,

    MAX_SORT: 12000,

    lastQuery: {},
    type: 'document',
    sortAttribute: '',
    smartJoinAttribute: null,
    smartGraphAttribute: null,
    isSmart: false,
    isEnterpriseEdge: false,
    distributeShardsLike: '',

    url: arangoHelper.databaseUrl('/_api/documents'),
    model: window.arangoDocumentModel,

    setSmartJoinAttribute: function (stringValue) {
      this.smartJoinAttribute = stringValue;
    },

    getSmartJoinAttribute: function () {
      return this.smartJoinAttribute;
    },

    setSmartGraphAttribute: function (stringValue) {
      this.smartGraphAttribute = stringValue;
    },

    getSmartGraphAttribute: function () {
      return this.smartGraphAttribute;
    },

    setIsSmart: function (booleanValue) {
      this.isSmart = booleanValue;
    },

    getIsSmart: function () {
      return this.isSmart;
    },

    setIsEnterpriseEdge: function (booleanValue) {
      this.isEnterpriseEdge = booleanValue;
    },

    getIsEnterpriseEdge: function () {
      return this.isEnterpriseEdge;
    },

    setDistributeShardsLike: function (stringValue) {
      this.distributeShardsLike = stringValue;
    },

    getDistributeShardsLike: function () {
      return this.distributeShardsLike;
    },

    loadCollectionConfig: function (callback) {
      var self = this;
      $.ajax({
        cache: false,
        type: 'GET',
        url: arangoHelper.databaseUrl('/_api/collection/' + encodeURIComponent(this.collectionID) + '/count'),
        contentType: 'application/json',
        processData: false,
        success: function (data) {
          if (data.count || data.count === 0) {
            self.setTotal(data.count);
          }

          if (data.smartJoinAttribute) {
            self.setSmartJoinAttribute(data.smartJoinAttribute);
          } else {
            self.setSmartJoinAttribute(null);
          }

          if (data.smartGraphAttribute) {
            self.setSmartGraphAttribute(data.smartGraphAttribute);
          } else {
            self.setSmartGraphAttribute(null);
          }

          if (data.isSmart) {
            self.setIsSmart(true);
          }

          if (data.distributeShardsLike) {
            self.setDistributeShardsLike(data.distributeShardsLike);
          }

          if (data.isSmart && data.type === 3) {
            // In case we do have a Smart Edge Collection
            // We are either in the context of a SmartGraph, or we are in the context of an EnterpriseGraph
            // To be able to distinguish between them, we need to take a look at rather the target distributeShardsLike
            // has a smartGraphAttribute set in its collection properties.
            // We do have then two states:
            // (1) If it is set, it is an edge in a SmartGraph environment
            // (2) If it is NOT set, it is an edge in a EnterpriseGraph environment

            // Unfortunately, as this is not a direct collection property, we need to make an additional call to
            // identify in which case we are, it must be either (1) or (2).

            const distributeShardsLikeCollectionId = self.getDistributeShardsLike();
            if (distributeShardsLikeCollectionId.length === 0) {
              // Means, it is empty and therefore not set properly before, as it has to be set in Smart context,
              // for both EnterpriseGraph and SmartGraph cases.
              // Means, we do have invalid state and cannot continue.
              callback(true);
            }

            $.ajax({
              cache: false,
              type: 'GET',
              url: arangoHelper.databaseUrl('/_api/collection/' + encodeURIComponent(distributeShardsLikeCollectionId) + '/properties'),
              contentType: 'application/json',
              processData: false,
              success: function (distributeDataCheck) {
                if (!distributeDataCheck.smartGraphAttribute) {
                  // We hit case (2), we must be in the context of an EnterpriseGraph
                  self.setIsEnterpriseEdge(true);
                }
                callback(false);
              }, error: function () {
                callback(true);
              }
            });
          } else {
            callback(false);
          }
        },
        error: function () {
          callback(true);
        }
      });
    },

    setCollection: function (id, page) {
      var callback = function (error) {
        if (error) {
          arangoHelper.arangoError('Documents', 'Could not fetch documents count');
        }
      };
      this.resetFilter();
      this.collectionID = id;
      if (page) {
        this.setPage(page);
      } else {
        this.setPage(1);
      }
      this.loadCollectionConfig(callback);
    },

    setSort: function (key) {
      this.sortAttribute = key;
    },

    getSort: function () {
      return this.sortAttribute;
    },

    addFilter: function (attr, op, val) {
      this.filters.push({
        attr: attr,
        op: op,
        val: val
      });
    },

    setFiltersForQuery: function (bindVars) {
      if (this.filters.length === 0) {
        return '';
      }
      var parts = _.map(this.filters, function (f, i) {
        var res = 'x.@attr' + i + ' ' + f.op + ' @param' + i;

        if (f.op === 'LIKE') {
          bindVars['param' + i] = '%' + f.val + '%';
        } else if (f.op === 'IN' || f.op === 'NOT IN ') {
          if (f.val.indexOf(',') !== -1) {
            bindVars['param' + i] = f.val.split(',').map(function (v) { return v.replace(/(^ +| +$)/g, ''); });
          } else {
            bindVars['param' + i] = [ f.val ];
          }
        } else {
          bindVars['param' + i] = f.val;
        }

        if (f.attr.indexOf('.') !== -1) {
          bindVars['attr' + i] = f.attr.split('.');
        } else {
          bindVars['attr' + i] = f.attr;
        }

        return res;
      });
      return ' FILTER ' + parts.join(' && ');
    },

    setPagesize: function (size) {
      this.setPageSize(size);
    },

    resetFilter: function () {
      this.filters = [];
    },

    moveDocument: function (key, fromCollection, toCollection, callback) {
      var queryObj1 = {
        query: 'FOR x IN @@fromCollection FILTER x._key == @filterid INSERT x IN @@toCollection',
        bindVars: {
          '@fromCollection': fromCollection,
          '@toCollection': toCollection,
          'filterid': key
        }
      };

      var queryObj2 = {
        query: 'FOR x in @@collection FILTER x._key == @filterid REMOVE x IN @@collection',
        bindVars: {
          '@collection': fromCollection,
          'filterid': key
        }
      };

      window.progressView.show();
      // first insert docs in toCollection
      $.ajax({
        cache: false,
        type: 'POST',
        url: arangoHelper.databaseUrl('/_api/cursor'),
        data: JSON.stringify(queryObj1),
        contentType: 'application/json',
        success: function () {
          // if successful remove unwanted docs
          $.ajax({
            cache: false,
            type: 'POST',
            url: arangoHelper.databaseUrl('/_api/cursor'),
            data: JSON.stringify(queryObj2),
            contentType: 'application/json',
            success: function () {
              var error = false;
              if (callback) {
                callback(error);
              }
              window.progressView.hide();
            },
            error: function () {
              var error = true;
              if (callback) {
                callback(error);
              }
              window.progressView.hide();
              arangoHelper.arangoError(
                'Document error', 'Documents inserted, but could not be removed.'
              );
            }
          });
        },
        error: function () {
          window.progressView.hide();
          arangoHelper.arangoError('Document error', 'Could not move selected documents.');
        }
      });
    },

    getDocuments: function (callback) {
      var self = this;
      var query;
      var bindVars;
      var queryObj;

      var pageSize = this.getPageSize();
      if (pageSize === 'all') {
        pageSize = this.MAX_SORT + 38000; // will result in 50k docs
      }

      bindVars = {
        '@collection': this.collectionID
      };

      // fetch just the first 25 attributes of the document
      // this number is arbitrary, but may reduce HTTP traffic a bit
      query = 'FOR x IN @@collection LET att = APPEND(SLICE(ATTRIBUTES(x), 0, 25), "_key", true)';
      query += this.setFiltersForQuery(bindVars);
      // Sort result, only useful for a small number of docs
      if (this.getTotal() < this.MAX_SORT && this.getSort() !== '') {
        query += ' SORT x.@sortAttribute';
        bindVars.sortAttribute = this.getSort();
      }

      if (bindVars.count !== 'all') {
        query += ' LIMIT @offset, @count';
        bindVars.offset = this.getOffset();
        bindVars.count = pageSize;
      }
      query += ' RETURN KEEP(x, att)';

      queryObj = {
        query: query,
        bindVars: bindVars,
        batchSize: pageSize
      };

      if (this.filters.length > 0) {
        queryObj.options = {
          fullCount: true
        };
      }

      var checkCursorStatus = function (jobid) {
        $.ajax({
          cache: false,
          type: 'PUT',
          url: arangoHelper.databaseUrl('/_api/job/' + encodeURIComponent(jobid)),
          contentType: 'application/json',
          success: function (data, textStatus, xhr) {
            if (xhr.status === 201) {
              window.progressView.toShow = false;
              self.clearDocuments();
              if (data.extra && data.extra.stats && data.extra.stats.fullCount !== undefined) {
                self.setTotal(data.extra.stats.fullCount);
              }
              _.each(data.result, function (v) {
                self.add({
                  'id': v._id,
                  'rev': v._rev,
                  'key': v._key,
                  'content': v
                });
              });
              self.lastQuery = queryObj;

              callback(false, data);
            } else if (xhr.status === 204) {
              self.checkCursorTimer = window.setTimeout(function () {
                checkCursorStatus(jobid);
              }, 500);
            }
          },
          error: function (data) {
            callback(false, data);
          }
        });
      };

      $.ajax({
        cache: false,
        type: 'POST',
        url: arangoHelper.databaseUrl('/_api/cursor'),
        data: JSON.stringify(queryObj),
        headers: {
          'x-arango-async': 'store'
        },
        contentType: 'application/json',
        success: function (data, textStatus, xhr) {
          if (xhr.getResponseHeader('x-arango-async-id')) {
            var jobid = xhr.getResponseHeader('x-arango-async-id');

            var cancelRunningCursor = function () {
              $.ajax({
                url: arangoHelper.databaseUrl('/_api/job/' + encodeURIComponent(jobid) + '/cancel'),
                type: 'PUT',
                success: function () {
                  window.clearTimeout(self.checkCursorTimer);
                  arangoHelper.arangoNotification('Documents', 'Canceled operation.');
                  $('.dataTables_empty').text('Canceled.');
                  window.progressView.hide();
                }
              });
            };

            window.progressView.showWithDelay(1000, 'Fetching documents...', cancelRunningCursor);

            checkCursorStatus(jobid);
          } else {
            callback(true, data);
          }
        },
        error: function (data) {
          callback(false, data);
        }
      });
    },

    clearDocuments: function () {
      this.reset();
    },

    buildDownloadDocumentQuery: function () {
      var query, queryObj, bindVars;

      bindVars = {
        '@collection': this.collectionID
      };

      query = 'FOR x in @@collection';
      query += this.setFiltersForQuery(bindVars);
      // Sort result, only useful for a small number of docs
      if (this.getTotal() < this.MAX_SORT && this.getSort() !== '') {
        query += ' SORT x.@sortAttribute';
        bindVars.sortAttribute = this.getSort();
      }

      query += ' RETURN x';

      queryObj = {
        query: query,
        bindVars: bindVars
      };

      return queryObj;
    },

    uploadDocuments: function (file, callback) {
      var analyzeResponse = function (data) {
        if (data.hasOwnProperty('error')) {
          delete data.error;
        }

        if (data.errors > 0) {
          callback(true, 'Info: ' + JSON.stringify(data));
        } else {
          callback(false, 'Info: ' + JSON.stringify(data));
        }
      };

      $.ajax({
        type: 'POST',
        url: arangoHelper.databaseUrl('/_api/import?type=auto&details=true&collection=' +
          encodeURIComponent(this.collectionID) +
          '&createCollection=false'),
        data: file,
        processData: false,
        contentType: 'json',
        dataType: 'json',
        complete: function (xhr) {
          if (xhr.responseJSON) {
            analyzeResponse(xhr.responseJSON);
          }
        },
        error: function (msg) {
          callback(true, msg.responseJSON.errorMessage);
        }
      });
    }
  });
}());
