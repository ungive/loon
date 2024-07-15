# TODOs

## High

- [ ] Fix max requests per second with the C++ client.
    Maximum requests per second should be *per content*,
    not *per connection*.
    If 10 pieces of content are registered per minute
    and each one is requested immediately,
    then a request limit of 5 requests per minute makes the connection fail,
    which is not what should happen.
- [ ] Add config option to limit how many chunks are buffered on the server.
- [ ] Add config option to enable TCP keep-alive or not.
    This should be a choice.
    If there's a cache in front of it it's e.g. probably a good idea,
    to keep a connection open between the server and the cache,
    instead of opening a new connection every time.
- [ ] Add versioning to the server and client libraries.
    Perhaps make releases on GitHub.

## Normal

- [ ] Automatically update docs on commit or push.
- [ ] Add test automation via GitHub Actions.
    Run Go tests, generate coverage, run client tests.
    Add badges to README for test coverage.
- [ ] Add proper logging to the C++ client
    (protobuf, libhv and protocol errors).
    The client ID should always be logged,
    this helps a ton with debugging on the server-side.
    Perhaps also log the Hello message?

## Low
- [ ] Write tests for heavy load (multiple parallel requests, no caching)
- [ ] Add a way to clear cached paths for connected clients,
    so that anything that might be cached on the server can be removed.
    Just a way to free resources whenever needed.
    Maybe also add a server option "maximum cached paths per client",
    if it's ever exceeded the connection is closed.
- [ ] Add flag in client program to verify a server's certificate.
- [ ] Change "-server" option in Go client to use the WSS endpoint url.
- [ ] Wildcard content types are not supported in the server's constraints yet.
- [ ] Add support for QT websockets to the C++ client,
    as an alternative to libhv.
- [ ] Move image resizing into the client library?
    Might be a good idea, but might also mean additional maintenance
    that is not really a concern of the client library.
    Needs discussion.
- [ ] Add support for JSON protobuf encoding, if the need ever arises
    (e.g. in a browser implementation)
    or to generally simplify client implementation.
- [ ] Add support for rotation between multiple loon servers?
