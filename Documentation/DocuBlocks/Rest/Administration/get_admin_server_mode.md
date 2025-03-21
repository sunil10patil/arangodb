@startDocuBlock get_admin_server_mode

@RESTHEADER{GET /_admin/server/mode, Return whether or not a server is in read-only mode, getServerMode}

@RESTDESCRIPTION
Return mode information about a server. The json response will contain
a field `mode` with the value `readonly` or `default`. In a read-only server
all write operations will fail with an error code of `1004` (_ERROR_READ_ONLY_).
Creating or dropping of databases and collections will also fail with error code `11` (_ERROR_FORBIDDEN_).

This API requires authentication.

@RESTRETURNCODES

@RESTRETURNCODE{200}
This API will return HTTP 200 if everything is ok

@endDocuBlock
