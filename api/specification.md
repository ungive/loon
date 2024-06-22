# Specification

## Abstract

This specification describes a system that allows a client application
to make arbitrary content (usually files)
available to the internet via an HTTP(S) endpoint.
Clients connect to a server, which provides an HTTP base URL
under which clients can accept HTTP GET requests.
These requests are forwards to the client via a web socket connection,
upon which the client responds by sending data for the requested subpath
to the server, which then forwards it to the party that made the HTTP request.

The server effectively acts as a tunnel for clients to host files.
The benefit of this protocol is that it merely requires clients
to be connecting to the server via a web socket connection
and adhere to the protocol,
which results in a very low-profile and lightweight implementation.

## Protocol Messages

The described system heavily relies on a strict communication protocol.
Protocol messages are defined in `messages.proto`.
These messages will be referenced throughout the specification,
they define exactly which data is sent to connection peers.

## Overview

There are three main actors:

- A client which has files that it wants to be accessible from the internet.
- A third party which will access these files via the HTTP protocol.
- The server that acts as the middleman.

The server allows third parties to publicly access client content,
by forwarding HTTP GET requests to the respective client (if any).

This is done by allowing clients to connect
to a bidirectional websocket endpoint,
through which clients receive forwarded requests from the server
and through which clients provides the content
they wants to be accessible from the internet.

## Protecting clients against abuse

The following techniques are employed to protect clients against abuse:

1. Clients have the option to cache already served files on the server,
   so that subsequent requests of the same URL are read from a temporary cache,
   instead of being requested from the client over and over.
2. Already ongoing requests are not repeated,
   but further requests to the same file wait until the initial request
   has been completed and has been successfully commited to the cache,
   upon which the cached data will be used.
   If for some reason the initial request fails,
   requests that have waited should fail as well.
3. URLs for which requests will be forwarded are authenticated
   by the client with a cryptographic hash.
   Only requests to URLs with a valid hash will be forwarded to the client.
   This ensures that only valid URLs
   that are actually served by the client are forwarded.
   Doing it this way doesn't require the client to register valid files with
   the server and drastically reduces the number of invalid forwarded requests,
   therefore reducing the potential for abuse and required bandwidth.

If clients still receive too many requests,
they should close the connection and open a new one.

## Endpoints

### Websocket

```
/ws
```

Clients connect to this endpoint
to open a websocket connection with the server.
Websocket messages must be binary messages
encoded with the Protobuf encoding of those messages
that are defined in `messages.proto`.
Messages from the client must always be encapsulated as `ClientMessage`.
Message from the server must always be encapsulated as `ServerMessage`.

### HTTP

```
GET <base_url>/<client_id>/<hash>/<path>
```

A GET request to the above path
forwards a request for path `<path>` to the client with the ID `<client_id>`.
The `<path>` must not start with a slash and it is allowed to be empty.
The `client_id` must be the same value
as the `client_id` string field in the `Hello` protocol message.
The `<base_url>` contains protocol (HTTP or HTTPS),
hostname, port and some optional base URL path, if desired.
The exact details are left to the server implementation.
No other HTTP verbs are allowed or forwarded to the client.
It is required that the hash `<hash>` is a valid cryptographic hash,
encoded in some URL-friendly way (exactly how is left to the implementation),
which must have been computed in the following way:

```
hash = HMAC-SHA256(client_id || '/' || path, secret)
```

The `secret` is random sequence of bytes that has been generated
with a cryptographically secure random number generator
which is sent by the server to the client within the `Hello` message
and stored on the server for the lifetime of the connection.
`client_id` and `path` are taken from the URL above.
The `path` must neither be URL-encoded nor start with a slash
for the computation of the hash.
`||` is the string concatenation operator.
The HMAC must use the SHA-256 underlying hashing function,
or an equivalently secure hashing function.

The computed hash ensures that the request URL has been created by the client
and not by an unauthorized third party
and that there is some guarantee that the path exists at the client.
Any request whose hash cannot be authenticated on the server side
must be discarded immediately
and msut not be forwarded to the client through the websocket connection,
which prevents the client from being spammed with arbitrary requests
(point 3 under client abuse protection).
Only requests with a path that contain a valid hash may be accepted.

#### Rationale: No query parameters

