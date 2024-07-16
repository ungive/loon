# local files online &ndash; loon

![loon logo](./assets/loon-small.png)

**<ins>loon</ins>** allows you to make
a **<ins>lo</ins>cal** file
accessible **<ins>on</ins>line**.

The loon client generates one URL per resource,
which points to an HTTP server,
which in turn forwards any request back to the loon client
through a websocket connection.
It acts like a **tunnel for HTTP traffic**,
but with a very minimal and lightweight implementation behind it,
ideal to keep your client small and lean.

## Architecture overview

There are three main actors with loon:

**Client** &ndash; The client library is used on the device
that wishes to share a resource.
It connects to the server via a websocket connection
and remains connected for the duration it wants to share these resources.
Sharing a resource is as simple as registering it with the client
and generating a URL - this process is instant.
The URL can then be shared with a third party to access the resource.
Uploading is done when the server receives a request at the generated URL,
the request is forwarded to the client and then uploaded by the client
through the websocket connection.
The generated URL becomes invalid once the resource has been unregistered
or the websocket connection has been closed.

**Server** &ndash; The server accepts both client websocket connections
and requests to URLs that were generated by these clients.
Its sole purpose is to provide a mechanism for clients
to generate URLs that are accessible from the internet
and to forward requests and responses between clients and third parties.

**Third party** &ndash; Any third party can make requests to generated URLs
to access the resource that has been registered by a client.

## Server limitations

The loon server in this repository
is **meant to be put behind a reverse proxy and a cache**.
The only caching mechanism that is provided by the implementation
is control over the `max-age` value in a response's `Cache-Control` header,
but the actual caching itself needs to be taken care of separately.

The used cache and/or reverse proxy must guarantee the following
to prevent clients from being flooded with requests:

- Concurrent requests **MUST** lead to a *single request* to the client,
  whose response should then be cached and served.
- Any `Cache-Control` request header **MUST** be ignored,
  otherwise third parties can bypass the cache,
  e.g. by setting the `no-cache` directive.
  When using a reverse proxy, this header should be deleted from all requests.
  If not done properly, the cache is effectively rendered obsolete
  from an attackers point of view.
  
A deployment example can be found in [**deployments**](./deployments).

## What is in this repository?

- System and protocol specification: **[api](./api)**
- A well-tested server library written in Go: **[pkg/server](./pkg/server)**
- A simple reference client library written in Go: **[pkg/client](./pkg/client)**
- A CLI program to run the server and client: **[cmd/loon](./cmd/loon)**
  - It makes use of the above server and client library
- A more feature-complete client library written in C++:
  **[clients/cpp](./clients/cpp)**
  - Well-tested and has more features than the reference client implementation
  - Dependencies: libhv or Qt for websocket communication + OpenSSL + Protobuf
  - Compatible with C++17 or newer
  - Uses the CMake build system
  - Less than 2.000 lines of code: 1.319 lines for commit `751c25`
  - Documentation: *https://ungive.github.io/loon/clients/cpp*
- A ready to use server Docker image:
  **[build/package/Dockerfile](./build/package/Dockerfile)**
- A server deployment example with Docker Compose and Caddy:
  **[deployments](./deployments)**
- An example server configuration to quickly get started:
  **[examples/server/config.yaml](./examples/server/config.yaml)**

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
