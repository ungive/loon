# Reference client implementation (C++17)

This directory contains the reference client implementation
to connect to a loon server and instantly generate internet-accessible URLs
for local files and resources.
It's well-tested and actively used
by [Music Presence](https://github.com/ungive/discord-music-presence).

## Features

- Easy-to-use API to connect to a loon server and register arbitrary content
- An internet-accessible HTTP URL is generated *instantly*,
  no network roundtrip needed
- Automatic idling built-in to disconnect after some time,
  when no content is registered anymore
- Automatic reconnecting on disconnect to ensure availability and reliability
- Configurable minimum cache duration to control
  how frequently content may be uploaded
- Automatic disconnects when the server doesn't cache
  an uploaded response appropriately or when generated URLs for old content
  are requested too frequently
- Server verification with a CA certificate (in-memory or on disk)
- Basic server authentication with username and password

## Progress

- The library is currently still in development
and the API may break across commits.
Documentation may be scarce and inconsistent,
but the tests should give a pretty good idea of how the library works
- The `Client` is well-tested and feature-complete
- The `SharedClient` class is almost complete, but is not entirely bug free yet
- The example in the examples directory
  as well as the generated Doxygen documentation may be out of date

## Areas to improve

- It should be safe to call client methods in event callbacks.
  It should e.g. be possible to re-register content in an event handler
  when the client disconnects, without risking a thread deadlock
- Currently one new thread is spawned for each piece of registered content.
  An internal thread pool should be used instead to save on resources
  and reduce overhead with lots of registered content
- The client currently uploads without bandwidth restrictions.
  It should be possible to limit the bandwidth
- Allow uploading content prematurely and prepopulate the server's cache,
  before a request is received
- Allow content to by served dynamically with a generator function,
  e.g. for cases where the content needs special computation to be available
  and can't be exposed with a stream object or in a buffer
- The source code could be organized a little better
- ...

## Dependencies

- Any C++17-compliant compiler
- Dependencies managed via Conan:
  - OpenSSL
  - Protobuf
- Git submodules:
  - libdatachannel (custom, pull requests pending)
- Test dependencies:
  - GTest (git submodule)
