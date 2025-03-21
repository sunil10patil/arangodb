
@startDocuBlock get_api_index

@RESTHEADER{GET /_api/index, List all indexes of a collection, listIndexes}

@RESTQUERYPARAMETERS

@RESTQUERYPARAM{collection,string,required}
The collection name.

@RESTQUERYPARAM{withStats,boolean,optional}
Whether to include figures and estimates in the result.

@RESTQUERYPARAM{withHidden,boolean,optional}
Whether to include hidden indexes in the result.

@RESTDESCRIPTION
Returns an object with an `indexes` attribute containing an array of all
index descriptions for the given collection. The same information is also
available in the `identifiers` attribute as an object with the index identifiers
as object keys.

@RESTRETURNCODES

@RESTRETURNCODE{200}
returns a JSON object containing a list of indexes on that collection.

@EXAMPLES

Return information about all indexes

@EXAMPLE_ARANGOSH_RUN{RestIndexAllIndexes}
    var cn = "products";
    db._drop(cn);
    db._create(cn);
    db[cn].ensureIndex({ type: "persistent", fields: ["name"] });
    db[cn].ensureIndex({ type: "persistent", fields: ["price"], sparse: true });

    var url = "/_api/index?collection=" + cn;

    var response = logCurlRequest('GET', url);

    assert(response.code === 200);

    logJsonResponse(response);
  ~ db._drop(cn);
@END_EXAMPLE_ARANGOSH_RUN

@endDocuBlock
