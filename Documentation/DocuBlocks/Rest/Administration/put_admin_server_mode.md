
@startDocuBlock put_admin_server_mode

@RESTHEADER{PUT /_admin/server/mode, Set the server mode to read-only or default, setServerMode}

@RESTBODYPARAM{mode,string,required,string}
The mode of the server `readonly` or `default`.

@RESTDESCRIPTION
Update mode information about a server. The JSON response will contain
a field `mode` with the value `readonly` or `default`. In a read-only server
all write operations will fail with an error code of `1004` (_ERROR_READ_ONLY_).
Creating or dropping of databases and collections will also fail with error
code `11` (_ERROR_FORBIDDEN_).

This is a protected API. It requires authentication and administrative
server rights.

@RESTRETURNCODES

@RESTRETURNCODE{200}
This API will return HTTP 200 if everything is ok

@RESTRETURNCODE{401}
if the request was not authenticated as a user with sufficient rights

@endDocuBlock
