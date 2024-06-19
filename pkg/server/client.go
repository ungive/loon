package server

import (
	"bytes"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"log"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/known/timestamppb"

	"github.com/ungive/loon/pkg/pb"
)

const (
	MAC_KEY_SIZE     = 64
	CONTENT_TYPE_SEP = ";"
)

var (
	ErrClientClosed        = errors.New("client connection closed")
	ErrBadPath             = errors.New("the path is malformed")
	ErrBadQuery            = errors.New("the query is malformed")
	ErrBadMac              = errors.New("the MAC hash is invalid")
	ErrRequestClosed       = errors.New("the request is closed")
	ErrRequestNotCompleted = errors.New("the request is not completed yet")
)

type Client interface {
	// Run loop for the client.
	Run()
	// Returns the ID of this client.
	ID() UUID
	// Sends a request to the client, with the given path and query.
	// Checks whether the MAC is authentic,
	// with the client's client ID and client secret.
	// Returns a Request instance or an error when an error occurs.
	Request(path string, query string, mac []byte) (Request, error)
	// Returns the number of requests that are currently active.
	// May include opened, closed and incomplete requests.
	ActiveRequests() (int, error)
	// Closes the connection and exits the Run loop,
	// if it isn't already closed.
	Close()
	// Returns a channel that is closed once the Run loop has fully terminated.
	Closed() <-chan struct{}
}

type Request interface {
	// Returns the request ID.
	ID() uint64
	// Returns the requested path.
	Path() string
	// Returns the query string of the request.
	Query() string
	// Returns the channel that supplies the request's response.
	// The channel yields exactly one value and is then closed.
	// Yields a Response instance, if the client sends a ContentHeader,
	// and a nil value if the client sends an EmptyResponse.
	// The channel is closed if the request is closed,
	// either because Close() was called on this Request instance
	// or because the response from the client has timed out.
	Response() chan Response
	// Indicate to the client that the request's response
	// has been successfully forwarded by sending a Success message.
	// May only be called if all chunks have been received.
	Success() error
	// Returns a channel that is closed if the request is closed,
	// either because Close() was called on this Request,
	// because the response from the client has timed out
	// or because the client closed the response prematurely
	// by sending a CloseResponse message.
	Closed() chan struct{}
	// Closes the request prematurely, if it hasn't already been completed,
	// by sendign a RequestClosed message to the client.
	// Does nothing if the request has already been closed.
	Close()
}

type Response interface {
	// Returns the request this response is associated with.
	Request() Request
	// Returns the content header for this response.
	Header() *pb.ContentHeader
	// Returns the channel that supplies the sender's chunks.
	// The returned channel is never closed.
	Chunks() chan []byte
	// Returns a channel that is closed,
	// when the response has been closed by the connected client
	// because it sent a CloseResponse message
	// or when the client disconnected.
	Closed() chan struct{}
}

type internalRequest struct {
	client *clientImpl
	id     uint64
	path   string
	query  string

	pendingResponse  chan Response
	response         *internalResponse
	responseProvided bool

	lastUpdated time.Time
	completed   bool
	closed      chan struct{}
}

func newRequest(
	client *clientImpl,
	requestID uint64,
	path string,
	query string,
) *internalRequest {
	return &internalRequest{
		client: client,
		id:     requestID,
		path:   path,
		query:  query,
		// This channel must be buffered, otherwise handling a response message
		// would block until the caller reads the response, which would prevent
		// all further communication with the client.
		// This way the response can also be ignored and discarded
		// by simply leaving it in the buffer and deleting the channel.
		pendingResponse:  make(chan Response, 1),
		response:         nil,
		responseProvided: false,
		lastUpdated:      time.Now(),
		completed:        false,
		closed:           make(chan struct{}),
	}
}

// Note that public methods (capitalized) of internalRequest
// are not meant to be used by the implementation below.

func (r *internalRequest) ID() uint64 {
	return r.id
}

func (r *internalRequest) Path() string {
	return r.path
}

func (r *internalRequest) Query() string {
	return r.query
}

func (r *internalRequest) Response() chan Response {
	return r.pendingResponse
}

func (r *internalRequest) Success() error {
	outErr := make(chan error)
	select {
	case r.client.triggerSuccess <- &forwardSuccess{
		request: r,
		outErr:  outErr,
	}:
	case <-r.client.done:
		outErr <- ErrClientClosed
	}
	return <-outErr
}

func (r *internalRequest) Closed() chan struct{} {
	return r.closed
}

