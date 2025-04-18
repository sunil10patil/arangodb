
@startDocuBlock put_api_collection_collection_load

@RESTHEADER{PUT /_api/collection/{collection-name}/load, Load a collection, loadCollection}

@HINTS
{% hint 'warning' %}
The load function is deprecated from version 3.8.0 onwards and is a no-op 
from version 3.9.0 onwards. It should no longer be used, as it may be removed
in a future version of ArangoDB.
{% endhint %}

{% hint 'warning' %}
Accessing collections by their numeric ID is deprecated from version 3.4.0 on.
You should reference them via their names instead.
{% endhint %}

@RESTURLPARAMETERS

@RESTURLPARAM{collection-name,string,required}
The name of the collection.

@RESTDESCRIPTION
Since ArangoDB version 3.9.0 this API does nothing. Previously it used to
load a collection into memory. 

The request body object might optionally contain the following attribute:

- `count`: If set, this controls whether the return value should include
  the number of documents in the collection. Setting `count` to
  `false` may speed up loading a collection. The default value for
  `count` is `true`.

A call to this API returns an object with the following attributes for
compatibility reasons:

- `id`: The identifier of the collection.

- `name`: The name of the collection.

- `count`: The number of documents inside the collection. This is only
  returned if the `count` input parameters is set to `true` or has
  not been specified.

- `status`: The status of the collection as number.

- `type`: The collection type. Valid types are:
  - 2: document collection
  - 3: edge collection

- `isSystem`: If `true` then the collection is a system collection.

@RESTRETURNCODES

@RESTRETURNCODE{400}
If the `collection-name` is missing, then a *HTTP 400* is
returned.

@RESTRETURNCODE{404}
If the `collection-name` is unknown, then a *HTTP 404*
is returned.

@EXAMPLES

@EXAMPLE_ARANGOSH_RUN{RestCollectionIdentifierLoad}
    var cn = "products";
    db._drop(cn);
    var coll = db._create(cn, { waitForSync: true });
    var url = "/_api/collection/"+ coll.name() + "/load";

    var response = logCurlRequest('PUT', url, '');

    assert(response.code === 200);

    logJsonResponse(response);
    db._drop(cn);
@END_EXAMPLE_ARANGOSH_RUN
@endDocuBlock
