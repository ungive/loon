# TODOs

## High

- [ ] Add support for QT websockets to the C++ client,
    as an alternative to libhv.
- [ ] Option to auto-restart after a given timeout to rotate client IDs?
    probably not a bad idea instead of e.g. staying connected for 24h
    and still using the same client ID and client secret
    and accumulating possibly a lot of authenticated generated URLs,
    which, if accumulated/collected, could be used to spam the client
    with requests, despite response caching.
- [ ] Add config option to limit how many chunks are buffered on the server.
- [ ] Add config option to enable TCP keep-alive or not.
    This should be a choice.
    If there's a cache in front of it it's e.g. probably a good idea,
    to keep a connection open between the server and the cache,
    instead of opening a new connection every time.

## Normal

- [ ] Call served/unregistered/failed callbacks on a separate thread?
    that way they can do more work without blocking client operation
    and client methods can be called without deadlocking.
- [ ] URL-encode path in generated URLs.
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

---

## Done

- [x] Request handler thread should be joined
- [x] Fix deadlock with request handler when restarting in send().
- [x] Websocket client: on_websocket_open does not use mutex?
- [x] Detect and log connection failure.
- [x] Separate logging for client and websocket.
- [x] Add proper logging to the C++ client
    (protobuf, libhv and protocol errors).
    The client ID should always be logged,
    this helps a ton with debugging on the server-side.
- [x] Fix max requests per second with the C++ client.
    Maximum requests per second should be *per content*,
    not *per connection*.
    If 10 pieces of content are registered per minute
    and each one is requested immediately,
    then a request limit of 5 requests per minute makes the connection fail,
    which is not what should happen.
- [x] Remove "with_callbacks" from "unregister_all_content" from C++ client
    and instead add flags to the unregistered callback
    indicating why it was unregistered (failure, disconnect, manually?).
- [x] Add method to iterate all content
    and allow unregistering it while iterating.
    That way a user can unregister e.g. all content except a selected few.
    Once implemented, remove the "unregister_all_content" method.
- [x] m_connected does not need to be atomic.
- [x] Rename failed to on_failed.
- [x] Add a timeout to "register_content".
    As it stands, it will just block forever if the client never connects.
- [x] If the connection fails, any thread that is waiting must be notified!
    Same todo as below: throw exception on failure.
- [x] Timeout for receiving Hello message
    \+ exception if it times out during registration or unregistration.
    Just reuse the connect timeout for this.
- [x] Properly document and implement if and how
    content remains registered across reconnects with the C++ client.