func (r *internalRequest) Close() {
	select {
	case r.client.triggerClose <- &forwardClose{
		request: r,
	}:
	case <-r.client.done:
	}
}

// Reset the internal timeout timestamp.
func (r *internalRequest) resetTimeout() {
	if !r.isClosed() {
		// Only reset the timeout timestamp if the request is not closed,
		// since if it is closed, the next message must be a CloseResponse,
		// which concludes the entire request and deletes it.
		r.lastUpdated = time.Now()
	}
}

// Provide a response for this request.
// The reponse is forwarded to the calling code that made the request.
func (r *internalRequest) provideResponse(response *internalResponse) {
	if r.responseProvided {
		panic("response already provided")
	}
	r.responseProvided = true
	r.response = response
	r.pendingResponse <- response
	close(r.pendingResponse)
}

// Check if this request has a response,
// i.e. if a response was provided via provideResponse().
func (r *internalRequest) hasResponse() bool {
	return r.responseProvided
}

// Get the internal response.
// Check hasResponse() before using the result of this function.
func (r *internalRequest) getResponse() *internalResponse {
	return r.response
}

// Marks this request as closed.
func (r *internalRequest) close() {
	if r.completed {
		return
	}
	if r.isClosed() {
		panic("the request has already been closed")
	}
	close(r.closed)
}

// Checks if this request has been marked as closed with close().
func (r *internalRequest) isClosed() bool {
	select {
	case <-r.closed:
		return true
	default:
		return false
	}
}

// Marks this request as completed.
func (r *internalRequest) complete() {
	if r.completed {
		panic("the request has already been completed")
	}
	if r.isClosed() {
		panic("cannot complete a closed request")
	}
	r.completed = true
}

// Checks if this request has been marked as completed with complete().
func (r *internalRequest) isCompleted() bool {
	return r.completed
}

type internalResponse struct {
	request       *internalRequest
	header        *pb.ContentHeader
	chunkSequence uint64
	chunks        chan []byte
	canceled      chan struct{}
	isCancelled   bool
}

func newResponse(
	request *internalRequest,
	header *pb.ContentHeader,
) *internalResponse {
	return &internalResponse{
		request:       request,
		header:        header,
		chunkSequence: 0,
		chunks:        make(chan []byte, 8),
		canceled:      make(chan struct{}),
		isCancelled:   false,
	}
}

func (r *internalResponse) Request() Request {
	return r.request
}

func (r *internalResponse) Header() *pb.ContentHeader {
	return r.header
}

func (r *internalResponse) Chunks() chan []byte {
	return r.chunks
}

func (r *internalResponse) Closed() chan struct{} {
	return r.canceled
}

func (r *internalResponse) cancel() {
	if !r.isCancelled {
		r.isCancelled = true
		close(r.canceled)
	}
}

func (r *internalResponse) nextChunkInfo() (sequence uint64, size uint64, last bool) {
	total := r.header.ContentSize
	received := r.chunkSequence * r.request.client.config.Constraints.ChunkSize
	if received >= total {
		panic("already received everything")
	}
	size = min(total-received, r.request.client.config.Constraints.ChunkSize)
	last = received+size == total
	sequence = r.chunkSequence
	r.chunkSequence++
	return
}

type forwardRequest struct {
	path  string
	query string
	out   chan Request
}

type forwardSuccess struct {
	request *internalRequest
	outErr  chan error
}

type forwardClose struct {
	request *internalRequest
}

type clientImpl struct {
	id     UUID
	secret []byte
	config *ClientConfig
	dirty  atomic.Bool

	conn    *websocket.Conn
	recv    chan *pb.ClientMessage
	send    chan *pb.ServerMessage
	stop    chan *pb.Close
	done    chan struct{}
	runDone chan struct{}

	requests      map[uint64]*internalRequest
	countRequests chan chan int
	nextRequest   uint64

	forwardRequest chan *forwardRequest
	triggerSuccess chan *forwardSuccess
	triggerClose   chan *forwardClose
}

type ClientConfig struct {
	BaseUrl     string
	Constraints *pb.Constraints
	Intervals   *ProtocolIntervals
}

func (c *ClientConfig) Clone() *ClientConfig {
	return &ClientConfig{
		BaseUrl:     c.BaseUrl,
		Constraints: proto.Clone(c.Constraints).(*pb.Constraints),
		Intervals:   c.Intervals,
	}
}

