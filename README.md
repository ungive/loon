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

## License

Project is still a WIP, a license will be added soon!

## Usage

### Start a server

```sh
git clone https://github.com/ungive/loon
cd ./loon
go install ./cmd/loon
loon server -addr localhost:8080 -config examples/server/config.yaml | hl -F
```

To run the server on your local network, change `addr` to `:8080`
and edit the example configuration
to contain your computer's IP address and the configured port for `base_url`,
e.g. `http://192.168.178.2:8080`.
Output is piped to [`hl`](https://github.com/pamburus/hl),
which is a log formatter for JSON logs.

### Run the client

```sh
loon client -server http://localhost:8080 assets/loon-small.png
loon client -server http://192.168.178.43:8080 assets/loon-small.png assets/loon-full.png
```

### Dockerized

```sh
make build-image
docker run --rm -it -v $(pwd)/examples/server/config.yaml:/app/config.yaml -p 8080:80 ungive/loon:latest
loon client -server http://localhost:8080 assets/loon-small.png
```

or using a pre-built image from the GitHub Container Registory:

```sh
docker run --rm -it -v $(pwd)/examples/server/config.yaml:/app/config.yaml -p 8080:80 ghcr.io/ungive/loon:latest
```

### Deployment

For a deployment example, see
[`deployments`](./deployments).

## The problem

To start, here is a problem description,
to best understand what this software does:

You want a **local file** on your computer
to be **publicly accessible from the internet**
by generating **a simple HTTP(S) URL** for it,
which points to a well-known server
that acts as the middleman for file transfers.
Files should only be uploaded
when they are actually requested via that URL.

This should be possible without:
- exposing your local computer to the internet,
- having to start an HTTP server and binding to a port locally,
- having to use sophisticated tunneling software,
  which does more than you actually want it to.

At the same time you want to have guaranteed protection from abuse:
- multiple requests to the same generated URL
  should use an optionally cached response,
  so that you only have to upload your file once,
  within a given period of time
- simultaneous requests to the same URL
  should only require you to upload once, not multiple times
- requests to invalid URLs (not generated by you)
  should be ignored by the server

You do not need file contents to be end-to-end encrypted,
it is okay for any third party to see what it is in the file
(the intermediate server included).

## The solution

The proposed solution is a server that acts as a middleman,
which accepts requests to a well-known HTTPS endpoint
and forwards them to you (the client) through a websocket connection.
You then send the image data back through that connection,
which is then sent to the server.

The protocol is described in full detail in
**[api/specification.md](./api/specification.md)**.

For instructions on how to use each of the following solutions,
read the [**Usage**](#usage) section below.

A reference **server implementation** is provided in `Golang`.
The source code can be found in the [`pkg/server`](./pkg/server) directory.
It has the following features:

- A well-tested server protocol implementation
  (89.7% test coverage for [`client.go`](./pkg/server/client.go) so far)
  - **`TODO`** *add tests for the entire server
    and add automated test coverage README badge*
- A ready-to-use Docker image and docker-compose file
  - **`TODO`** *not available yet*

**`TODO`** A **client library** is provided in `C++`.
The source code can be found in the [`clients/cpp`](./clients/cpp) directory.

**`TODO`** A reference **client implementation** in `Go`
may be provided in the future in the [`pkg/client`](./pkg/client) directory.

More client implementations may follow,
namely a JavaScript implementation for the server (Node)
and the browser, so it can be used more widely for other use cases.
Contributions are welcome!

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

## Copyright

Copyright (c) 2024 Jonas van den Berg
