@startDocuBlock delete_api_view_view

@RESTHEADER{DELETE /_api/view/{view-name}, Drop a View, deleteView}

@RESTURLPARAMETERS

@RESTURLPARAM{view-name,string,required}
The name of the View to drop.

@RESTDESCRIPTION
Drops the View identified by `view-name`.

If the View was successfully dropped, an object is returned with
the following attributes:
- `error`: `false`
- `id`: The identifier of the dropped View

@RESTRETURNCODES

@RESTRETURNCODE{400}
If the `view-name` is missing, then a *HTTP 400* is returned.

@RESTRETURNCODE{404}
If the `view-name` is unknown, then a *HTTP 404* is returned.

@EXAMPLES

Using an identifier:

@EXAMPLE_ARANGOSH_RUN{RestViewDeleteViewIdentifierArangoSearch}
    var view = db._createView("productsView", "arangosearch");

    var url = "/_api/view/"+ view._id;
    var response = logCurlRequest('DELETE', url);
    assert(response.code === 200);
    logJsonResponse(response);
@END_EXAMPLE_ARANGOSH_RUN

Using a name:

@EXAMPLE_ARANGOSH_RUN{RestViewDeleteViewNameArangoSearch}
    var view = db._createView("productsView", "arangosearch");

    var url = "/_api/view/productsView";
    var response = logCurlRequest('DELETE', url);
    assert(response.code === 200);
    logJsonResponse(response);
@END_EXAMPLE_ARANGOSH_RUN
@endDocuBlock