func NewClient(conn *websocket.Conn, config *ClientConfig) (Client, error) {
	id, err := NewUUID()
	if err != nil {
		return nil, err
	}
	secret := [MAC_KEY_SIZE]byte{}
	_, err = io.ReadFull(rand.Reader, secret[:])
	if err != nil {
		return nil, err
	}
	client := &clientImpl{
		id:             id,
		secret:         secret[:],
		config:         config.Clone(),
		dirty:          atomic.Bool{},
		conn:           conn,
		recv:           make(chan *pb.ClientMessage, 256),
		send:           make(chan *pb.ServerMessage, 256),
		stop:           make(chan *pb.Close),
		done:           make(chan struct{}),
		runDone:        make(chan struct{}),
		requests:       make(map[uint64]*internalRequest),
		countRequests:  make(chan chan int),
		nextRequest:    0,
		forwardRequest: make(chan *forwardRequest),
		triggerSuccess: make(chan *forwardSuccess),
		triggerClose:   make(chan *forwardClose),
	}
	// Make sure the first message that we send is a Hello message,
	// by putting it into the buffer before the caller can call Run().
	client.send <- &pb.ServerMessage{
		Data: &pb.ServerMessage_Hello{
			Hello: &pb.Hello{
				Constraints:      client.config.Constraints,
				ClientId:         client.id.String(),
				ConnectionSecret: client.secret,
				BaseUrl:          strings.TrimSuffix(config.BaseUrl, "/"),
			},
		},
	}
	return client, nil
}

func (c *clientImpl) Run() {
	if !c.dirty.CompareAndSwap(false, true) {
		// A client's run loop may only be called once.
		panic("client is in a dirty state")
	}
	var wg sync.WaitGroup
	wg.Add(3)
	go func() { c.writePump(); wg.Done() }()
	go func() { c.readPump(); wg.Done() }()
	go func() { c.protocol(); wg.Done() }()
	wg.Wait()
	close(c.runDone)
}

func (c *clientImpl) ID() UUID {
	return c.id
}

func (c *clientImpl) Request(path string, query string, mac []byte) (Request, error) {
	if len(path) == 0 || !strings.HasPrefix(path, "/") {
		return nil, ErrBadPath
	}
	if len(query) > 0 && !strings.HasPrefix(query, "?") {
		return nil, ErrBadQuery
	}
	computedMac, err := c.computeMac(path, query)
	if err != nil {
		return nil, err
	}
	if !bytes.Equal(mac, computedMac) {
		return nil, ErrBadMac
	}
	out := make(chan Request)
	select {
	case c.forwardRequest <- &forwardRequest{
		path:  path,
		query: query,
		out:   out,
	}:
	case <-c.done:
		return nil, ErrClientClosed
	}
	return <-out, nil
}

func (c *clientImpl) ActiveRequests() (int, error) {
	out := make(chan int)
	select {
	case c.countRequests <- out:
	case <-c.done:
		return 0, ErrClientClosed
	}
	return <-out, nil
}

func (c *clientImpl) Close() {
	c.close(pb.Close_REASON_CLOSED, "Connection was closed by the server")
}

func (c *clientImpl) Closed() <-chan struct{} {
	return c.runDone
}

func (c *clientImpl) computeMac(path string, query string) ([]byte, error) {
	mac := hmac.New(sha256.New, c.secret)
	items := [][]byte{
		[]byte(c.id.String()),
		[]byte("/"),
		[]byte(strings.TrimPrefix(path, "/")),
		[]byte("?"),
		[]byte(strings.TrimPrefix(query, "?")),
	}
	for _, item := range items {
		n, err := mac.Write(item)
		if err != nil {
			return nil, err
		}
		if n != len(item) {
			return nil, errors.New("write did not write all bytes")
		}
	}
	return mac.Sum(nil), nil
}

// Queues a close message for this connection
// and blocks until it was written and the client connection has been closed.
// Makes sure that the write loop is exited.
func (c *clientImpl) close(
	reason pb.Close_Reason,
	format string,
	a ...interface{},
) {
	select {
	case c.stop <- &pb.Close{
		Reason:  reason,
		Message: fmt.Sprintf(format, a...),
	}:
	case <-c.done:
		return
	}
	// Block until the stop message has been processed
	// and the client's run loops are all done
	// (or continue if they were already done)
	<-c.done
}

// Identical to close(), but does not send a Close message.
func (c *clientImpl) exit() {
	select {
	case c.stop <- nil:
	case <-c.done:
	}
	<-c.done
}

