![loon logo](./assets/loon-small.png)

> Note: The project is still a WIP.
> A license, a stable API and proper versioning will be available soon.

# loon &ndash; local files online

**<ins>loon</ins>** allows you to make
a **<ins>lo</ins>cal** resource
accessible **<ins>on</ins>line**. Instantly.

The loon client connects to the loon server over a persistent connection.
When you register content with the client, no network roundtrip is made,
a URL is generated locally and is immediately available for your use.

When a request is made to the URL (via HTTP or HTTPS),
the server forwards this request to your client
and the client uploads the content to the server,
which then forwards it to whomever made the initial request.
The loon client and server effectively act like a **tunnel for HTTP traffic**,
but with a very minimal footprint and several safety guarantees,
to keep your client small and lean
and remove the headaches of traditional tunnels.

The Go server and C++ client implementation are battle-tested
and actively used with [**Music Presence**](https://musicpresence.app),
a desktop application to show what you are listening to
in your Discord status.
Thanks to loon, Music Presence is the only application out there
that reliably displays album cover images on the user's profile!

## Features

- Generate a URL for any local resource in under 1 millisecond
- Efficiently recycle the server connection
  with automatic idling and delayed disconnects
- Client abstraction layer to use one underlying client connection
  for use with multiple shared clients in different,
  fully distinct application contexts
- Automatic reconnecting to the server, in case the connection is lost
- Support for client authentication via HTTP
  and server verification with CA certificates
- Lightweight, unit- and battle-tested client library written in C++
  and server written in Go
- Not just a side project.
  The loon client runs on well over 10.000 devices daily.
  See [here](https://musicpresence.app)
- All the safety guarantees you'd expect
  from software that tunnels traffic from the internet to your device

## Safety guarantees

- Your client never receives a request for a URL
  for which it didn't register any content
- Generated URLs are unique.
  Knowing a URL, it is infeasible to craft other URLs and test if they work.
  Each URL contains an HMAC that is computed from the URL path and an ephemeral,
  securely generated secret,
  which is only used for the lifetime of the connection and
  is only known to the client and server
- Generated URLs can't be easily guessed.
  Each content URL contains a random client ID,
  which is generated upon connecting to the server
  and discarded upon disconnecting.
  Together with the HMAC, the URL appears entirely random to external actors
- Uploaded content is cached on the server for a configurable duration,
  to not stress your bandwidth
- The client aborts,
  should the server not respect your client's minimum required cache duration
- The client aborts,
  when it receives too many requests in a configurable time frame
- Your public IP is never exposed to third-parties that request your content

## Limitations

- Generated URLs can be rather long (~100 characters),
  as the path contains the client ID, an HMAC and
  a path component selected by the client
- Registering different content over time, within the same connection
  and under the same path is possible, but not recommended,
  as the content will be accessible to someone
  who has knowledge of the old URL
- Anyone who knows the URL is able to access its contents
  and the server can see what is transmitted

Current limitations that might be remedied in the future:

- There is currently no support for compression,
  as the primary use case was to transmit images,
  which are already compressed.
  Support for compression is planned though:
  [#23](https://github.com/ungive/loon/issues/23)
- Content may be kept in the server's cache
  longer than it is registered with the client,
  depending on the maximum configured cache duration.
  There is currently no way to purge the cache for unregistered content,
  which may be undesirable for certain use cases.
  Discussion: [#22](https://github.com/ungive/loon/issues/22)
- Generated URLs can't be invalidated.
  Once content is unregistered,
  requests to the URL are still forwarded to the client
  and responses are not cached by the server.
  The client will disconnect from the server,
  should it get too many requests to old URLs,
  but this could be abused
  to make the client reconnect to the server,
  only with the knowledge of a URL that has no content anymore.
  Discussion: [#21](https://github.com/ungive/loon/issues/21)
- Only static paths without query parameters are supported.
  Each URL that is forwarded to the client must have been generated explicitly.
  Discussion: [#17](https://github.com/ungive/loon/issues/17)
- Uploading of registered content only starts when a request comes in.
  Content can't be uploaded in preparation of an incoming request yet.
  Discussion: [#27](https://github.com/ungive/loon/issues/27)
- The loon client currently uploads data as fast as it can,
  there currently is no way to limit the upload speed.
  Discussion: [#26](https://github.com/ungive/loon/issues/26)

## Architecture overview

There are three main actors with loon:

**loon client** &nbsp;&bullet;&nbsp;
The client library is used on the device that wishes to share a resource.
It connects to the server via a websocket connection
and remains connected for the duration it wants to share these resources.
Sharing a resource is as simple as registering it with the client
and generating a URL &ndash; this process is instant.
The URL can then be shared with a third party to access the resource.
Uploading is done when the server receives a request at the generated URL,
the request is forwarded to the client and then uploaded by the client
through the websocket connection.

**loon server** &nbsp;&bullet;&nbsp;
The server accepts both client websocket connections
and requests to URLs that were generated by these clients.
Its sole purpose is to provide a mechanism for clients
to generate URLs that are accessible from the internet
and to forward requests and responses between clients and third parties.

**Third party** &nbsp;&bullet;&nbsp;
Any third party with knowledge of a generated URL can make a request to it
to access the resource that has been registered by a client.

## Server limitations

The loon server in this repository
is **meant to be put behind a reverse proxy and a cache**.
The only caching mechanism that is provided by the implementation
is control over the `max-age` value in a response's `Cache-Control` header,
but the actual caching itself needs to be taken care of separately.

The reverse proxy and cache must guarantee the following
to prevent clients from being flooded with requests:

- Concurrent requests **MUST** lead to a single request to the client,
  whose response should then be cached and served.
  Concurrent requests must not lead to multiple requests to the client
- Any `Cache-Control` request header **MUST** be ignored,
  otherwise third parties can bypass the cache,
  e.g. by setting the `no-cache` directive.
  When using a reverse proxy, this header should be deleted from all requests.
  If not done properly, the cache is effectively rendered obsolete
  from an attackers point of view
- Query parameters are not forwarded to a connected client,
  so they **MUST** not be part of the cache key.
  Setting a meaningless query parameter should not bypass the cache
- The content **MUST** be committed to the cache once the upload finished
  and stay cached for the desired cache duration.
  This should go without saying, but it's worth mentioning anyway

A deployment example can be found in [**./deployments**](./deployments).

## What is in this repository?

- System and protocol specification: **[./api](./api)**
- A well-tested server library written in Go: **[./pkg/server](./pkg/server)**
- A well-tested, feature-complete, reference client library written in C++:
  **[./client](./client)**
  - C++17-compatible
  - Uses the CMake build-system
  - Conan and git submodules for dependency-management
  - Dependencies: OpenSSL, Protobuf, libdatachannel
- A server deployment example with Docker Compose and Caddy:
  **[./deployments](./deployments)**  
- A CLI program to run the server and a simple client: **[./cmd/loon](./cmd/loon)**
  - The Go client is very rudimentary
    and is just meant for development and testing
- A ready to use server Docker image:
  **[./build/package/Dockerfile](./build/package/Dockerfile)**  
  For a more production-ready example have a look at
  [github.com/music-presence/client-proxy](https://github.com/music-presence/client-proxy)
- An example server configuration to quickly get started:
  **[./examples/server/config.yaml](./examples/server/config.yaml)**

> **Warning!**  
> The public API of these libraries and programs
> is not yet stable and might change in the future.
> Wait for the first stable release, if a stable API is required.

## Basic usage

### Run the server

To expose the server to your local network,
change the value after `-addr` to `:8080`
and edit `base_url` in the used example configuration
to contain your computer's IP address and the configured port,
e.g. `http://192.168.178.2:8080`.

#### Using Go

```sh
git clone https://github.com/ungive/loon
cd ./loon
go install ./cmd/loon
loon server -addr localhost:8080 -config examples/server/config.yaml
```

#### Using Docker

Build the image yourself:

```sh
docker build -t loon -f build/package/Dockerfile .
docker run --rm -it -v $(pwd)/examples/server/config.yaml:/app/config.yaml -p 8080:80 ungive/loon:latest
loon client -server http://localhost:8080 assets/loon-small.png
```

Or use a pre-built image from the GitHub Container Registry
(not updated regularly yet):

```sh
docker run --rm -it -v $(pwd)/examples/server/config.yaml:/app/config.yaml -p 8080:80 ghcr.io/ungive/loon:latest
```

### Run the client

```sh
loon client -server http://localhost:8080 assets/loon-small.png
loon client -server http://192.168.178.43:8080 assets/loon-small.png assets/loon-full.png
```

Example output:

```
assets/loon-small.png: http://localhost:8080/BnRWzVodwVmY11V7MXQ-Mw/f7AapwFxVSwlHgiTungSpqFNkb_jAdkhvGVGaNeoWJc/loon-small.png
```

Open the URL in a browser and it should show the file's contents.

## Server deployment

For a docker compose deployment example, see [`deployments`](./deployments).
Features:

- Uses Caddy server (similar to nginx)
- Client responses are cached (using Souin cache)
- Prometheus and Grafana integration for metrics
- The websocket endpoint is authenticated,
  so only trusted clients can connect to the server
- Uses a self-signed certificate for websocket connections,
  so clients can authenticate the server
- Generated URLs are exposed via HTTPS with a certificate from a trusted CA
  (thanks to Caddy), so traffic is encrypted everywhere

## What problem does this solve?

To better understand the aim of this software
and what problem it tries to solve,
read the following problem statement:

You want a **local file** on your computer
to be **publicly accessible from the internet**
by generating **a simple HTTP(S) URL** for it,
which points to a well-known server
that acts as the middleman for file transfers.
Files should only be uploaded
when they are actually requested via that URL
and they should not be stored on the server indefinitely.

This should be possible without:
- exposing your local computer to the internet,
- having to start an HTTP server and binding to a port on the local device,
- having to use sophisticated tunneling software,
  which does more than you actually want it to,
- or using some kind of uploading service or cloud provider.

At the same time you want to have guaranteed protection from abuse:
- multiple requests to the same generated URL
  should use an optionally cached response on the server,
  so that you only have to upload your file once,
  within a given period of time.
- simultaneous requests to the same URL
  should only require you to upload once, not multiple times.
- requests to invalid URLs (not generated by you)
  should be ignored by the server.

File contents do not need to be end-to-end encrypted,
it is okay for any third party to see what it is in the file,
including the intermediate server and anyone in possession of a generated URL.

## The solution

The proposed solution is a server that acts as a middleman,
which accepts requests to a well-known HTTPS endpoint
and forwards them to a client through a websocket connection.
The client then sends the image data back through that connection,
which is then sent to whoever made the request.

The protocol is described in full detail in
**[api/specification.md](./api/specification.md)**.

## Use cases

### Discord Rich Presence

This software was primarily designed for the use case of
hosting an image online, that is only available locally,
for a temporary amount of time,
so that the image can be used within a Discord activity status
(also known as Discord Rich Presence),
by supplying the generated HTTPS URL to the Discord Game SDK,
as the `largeImageKey` or `smallImageKey`.

Discord requires the image shown in a status
to either be one of a handful of previously uploaded images,
which is insufficient for this use case,
or a link to an image that is available via HTTPS.
Since the image is only available locally, it must be uploaded to a server
or a server on the client computer must be exposed to the internet.
The latter was not an option, so a solution was needed
to be able to temporarily upload images,
but with the added benfit that:
- images are not stored indefinitely (like with an image uploading service)
- the image is only uploaded anywhere when it is actually needed
  (sometimes a Discord status is never actually viewed by anybody)
- the client merely has to connect to a websocket server
  and adhere to a very simple communication protocol

This software solves that problem,
by allowing any client to generate URLs locally,
set them as an image in the Discord status
and supply the image contents for the case that the image is needed
via a websocket connection.

### Other use cases

Since files are not end-to-end encrypted
the number of other use cases are limited,
but it could certainly be used for other things as well.
If you have any ideas or if you are using this software for another use case,
feel free to open an issue to bring it to my attention!

## License

Project is still a WIP, a license will be added soon!

## Copyright

Copyright (c) 2024 Jonas van den Berg
