# TODOs

## High

- [ ] Auto-restart after...? for new client ID, to stay anonymous or idk
    as an option for the client
- [ ] Add proper logging to the C++ client
    (protobuf, libhv and protocol errors).
    The client ID should always be logged,
    this helps a ton with debugging on the server-side.
    Perhaps also log the Hello message?
- [ ] Add support for QT websockets to the C++ client,
    as an alternative to libhv.

## Normal

- [ ] Currently the server synchronizes *all requests*
    through the client manager.
    There should be at least *some* concurrency (with goroutines)
    to handle simultaneous requests concurrently.
- [ ] Add a feature to "prepopulate" the cache:
    The client makes a request to the URL, but maybe a special URL,
    which causes the server to request and cache the response,
    but the response is not unnecessarily forwarded to the client.
    Basically a "HEAD" HTTP request, which populates the cache.
    This would be useful to trigger an immediate upload of the data,
    in case the upload might take longer.
    Basically allowing loon to be an "opt-in temporary upload service".
    Could be done with `content_handle->populate_cache();`?
    What if the cache expires?
    Maybe add a "minimum cache duration" *per content*?
    Note: Maybe add a protocol message to abstract away from the client
    how exactly the cache is prepopulated.
    The server should perhaps make a request to a URL
    that was specified in the config, which prepopulates the cache.
    Maybe cache-handler/Souin provides a mechanism for that with Caddy?
    Maybe add a callback that would be called to prepopulate the cache,
    which can be registered with the server library API?
    That way, if prepopulation is required, users can write their own server
    instead of running "loon server" directly.
- [ ] Add versioning to the server and client libraries.
    Perhaps make releases on GitHub.

## Low
- [ ] Add config option to limit how many chunks are buffered on the server.
- [ ] Add config option to enable TCP keep-alive or not.
    This should be a choice.
    If there's a cache in front of it it's e.g. probably a good idea,
    to keep a connection open between the server and the cache,
    instead of opening a new connection every time.
- [ ] Automatically update docs on commit or push.
- [ ] Add test automation via GitHub Actions.
    Run Go tests, generate coverage, run client tests.
    Add badges to README for test coverage.
- [ ] Write tests for heavy load (multiple parallel requests, no caching)
- [ ] Add a way to clear cached paths for connected clients,
    so that anything that might be cached on the server can be removed.
    Just a way to free resources whenever needed.
    Maybe also add a server constraint "maximum cached paths per client",
    if it's ever exceeded the connection is closed.
- [ ] Add flag in the Go CLI program to verify a server's certificate.
- [ ] Change "-server" option in Go client to use the WSS endpoint url.
- [ ] Wildcard content types are not supported in the server's constraints yet.
- [ ] Move image resizing into the client library?
    Might be a good idea, but might also mean additional maintenance
    that is not really a concern of the client library.
    Needs discussion.
- [ ] Add support for JSON protobuf encoding, if the need ever arises
    (e.g. in a browser implementation)
    or to generally simplify client implementation.
- [ ] Add support for rotation between multiple loon servers?