// Pumps incoming messages from the websocket connection to the recv channel.
func (c *clientImpl) readPump() {
	// Chunks are the largest messages, so make sure a message can fit one.
	c.conn.SetReadLimit(max(512, 2*int64(c.config.Constraints.ChunkSize)))
	c.conn.SetReadDeadline(time.Now().Add(c.config.Intervals.PongWait))
	c.conn.SetPongHandler(func(string) error {
		c.conn.SetReadDeadline(time.Now().Add(c.config.Intervals.PongWait))
		return nil
	})
	// Always call c.close() before returning from the loop,
	// so that the loop in writePump() closes the done channel
	// and therefore returns from writePump() and protocol().
	for {
		t, data, err := c.conn.ReadMessage()
		select {
		case <-c.done:
			return
		default:
		}
		if websocket.IsUnexpectedCloseError(err) {
			c.exit()
			return
		}
		if err != nil {
			c.close(pb.Close_REASON_ERROR,
				"Failed to read from websocket connection: %v", err)
			return
		}
		if t == websocket.TextMessage {
			c.close(pb.Close_REASON_INVALID_CLIENT_MESSAGE,
				"Only accepting binary websocket messages")
			return
		}
		if t != websocket.BinaryMessage {
			continue
		}
		message := &pb.ClientMessage{}
		err = proto.Unmarshal(data, message)
		if err != nil {
			c.close(pb.Close_REASON_INVALID_CLIENT_MESSAGE,
				"Failed to unmarshal proto message: %v", err)
			return
		}
		// fmt.Printf("Got: %v", message)
		select {
		case <-c.done:
			return
		default:
		}
		select {
		case <-c.done:
			return
		case c.recv <- message:
		}
	}
}

// Writes a protocol server message to the websocket connection.
func (c *clientImpl) write(message *pb.ServerMessage) error {
	data, err := proto.Marshal(message)
	if err != nil {
		log.Printf("Failed to marshal message: %v", err)
		return err
	}
	err = c.conn.SetWriteDeadline(time.Now().Add(c.config.Intervals.WriteWait))
	if err != nil {
		return err
	}
	err = c.conn.WriteMessage(websocket.BinaryMessage, data)
	return err
}

// Pumps outgoing messages from the send and close channel to the connection.
// NOTE: Only call once, since close(c.done) would cause a panic otherwise.
func (c *clientImpl) writePump() {
	ticker := time.NewTicker(c.config.Intervals.PingInterval)
	done := c.done
	defer func() {
		if done != nil {
			// Make sure the other run loops are closed.
			close(done)
		}
		ticker.Stop()
		c.conn.WriteMessage(websocket.CloseMessage, []byte{})
		c.conn.Close()
	}()
	for {
		select {
		case message := <-c.stop:
			// Close the done channel as early as possible.
			close(done)
			done = nil
			if message != nil {
				c.write(&pb.ServerMessage{
					Data: &pb.ServerMessage_Close{
						Close: message,
					},
				})
			}
			return
		case message := <-c.send:
			err := c.write(message)
			if err != nil {
				return
			}
		case <-ticker.C:
			err := c.conn.SetWriteDeadline(
				time.Now().Add(c.config.Intervals.WriteWait))
			if err != nil {
				return
			}
			err = c.conn.WriteMessage(websocket.PingMessage, nil)
			if err != nil {
				return
			}
		}
	}
}

// Executes the run loop for handling all protocol messages.
func (c *clientImpl) protocol() {
	timeoutTicker := time.NewTicker(c.config.Intervals.TimeoutInterval)
	defer func() {
		timeoutTicker.Stop()
		// Close all requests and cancel all respones when the protocol ends.
		for _, request := range c.requests {
			if !request.isClosed() {
				request.close()
			}
			if request.hasResponse() {
				request.getResponse().cancel()
			}
		}
		// Clear some memory of possibly big objects.
		c.requests = nil
	}()
	for {
		select {
		case <-c.done:
			return
		default:
		}
		select {
		case message := <-c.recv:
			switch data := message.Data.(type) {
			case *pb.ClientMessage_EmptyResponse:
				c.onEmptyResponse(data.EmptyResponse)
			case *pb.ClientMessage_ContentHeader:
				c.onContentHeader(data.ContentHeader)
			case *pb.ClientMessage_ContentChunk:
				c.onContentChunk(data.ContentChunk)
			case *pb.ClientMessage_CloseResponse:
				c.onCloseResponse(data.CloseResponse)
			default:
				panic("unknown client message type")
			}
		case info := <-c.forwardRequest:
			c.sendRequest(info)
		case out := <-c.countRequests:
			out <- len(c.requests)
		case info := <-c.triggerSuccess:
			c.sendSuccess(info)
		case info := <-c.triggerClose:
			c.sendRequestClosed(info)
		case <-timeoutTicker.C:
			c.checkTimeouts()
		case <-c.done:
			return
		}
	}
}

