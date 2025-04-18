
@startDocuBlock put_api_query_cache_properties

@RESTHEADER{PUT /_api/query-cache/properties, Set the AQL query results cache configuration, setQueryCacheProperties}

@RESTDESCRIPTION
Adjusts the global properties for the AQL query results cache.

After the properties have been changed, the current set of properties will
be returned in the HTTP response.

Note: changing the properties may invalidate all results in the cache.

The properties need to be passed in the `properties` attribute in the body
of the HTTP request. `properties` needs to be a JSON object with the following
properties:

@RESTBODYPARAM{mode,string,optional,string}
 the mode the AQL query cache should operate in. Possible values are `off`, `on`, or `demand`.

@RESTBODYPARAM{maxResults,integer,optional,int64}
the maximum number of query results that will be stored per database-specific cache.

@RESTBODYPARAM{maxResultsSize,integer,optional,int64}
the maximum cumulated size of query results that will be stored per database-specific cache.

@RESTBODYPARAM{maxEntrySize,integer,optional,int64}
the maximum individual size of query results that will be stored per database-specific cache.

@RESTBODYPARAM{includeSystem,boolean,optional,}
whether or not to store results of queries that involve system collections.

@RESTRETURNCODES

@RESTRETURNCODE{200}
Is returned if the properties were changed successfully.

@RESTRETURNCODE{400}
The server will respond with *HTTP 400* in case of a malformed request,

@endDocuBlock