Query parameters are useless, if they are part of the MAC hash.
Such a parameter is expected to be modifiable and dynamic,
without having to compute an entirely new MAC for the URL.
Values for query parameters are meant to be set by the HTTP client
and not by the server
(or the websocket client that generates the URLs in this case).
Possible values would be dictated by the websocket client,
which would limit their function to such a degree,
that one might as well not use query parameters at all.

By that reasoning query parameters *should not* be part of the MAC,
but that poses another problem, which conflicts with the design of the system:
If the point of the MAC
is to prevent HTTP clients from creating any number of unique request URLs
that could circumvent any cached response,
but we allow HTTP clients to set arbitrary query parameters
that don't need to be part of the MAC,
then what is the point of the MAC in the first place?
Setting a query parameter to a unique, never-seen-before value
requires the server to forward the request to the websocket client.
Allowing query parameters to not be part of the MAC
would render all caching and authentication,
as a means of abuse prevention, useless.

For that reason query parameters are never sent to a websocket client.

#### Status codes

The following status codes must be returned by the server
under the given circumstances
and possibly some actions need to be taken
in regard to the communication with the websocket client.

Code `200` (OK), if the connected websocket client
successfully sent a full response.
The body of the HTTP response contains the content that the client sent.

Code `400` (Bad Request), if the request is malformed.

Code `404` (Not Found) in the following cases:
- no client with the given client ID is connected to the server, or
- the MAC hash could not be authenticated for the given client, or
- the client returned an empty response.

The first two errors are combined under the `404` status code,
in order to hide to the outside when client ID is correct,
but the MAC is not. Attackers should not be able to tell the difference,
whether the client ID is incorrect or the MAC.
Only when the client ID is valid, a client is connected with that ID
and the MAC could be authenticated against the client's secret,
only then the request should proceed past a `404` status code.

Code `406` (Not Acceptable) in the following cases:
- none of the content types that are accepted in the HTTP request header
  overlap with the content types that the server accepts from the client, or
- the client returned a response with a content type
  which is not accepted in the HTTP request header.
  The server should then close the request with the websocket client.

Code `504` (Gateway Timeout), in the following cases:
- the connected websocket client did not send a response in time, or
- the connected websocket client has closed the response, or
- the connected websocket client has disconnected.

Further status codes may be enforced by the HTTP server implementation.

The HTTP server may send a text message explaining what happened.

Whenever an error occurs while handling an HTTP request
the request with the websocket client should be closed
by sending a `RequestClosed` message.

### Caching

HTTP responses from clients may be cached
(see point 1 under client abuse protection).
Note that caching should be done for an entire request URL,
including the `<base_url>`, `<client_id>`, `<hash>` and `<path>`.
The actual HTTP request URL should be used.

## Protocol

This section describes the protocol over a websocket connection
between any connected client and the server.
All messages are defined in `messages.proto`.

### Field descriptions

The meaning of message fields are described in the protobuf file
and may be further described here, if necessary.

### Message encapsulation

All messages from the client to the server are encapsulated
in a `ClientMessage` message.
All messages from the server to the client are encapsulated
in a `ServerMessage` message.
This ensures websocket peers can differentiate each type of message
that is being sent over the connection.

### Errors

Whenever a protocol error or a different error occurs,
the server will send a `Close` message,
which explains what happened,
and then closes the websocket connection.

### Hello

The first message of every connection is a `Hello` message,
which is sent by the server to the client
as soon as the websocket connection has been established.
The client may not send any messages
until it has received the `Hello` message from the server.

The `base_url` field contains the base URL for creating HTTP requests.
It represents the `<base_url>` in the Endpoints section.
It never ends with a trailing slash.

The `client_id` is the ID for the client that is connected to the server.
It represents the `<client_id>` in the Endpoints section.
It is randomly generated and unique per connection.

The `connection_secret` is the cryptographically secure secret
that must be used as the key
for computing the hash that is part of a request URL
(`<hash>` in the Endpoints section).
It is generated by the server and unique per connection.

The `constraints` field contains additional constraints
that are imposed on the client which must be respected,
otherwise the connection will be closed by the server with an error.
The available constraints are described in the `Constraints` message.

Once the `Hello` message has been sent,
the connection is ready to be used to exchange further messages.

### Constraints

The server must define the following constraints:

- `chunk_size` -
  Required size for content chunks in bytes.
  The last chunk may of course be smaller than this (but greater than zero).