func (c *clientImpl) checkTimeouts() {
	now := time.Now()
	for _, request := range c.requests {
		if now.Sub(request.lastUpdated) < c.config.Intervals.TimeoutDuration {
			continue
		}
		if request.isClosed() {
			// Close the connection if the request is closed and timed out
			// because the client was meant to send a CloseResponse message.
			c.close(pb.Close_REASON_TIMED_OUT,
				"Response for closed request was not closed in time [#%d]",
				request.id)
			return
		}
		c.sendRequestClosed(&forwardClose{
			request: request,
		})
		if request.hasResponse() {
			request.getResponse().cancel()
		}
		if !request.isClosed() && request.isCompleted() {
			c.deleteRequest(request)
		}
	}
}

func (c *clientImpl) sendMessage(message *pb.ServerMessage) {
	select {
	case c.send <- message:
	case <-c.done:
	}
}

func (c *clientImpl) sendRequest(info *forwardRequest) {
	var id uint64
	for {
		id = c.nextRequestID()
		if _, ok := c.requests[id]; !ok {
			break
		}
	}
	request := newRequest(c, id, info.path, info.query)
	c.requests[id] = request
	info.out <- request
	c.sendMessage(&pb.ServerMessage{
		Data: &pb.ServerMessage_Request{
			Request: &pb.Request{
				Id:        id,
				Path:      info.path,
				Query:     info.query,
				Timestamp: timestamppb.Now(),
			},
		},
	})
}

func (c *clientImpl) sendSuccess(info *forwardSuccess) {
	request, ok := c.requests[info.request.id]
	if !ok {
		// This request does not exist.
		info.outErr <- ErrRequestClosed
		return
	}
	if request.isClosed() {
		// Cannot send a success message for a closed request.
		info.outErr <- ErrRequestClosed
		return
	}
	if !request.isCompleted() {
		// The request must be completed before sending a success message.
		info.outErr <- ErrRequestNotCompleted
		return
	}
	c.sendMessage(&pb.ServerMessage{
		Data: &pb.ServerMessage_Success{
			Success: &pb.Success{
				RequestId: request.id,
			},
		},
	})
	c.deleteRequest(request)
	info.outErr <- nil
}

func (c *clientImpl) sendRequestClosed(info *forwardClose) {
	request, ok := c.requests[info.request.id]
	if !ok {
		// This request does not exist.
		return
	}
	if request.isClosed() {
		// Request is already closed.
		return
	}
	c.sendMessage(&pb.ServerMessage{
		Data: &pb.ServerMessage_RequestClosed{
			RequestClosed: &pb.RequestClosed{
				RequestId: request.id,
			},
		},
	})
	// Reset the timeout and mark the request as closed.
	// This will make sure the timeout is not reset anymore after this
	// and since we are not deleting the request from the internal map,
	// the client has to send a CloseResponse message in time.
	// But since we are resetting the timeout one last time,
	// the client gets another timeout period of time
	// before it has to answer with a CloseResponse message.
	request.resetTimeout()
	request.close()
}

func (c *clientImpl) onCloseResponse(info *pb.CloseResponse) {
	request, ok := c.requests[info.RequestId]
	if !ok {
		c.close(pb.Close_REASON_INVALID_REQUEST_ID,
			"Closed response for unknown request ID %d", info.RequestId)
		return
	}
	if request.completed {
		c.close(pb.Close_REASON_INVALID_CLIENT_MESSAGE,
			"Cannot close a response that is already completed [#%d]", request.id)
		return
	}
	if !request.hasResponse() {
		c.close(pb.Close_REASON_INVALID_CLIENT_MESSAGE,
			"Must receive a response before closing it [#%d]", request.id)
		return
	}
	request.getResponse().cancel()
	// This is the only point at which responses that have content
	// are deleted from the internal map.
	c.deleteRequest(request)
}

