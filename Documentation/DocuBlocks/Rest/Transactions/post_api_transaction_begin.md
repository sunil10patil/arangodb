
@startDocuBlock post_api_transaction_begin

@RESTHEADER{POST /_api/transaction/begin, Begin a Stream Transaction, beginStreamTransaction}

@RESTHEADERPARAMETERS

@RESTHEADERPARAM{x-arango-allow-dirty-read,boolean,optional}
Set this header to `true` to allow the Coordinator to ask any shard replica for
the data, not only the shard leader. This may result in "dirty reads".

This header decides about dirty reads for the entire transaction. Individual
read operations, that are performed as part of the transaction, cannot override it.

@RESTBODYPARAM{collections,string,required,string}
`collections` must be a JSON object that can have one or all sub-attributes
`read`, `write` or `exclusive`, each being an array of collection names or a
single collection name as string. Collections that will be written to in the
transaction must be declared with the `write` or `exclusive` attribute or it
will fail, whereas non-declared collections from which is solely read will be
added lazily.

@RESTBODYPARAM{waitForSync,boolean,optional,}
an optional boolean flag that, if set, will force the
transaction to write all data to disk before returning.

@RESTBODYPARAM{allowImplicit,boolean,optional,}
Allow reading from undeclared collections. 

@RESTBODYPARAM{lockTimeout,integer,optional,int64}
an optional numeric value that can be used to set a
timeout in seconds for waiting on collection locks. This option is only
meaningful when using exclusive locks. If not specified, a default
value will be used. Setting `lockTimeout` to `0` will make ArangoDB
not time out waiting for a lock.

@RESTBODYPARAM{maxTransactionSize,integer,optional,int64}
Transaction size limit in bytes.

@RESTDESCRIPTION
Begin a Stream Transaction that allows clients to call selected APIs over a
short period of time, referencing the transaction ID, and have the server
execute the operations transactionally.

Committing or aborting a running transaction must be done by the client.
It is bad practice to not commit or abort a transaction once you are done
using it. It forces the server to keep resources and collection locks 
until the entire transaction times out.

The transaction description must be passed in the body of the POST request.
If the transaction can be started on the server, *HTTP 201* will be returned.

For successfully started transactions, the returned JSON object has the
following properties:

- `error`: boolean flag to indicate if an error occurred (`false`
  in this case)

- `code`: the HTTP status code

- `result`: result containing
    - `id`: the identifier of the transaction
    - `status`: containing the string 'running'

If the transaction specification is either missing or malformed, the server
will respond with *HTTP 400* or *HTTP 404*.

The body of the response will then contain a JSON object with additional error
details. The object has the following attributes:

- `error`: boolean flag to indicate that an error occurred (`true` in this case)

- `code`: the HTTP status code

- `errorNum`: the server error number

- `errorMessage`: a descriptive error message

@RESTRETURNCODES

@RESTRETURNCODE{201}
If the transaction is running on the server,
*HTTP 201* will be returned.

@RESTRETURNCODE{400}
If the transaction specification is either missing or malformed, the server
will respond with *HTTP 400*.

@RESTRETURNCODE{404}
If the transaction specification contains an unknown collection, the server
will respond with *HTTP 404*.

@EXAMPLES

Executing a transaction on a single collection

@EXAMPLE_ARANGOSH_RUN{RestTransactionBeginSingle}
    const cn = "products";
    db._drop(cn);
    db._create(cn);
    let url = "/_api/transaction/begin";
    let body = {
      collections: {
        write : cn
      },
    };

    let response = logCurlRequest('POST', url, body);
    assert(response.code === 201);
    logJsonResponse(response);

    url = "/_api/transaction/" + response.parsedBody.result.id;
    db._connection.DELETE(url);
    db._drop(cn);
@END_EXAMPLE_ARANGOSH_RUN

Referring to a non-existing collection

@EXAMPLE_ARANGOSH_RUN{RestTransactionBeginNonExisting}
    const cn = "products";
    db._drop(cn);
    let url = "/_api/transaction/begin";
    let body = {
      collections: {
        read : "products"
      }
    };

    var response = logCurlRequest('POST', url, body);
    assert(response.code === 404);

    logJsonResponse(response);
@END_EXAMPLE_ARANGOSH_RUN
@endDocuBlock