- `max_content_size` -
  The maximum number of total bytes a client response may contain.
- `accepted_content_types` -
  A list of HTTP "Content-Type" values that are accepted in response messages.
  Responses may not contain content with a content type
  that does not conform to any of the listed types.
  This list should only containt "type" and "subtype",
  but not the "parameters" (anything after the first semicolon)
  of a content type
  (see https://www.w3.org/Protocols/rfc1341/4_Content-Type.html)

### Creating URLs

Before further messages are exchanged,
the connected client must create an HTTP URL
as described in the Endpoints section,
and send it to a third party which then makes a request to the HTTP endpoint.
That HTTP request will then trigger further exchange of message
between the websocket client and the server.

### Request

At any time the server may send a `Request` message
to indicate to the client that a valid HTTP GET request
has been made to the server
and that the client should send the respective content.

The `id` resembles the request ID,
which uniquely identifies the request within the connection.
It must be used in response messages to refer to a request.
The message ID must be larger than zero and unique per request.

The `timestamp` contains the time
at which this request has been received by the server.

The `path` is the path to identify the resource that is requested.
It represents the `<path>` in the Endpoints section
and never starts with a leading slash.
Path components should never be URL encoded.

### Closing a response

The client may close a response for any reason
by sending a `CloseResponse` message to the server.
The `request_id` field designates the request
for which the response should be closed.
The request for which the response should be closed
must exist and must not already be completed
(definition for a completed request is in the Response section).
It is okay to send a `CloseResponse` message in place of a response.

### Response

The client must send either an `EmptyResponse`, a `ContentHeader`
or a `CloseResponse` message in response to a `Request` message.

#### Completed responses

A response is completed by either an `EmptyResponse`
or by the last `ContentChunk` after a `ContentHeader`
(unless the content size is zero,
in which case `ContentHeader` completes the response).
A request is NOT completed,
if it is closed with a `CloseResponse` message.
Only when all data has been sent to the server,
a response is completed

#### Sending content

The client may send a `ContentHeader` message
in response to a received `Request` message,
to indicate that there is content available for the request
and to send metadata about that content.

The `request_id` maps the response to the correct request.

The `content_type` designates the HTTP "Content-Type"
of the data that will follow.
This content type must be accepted by the server,
see the `Constraints` message.
It may contain additional content type parameters after a semicolon,
e.g. `text/html; charset=utf-8`.
Parameters must be ignored by the implementation.

The `content_size` indicates the number of bytes in the content.
The content size must be less than or equal
to the allowed size in the `Constraints`,
but greater than or equal to zero.

The `max_cache_duration` indicates the maximum duration in seconds
for which the response data may be cached on the server.
A value of 0 indicates that the data should not be stored in any cache.

The `filename` may contain an optional filename,
which will be used as the filename for the data,
in case the file is downloaded with a browser.
This filename may be empty (optional),
but if it is set, it must have a length greater than 0.
This will set the HTTP response's "Content-Disposition"
to "attachment" with a "filename" set to the given value (quoted).

If a `ContentHeader` message is sent in response to a `Request` message,
a number of `ContentChunk` messages must follow,
which add up to the size of the content
as declared in the `ContentHeader` message,
and which all contain the request ID in the `request_id` field,
so they can be mapped to the correct response.
If the content size is zero, no chunks may follow.

Each chunk must have the `sequence` field set to a number
that is one larger than the sequence number of the previously sent chunk,
or 0 if it is the first chunk.
Chunks must be sent in order.

Each chunk's `data` field must contain the next chunk of data,
so that combining all received chunks
by contatenating them in the order of the sequence ID
would lead to the original data for the requested content.

The number of bytes in `data`
must match the chunk size in the `Constraints` message.
The last chunk is an exception,
it must have at least 1 byte and at most as many bytes
as the maximum chunk size allows.
The sum of all chunk sizes must match the `content_size`
that was specified in the `ContentHeader` message.

#### Empty response

The client may send an `EmptyResponse` message
in response to a received `Request` message,
to indicate that no content is available for the request.
The `request_id` maps the response to the correct request.

#### Missing response

If the client does not send an `EmptyResponse`, `ContentHeader`, `ContentChunk`
or `CloseResponse` message within a given time frame
(configured in the `Constraints` message),
the connection will be closed by the server with an error.

### Request closed

The server must send a `RequestClosed` message
if it does not want to receive a response or further response chunks.
This may be due to the HTTP client having closed the HTTP request
and the data not being needed anymore
or because the client is sending the response too slowly
and the request timed out.

If a request is canceled and the client is in the middle of sending a response,
it is okay for the client to still send a response
or continue sending chunks, the server should not trigger an error,
but the client should stop sending messages to save on bandwidth.
The server must discard any respones or further chunks for the request ID,
after a `RequestClosed` message has been sent for it,
and not forward it to any recipient.

If a request is closed in this way,
the client must acknowledge this by sending a `CloseResponse` message
within the timeout period,
otherwise the websocket connection will be closed.
Once the server receives a `CloseResponse`
it should free resources associated with a request and its response.

The server may not send a `RequestClosed` message,
if the response has already been completed,
as this would violate the protocol,
since the client would be then forced to send a `CloseResponse` message,
which would be invalid, since the request has been completed.

### Acknowledging successful responses

Once the server has fully received a client's response
and successfully sent it to the HTTP client that made the request,
the server should acknowledge this by sending a `Success` message
with the respective request ID in the `request_id` field.
This message is optional.

The `Success` message should only be sent for `ContentHeader` responses.
`EmptyResponse` messages should not be acknowledged.

### Disconnect

Once a websocket client disconnects,
the server should clear all cached files for that client.

## Server Protocol Interface

Proposed Golang protocol interface
for interfacing with a client from the server side,
which encapsulates protocol communication with a websocket client.

```go
type Client interface {
  // Runs the clients internal run loop.
  Run()
  // Sends a request to the client with the given path.
  // Checks whether the MAC is authentic,
  // with the client's client ID and client secret.
  // Returns a Request instance or an error when an error occurs.
  Request(path string, mac []byte) (Request, error)
  // Closes the client, if it isn't already closed, and exits the run loop.
  Close()
  // Returns a channel that is closed once the Run loop has fully terminated.
  Closed() <-chan struct{}
}

type Request interface {
  // Returns the channel that supplies the request's response.
  // The channel yields exactly one value and is then closed.
  // Yields a Response instance, if the client sends a ContentHeader,
  // and a nil value if the client sends an EmptyResponse.
  Response() <-chan Response
  // Indicate to the client that the request's response
  // has been successfully forwarded by sending a Success message.
  // May only be called if all chunks have been received.
  // Deletes the request internally.
  Success() error
  // Returns a channel that is closed once the request has been completed,
  // i.e. all chunks have been received by the websocket client.
  // Some chunks may still be buffer though
  // and should be read from the Response object,
  // before calling Success().
  Completed() <-chan struct{}
  // Returns a channel that is closed in the following cases:
  // - when the Client itself has been closed with the Close() method,
  // - when the Close() method is called on this Request,
  // - when the client has closed the response with a CloseResponse message,
  // - when the client times out because it did not respond in time, or
  // - when the client disconnected.
  Closed() <-chan struct{}
  // Closes the request prematurely by sending a RequestClosed message
  // to the websocket peer. Returns an error if the client is closed
  // or if the request has already been completed or closed.
  Close(message string) error
}

type Response interface {
  // Returns the content header for this response.
  Header() ContentHeader
  // The returned channel is closed if and only if
  // the response has been fully received.
  Chunks() <-chan []byte
}
```

## Tests

For tests see [pkg/server/client_test.go](../pkg/server/client_test.go).

## Client Capabilities

Any websocket client needs the following capabilities:

- Opening a websocket connection (WS or WSS)
- Providing an HTTP content type for any data that is supplied
- Decoding and encoding Protobuf protocol messages (either wire-encoded or JSON,
  depending on the server implementation)
- Sending the following protocol messages (see `messages.proto`):
  - `EmptyResponse` in response to a `Request`,
    if no data is available for the request
  - `ContentHeader` in response to a `Request` for available data
  - `ContentChunk` (possibly multiple) following a `ContentHeader`
    with the data chunked to the server-configured chunk size
  - `CloseResponse` in response to a `RequestClosed` message,
    to acknowledge to the server that no data follows for the response
    and resources can be freed
  - `CloseResponse` after a `ContentHeader`,
    if the data is somehow not available anymore,
    in case it is needed to cancel a response
    without closing the entire websocket connection