func (c *clientImpl) onEmptyResponse(response *pb.EmptyResponse) {
	request, ok := c.requests[response.RequestId]
	if !ok {
		c.close(pb.Close_REASON_INVALID_REQUEST_ID,
			"Response for unknown request ID %d", response.RequestId)
		return
	}
	if request.id != response.RequestId {
		panic("inconsistent request IDs")
	}
	if request.hasResponse() {
		c.close(pb.Close_REASON_INVALID_CLIENT_MESSAGE,
			"Already received a response for this request [#%d]",
			request.id)
		return
	}
	request.provideResponse(nil)
	c.completeRequest(request)
	// Empty responses are not acknowledged with a Success message,
	// therefore we can immediately delete them here.
	c.deleteRequest(request)
}

func (c *clientImpl) onContentHeader(header *pb.ContentHeader) {
	request, ok := c.requests[header.RequestId]
	if !ok {
		c.close(pb.Close_REASON_INVALID_REQUEST_ID,
			"Response for unknown request ID %d", header.RequestId)
		return
	}
	if request.id != header.RequestId {
		panic("inconsistent request IDs")
	}
	if request.hasResponse() {
		c.close(pb.Close_REASON_INVALID_CLIENT_MESSAGE,
			"Already received a response for this request [#%d]",
			request.id)
		return
	}
	if header.ContentSize > c.config.Constraints.MaxContentSize {
		c.close(pb.Close_REASON_INVALID_CONTENT_SIZE,
			"Content size exceeds allowed maximum content size [#%d]",
			request.id)
		return
	}
	if header.Filename != nil && len(*header.Filename) == 0 {
		c.close(pb.Close_REASON_INVALID_FILENAME,
			"The filename length may not be zero [#%d]", request.id)
		return
	}
	// Only look at the type and subtype, not the parameters:
	// https://www.w3.org/Protocols/rfc1341/4_Content-Type.html
	content_type := strings.Split(header.ContentType, CONTENT_TYPE_SEP)[0]
	is_allowed := false
	for _, allowed := range c.config.Constraints.AcceptedContentTypes {
		if content_type == allowed {
			is_allowed = true
			break
		}
	}
	if !is_allowed {
		c.close(pb.Close_REASON_FORBIDDEN_CONTENT_TYPE,
			"The given content type is forbidden [#%d]", request.id)
		return
	}
	request.resetTimeout()
	response := newResponse(request, header)
	request.provideResponse(response)
	// Content size is empty, the request is therefore immediately concluded.
	if header.ContentSize == 0 {
		response.chunks <- []byte{}
		c.completeRequest(request)
	}
}

func (c *clientImpl) onContentChunk(chunk *pb.ContentChunk) {
	request, ok := c.requests[chunk.RequestId]
	if !ok {
		c.close(pb.Close_REASON_INVALID_REQUEST_ID,
			"Content chunk for unknown request ID %d", chunk.RequestId)
		return
	}
	if request.id != chunk.RequestId {
		panic("inconsistent request IDs")
	}
	if !request.hasResponse() {
		c.close(pb.Close_REASON_INVALID_CLIENT_MESSAGE,
			"Must receive a content header before any chunks [#%d]",
			chunk.RequestId)
		return
	}
	if request.isCompleted() {
		c.close(pb.Close_REASON_INVALID_CLIENT_MESSAGE,
			"Request has already been completed [#%d]",
			chunk.RequestId)
		return
	}
	response := request.getResponse()
	if response == nil {
		panic("stored response cannot be nil")
	}
	expectedSequence, expectedSize, lastChunk := response.nextChunkInfo()
	if chunk.Sequence != expectedSequence {
		c.close(pb.Close_REASON_CONTENT_CHUNK_OUT_OF_SEQUENCE,
			"Content chunk out of sequence, "+
				"expected sequence number %d, got %d [#%d]",
			expectedSequence,
			chunk.Sequence,
			chunk.RequestId)
		return
	}
	if uint64(len(chunk.Data)) != expectedSize {
		c.close(pb.Close_REASON_INVALID_CHUNK_SIZE,
			"Unexpected content chunk size, "+
				"expected %d bytes, got %d [#%d]",
			expectedSize,
			len(chunk.Data),
			chunk.RequestId)
		return
	}
	request.resetTimeout()
	response.chunks <- chunk.Data
	if lastChunk {
		c.completeRequest(request)
	}
}

func (c *clientImpl) completeRequest(request *internalRequest) {
	if request.isCompleted() {
		panic("request already completed")
	}
	request.complete()
	response := request.getResponse()
	if response != nil {
		close(response.chunks)
	}
}

func (c *clientImpl) deleteRequest(request *internalRequest) {
	delete(c.requests, request.id)
}

func (c *clientImpl) nextRequestID() uint64 {
	c.nextRequest++
	return c.nextRequest
}
