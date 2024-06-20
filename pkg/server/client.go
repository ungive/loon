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
	"mime"
	"net/url"
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
	MAC_KEY_SIZE = 64
)

var (
	ErrClientClosed        = errors.New("client connection closed")
	ErrBadPath             = errors.New("the path is malformed")
	ErrBadQuery            = errors.New("the query is malformed")
	ErrBadMac              = errors.New("the MAC hash is invalid")
	ErrRequestDeleted      = errors.New("the request does not exist")
	ErrRequestClosed       = errors.New("the request is closed")
	ErrRequestCompleted    = errors.New("the request is already completed")
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
	// Closes the client, if it isn't already closed, and exits the run loop.
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
	// Returns the request this response is associated with.
	Request() Request
	// Returns the content header for this response.
	Header() *pb.ContentHeader
	// Returns the channel that supplies the sender's chunks.
	// The returned channel is closed if and only if
	// the response has been fully received.
	Chunks() <-chan []byte
}

// Note that objects of this type are owned by the protocol() run loop,
// but instances of it are passed to outside callers.
// Public methods must therefore be thread-safe,
// since they could be called from multiple goroutines.
// Internal methods do not need to be thread-safe,
// as calls to them are synchronized within the protocol() method.
type internalRequest struct {
	client *clientImpl
	id     uint64
	path   string
	query  string

	pendingResponse chan Response
	response        *internalResponse
	responseMutex   sync.Mutex

	lastUpdated time.Time
	completed   chan struct{}
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
		pendingResponse: make(chan Response, 1),
		response:        nil,
		lastUpdated:     time.Now(),
		completed:       make(chan struct{}),
		closed:          make(chan struct{}),
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

func (r *internalRequest) Response() <-chan Response {
	return r.pendingResponse
}

func (r *internalRequest) Success() error {
	if chanClosed(r.client.done) {
		return ErrClientClosed
	}
	if chanClosed(r.closed) {
		return ErrRequestClosed
	}
	outErr := make(chan error)
	select {
	case r.client.triggerSuccess <- &forwardSuccess{
		request: r,
		outErr:  outErr,
	}:
	case <-r.closed:
		return ErrRequestClosed
	case <-r.client.done:
		return ErrClientClosed
	}
	return <-outErr
}

func (r *internalRequest) Completed() <-chan struct{} {
	return r.completed
}

func (r *internalRequest) Closed() <-chan struct{} {
	return r.closed
}

func (r *internalRequest) Close(message string) error {
	if chanClosed(r.closed) || chanClosed(r.client.done) {
		return nil
	}
	if chanClosed(r.completed) {
		return ErrRequestCompleted
	}
	// Critical during heavy load:
	// The protocol loop for this client can get stuck at writing chunks,
	// when the chunk buffer is filled and sending to the chunk channel blocks.
	// Before sending a value to "triggerClose", which requires the protocol
	// loop to be ready, we need to make space for further chunks and
	// immediately discard them, until the request has been closed.
	r.responseMutex.Lock()
	response := r.response
	r.responseMutex.Unlock()
	if response != nil {
		select {
		case r.client.discardChunks <- &forwardDiscard{
			chunks: response.chunks,
			done:   r.closed,
		}:
		case <-r.completed:
			return ErrRequestCompleted
		case <-r.closed:
			return nil
		case <-r.client.done:
			return nil
		}
	}
	outErr := make(chan error)
	select {
	case r.client.triggerClose <- &forwardClose{
		request: r,
		message: message,
		outErr:  outErr,
	}:
	case <-r.completed:
		return ErrRequestCompleted
	case <-r.closed:
		return nil
	case <-r.client.done:
		return nil
	}
	return <-outErr
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
	r.responseMutex.Lock()
	if r.response != nil {
		panic("response already provided")
	}
	r.response = response
	r.responseMutex.Unlock()
	if !r.isClosed() {
		// Only forward the response if the request is not closed.
		r.pendingResponse <- response
		close(r.pendingResponse)
	}
}

// Check if this request has a response,
// i.e. if a response was provided via provideResponse().
func (r *internalRequest) hasResponse() bool {
	// No need to lock, since private methods are synchronized
	return r.response != nil
}

// Get the internal response.
// Check hasResponse() before using the result of this function.
func (r *internalRequest) getResponse() *internalResponse {
	// No need to lock, since private methods are synchronized
	return r.response
}

// Marks this request as closed, if it isn't already
func (r *internalRequest) close() {
	if r.isCompleted() {
		return
	}
	if r.isClosed() {
		return
	}
	close(r.closed)
}

// Marks this request as completed.
func (r *internalRequest) complete() {
	if r.isCompleted() {
		panic("the request has already been completed")
	}
	if r.isClosed() {
		panic("cannot complete a closed request")
	}
	close(r.completed)
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

// Checks if this request has been marked as completed with complete().
func (r *internalRequest) isCompleted() bool {
	select {
	case <-r.completed:
		return true
	default:
		return false
	}
}

type internalResponse struct {
	request       *internalRequest
	header        *pb.ContentHeader
	chunkSequence uint64
	chunks        chan []byte
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
	}
}

func (r *internalResponse) Request() Request {
	return r.request
}

func (r *internalResponse) Header() *pb.ContentHeader {
	return r.header
}

func (r *internalResponse) Chunks() <-chan []byte {
	return r.chunks
}

func (r *internalResponse) write(chunk []byte) {
	if !r.request.isClosed() {
		// Only forward a chunk if the request is not closed.
		r.chunks <- chunk
	}
}

func (r *internalResponse) done() {
	if !r.request.isClosed() {
		// Only signal that all chunks have been received,
		// if the request is not closed.
		close(r.chunks)
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
	path   string
	query  string
	out    chan Request
	outErr chan error
}

type forwardSuccess struct {
	request *internalRequest
	outErr  chan error
}

type forwardClose struct {
	request *internalRequest
	message string
	outErr  chan error
}

type forwardDiscard struct {
	chunks <-chan []byte
	done   <-chan struct{}
}

type clientImpl struct {
	id           UUID                // ownership: read-only
	idStr        string              // ownership: read-only
	secret       []byte              // ownership: read-only
	config       *ClientConfig       // ownership: read-only
	contentTypes map[string]struct{} // ownership: protocol()
	dirty        atomic.Bool

	conn    *websocket.Conn // ownership: read: readPump() write: writePump()
	recv    chan *pb.ClientMessage
	send    chan *pb.ServerMessage
	stop    chan *pb.Close
	done    chan struct{}
	runDone chan struct{}

	requests      map[uint64]*internalRequest // ownership: protocol()
	countRequests chan chan int
	nextRequest   uint64 // ownership: protocol()

	forwardRequest chan *forwardRequest
	triggerSuccess chan *forwardSuccess
	triggerClose   chan *forwardClose
	discardChunks  chan *forwardDiscard
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

func (c *ClientConfig) validate() error {
	if len(c.BaseUrl) == 0 {
		return errors.New("base URL cannot be empty")
	}
	if c.Intervals.WriteWait <= 0 {
		return errors.New("write wait must be greater than zero")
	}
	if c.Intervals.PongWait <= 0 {
		return errors.New("pong wait must be greater than zero")
	}
	if c.Intervals.PingInterval <= 0 {
		return errors.New("ping interval must be greater than zero")
	}
	if c.Intervals.TimeoutDuration <= 0 {
		return errors.New("timeout duration must be greater than zero")
	}
	if c.Intervals.TimeoutInterval <= 0 {
		return errors.New("timeout interval must be greater than zero")
	}
	if c.Constraints.ChunkSize > c.Constraints.MaxContentSize {
		return errors.New(
			"chunk size cannot be larger than maximum content size")
	}
	if len(c.Constraints.AcceptedContentTypes) == 0 {
		return errors.New("accepted content types cannot be empty")
	}
	for _, contentType := range c.Constraints.AcceptedContentTypes {
		sanitized, params, err := mime.ParseMediaType(contentType)
		if err != nil {
			return fmt.Errorf("invalid accepted content type: %w", err)
		}
		if len(params) > 0 {
			return errors.New("content type may not contain parameters")
		}
		if contentType != sanitized {
			return errors.New(
				"content type must be lowercase and contain no spaces")
		}
	}
	return nil
}

func NewClient(conn *websocket.Conn, config *ClientConfig) (Client, error) {
	err := config.validate()
	if err != nil {
		return nil, err
	}
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
		idStr:          id.String(),
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
		discardChunks:  make(chan *forwardDiscard),
		contentTypes:   make(map[string]struct{}),
	}
	for _, key := range client.config.Constraints.AcceptedContentTypes {
		client.contentTypes[key] = struct{}{}
	}
	// Make sure the first message that we send is a Hello message,
	// by putting it into the buffer before the caller can call Run().
	client.send <- &pb.ServerMessage{
		Data: &pb.ServerMessage_Hello{
			Hello: &pb.Hello{
				Constraints:      client.config.Constraints,
				ClientId:         client.idStr,
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
	wg.Add(4)
	go func() { c.writePump(); wg.Done() }()
	go func() { c.readPump(); wg.Done() }()
	go func() { c.protocol(); wg.Done() }()
	go func() { c.chunkDiscarder(); wg.Done() }()
	wg.Wait()
	close(c.runDone)
}

func (c *clientImpl) ID() UUID {
	return c.id.Clone()
}

func (c *clientImpl) Request(path string, query string, mac []byte) (Request, error) {
	if len(path) == 0 {
		return nil, ErrBadPath
	}
	if len(query) > 0 {
		_, err := url.ParseQuery(query)
		if err != nil {
			return nil, errors.Join(ErrBadQuery, err)
		}
	}
	path = strings.TrimSpace(strings.TrimLeft(path, "/"))
	query = strings.TrimSpace(strings.TrimLeft(query, "?"))
	computedMac, err := c.computeMac(path, query)
	if err != nil {
		return nil, err
	}
	if !bytes.Equal(mac, computedMac) {
		return nil, ErrBadMac
	}
	out := make(chan Request)
	outErr := make(chan error)
	select {
	case c.forwardRequest <- &forwardRequest{
		path:   path,
		query:  query,
		out:    out,
		outErr: outErr,
	}:
	case <-c.done:
		return nil, ErrClientClosed
	}
	select {
	case request := <-out:
		return request, nil
	case err = <-outErr:
		return nil, err
	}
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

func ComputeMac(
	clientId string,
	clientSecret []byte,
	path string,
	query string,
) ([]byte, error) {
	path = strings.TrimLeft(path, "/")
	query = strings.TrimLeft(query, "?")
	mac := hmac.New(sha256.New, clientSecret)
	items := [][]byte{
		[]byte(clientId),
		[]byte("/"),
		[]byte(path),
	}
	if len(query) > 0 {
		items = append(items, []byte("?"))
		items = append(items, []byte(query))
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

func (c *clientImpl) computeMac(path string, query string) ([]byte, error) {
	return ComputeMac(c.idStr, c.secret, path, query)
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

// Runs in the background to discard any buffered response chunks.
func (c *clientImpl) chunkDiscarder() {
	for {
		select {
		case info := <-c.discardChunks:
		discard:
			for {
				select {
				case _, ok := <-info.chunks:
					if !ok {
						continue discard
					}
				case <-info.done:
					break discard
				}
			}
		case <-c.done:
			return
		}
	}
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
		// Close all requests when the protocol ends.
		for _, request := range c.requests {
			request.close()
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
			request, err := c.sendRequest(info.path, info.query)
			if err != nil {
				info.outErr <- err
			} else {
				info.out <- request
			}
		case info := <-c.triggerSuccess:
			info.outErr <- c.sendSuccess(info.request)
		case info := <-c.triggerClose:
			info.outErr <- c.closeRequest(info.request, info.message)
		case out := <-c.countRequests:
			out <- len(c.requests)
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
		if request.isCompleted() {
			// The request is completed, but it has not been deleted yet,
			// which it would have, if a Success message had been sent.
			// In that case we just delete it silently.
			c.deleteRequest(request)
			continue
		}
		// Ignore any error, just make sure it's closed.
		_ = c.closeRequest(request, "request timed out")
	}
}

// Sends a message to the connected websocket client.
// Returns false if the client has been closed.
func (c *clientImpl) sendMessage(message *pb.ServerMessage) bool {
	select {
	case c.send <- message:
		return true
	case <-c.done:
		return false
	}
}

func (c *clientImpl) sendRequest(
	path string,
	query string,
) (*internalRequest, error) {
	var id uint64
	for {
		id = c.nextRequestID()
		if _, ok := c.requests[id]; !ok {
			break
		}
	}
	request := newRequest(c, id, path, query)
	c.requests[id] = request
	ok := c.sendMessage(&pb.ServerMessage{
		Data: &pb.ServerMessage_Request{
			Request: &pb.Request{
				Id:        id,
				Path:      path,
				Query:     query,
				Timestamp: timestamppb.Now(),
			},
		},
	})
	if ok {
		return request, nil
	} else {
		return nil, ErrClientClosed
	}
}

func (c *clientImpl) sendSuccess(request *internalRequest) error {
	request, ok := c.requests[request.id]
	if !ok {
		return ErrRequestDeleted
	}
	if request.isClosed() {
		return ErrRequestClosed
	}
	if !request.isCompleted() {
		return ErrRequestNotCompleted
	}
	ok = c.sendMessage(&pb.ServerMessage{
		Data: &pb.ServerMessage_Success{
			Success: &pb.Success{
				RequestId: request.id,
			},
		},
	})
	if !ok {
		return ErrClientClosed
	}
	c.deleteRequest(request)
	return nil
}

func (c *clientImpl) closeRequest(request *internalRequest, message string) error {
	request, ok := c.requests[request.id]
	if !ok {
		return ErrRequestDeleted
	}
	if request.isClosed() {
		return nil
	}
	if request.isCompleted() {
		return ErrRequestCompleted
	}
	ok = c.sendMessage(&pb.ServerMessage{
		Data: &pb.ServerMessage_RequestClosed{
			RequestClosed: &pb.RequestClosed{
				RequestId: request.id,
				Message:   message,
			},
		},
	})
	if !ok {
		return ErrClientClosed
	}
	// Reset the timeout and mark the request as closed.
	// This will make sure the timeout is not reset anymore after this
	// and since we are not deleting the request from the internal map,
	// the client has to send a CloseResponse message in time.
	// But since we are resetting the timeout one last time,
	// the client gets another timeout period of time
	// before it has to answer with a CloseResponse message.
	request.resetTimeout()
	request.close()
	return nil
}

func (c *clientImpl) onCloseResponse(info *pb.CloseResponse) {
	request, ok := c.requests[info.RequestId]
	if !ok {
		c.close(pb.Close_REASON_INVALID_REQUEST_ID,
			"Closed response for unknown request ID %d", info.RequestId)
		return
	}
	if !request.hasResponse() {
		c.close(pb.Close_REASON_INVALID_CLIENT_MESSAGE,
			"Must receive a response before closing it [#%d]", request.id)
		return
	}
	if request.isCompleted() {
		c.close(pb.Close_REASON_INVALID_CLIENT_MESSAGE,
			"Cannot close a response that is already completed [#%d]", request.id)
		return
	}
	request.close()
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
	content_type := strings.Split(header.ContentType, CONTENT_TYPE_PARAM_SEP)[0]
	_, is_allowed := c.contentTypes[content_type]
	if !is_allowed {
		c.close(pb.Close_REASON_FORBIDDEN_CONTENT_TYPE,
			"The given content type is forbidden [#%d]", request.id)
		return
	}
	request.resetTimeout()
	response := newResponse(request, header)
	request.provideResponse(response)
	// Content size is empty, the request is therefore immediately completed.
	if header.ContentSize == 0 {
		response.write([]byte{})
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
	response.write(chunk.Data)
	if lastChunk {
		c.completeRequest(request)
	}
}

// Marks the request as completed and closes the response's chunk channel,
// in case a response was sent. Panics if the request is already completed.
// Should only be called if:
// the client sent an EmptyResponse, or
// the client sent a ContentHeader with content length 0, or
// the client sent the last ContentChunk.
func (c *clientImpl) completeRequest(request *internalRequest) {
	if request.isCompleted() {
		panic("request already completed")
	}
	if request.isClosed() {
		// A closed request whose response was fully received
		// does not need to be marked as completed anymore,
		// but instead should be deleted from the internal map.
		c.deleteRequest(request)
		return
	}
	request.complete()
	response := request.getResponse()
	if response != nil {
		response.done()
	}
}

// Deletes the request from the internal map.
// Should only be called if:
// the request is completed and the Success message has been sent, or
// the request is completed and the request has timed out, or
// the request is not completed and the client closed the response, or
// the client sent an EmptyResponse.
func (c *clientImpl) deleteRequest(request *internalRequest) {
	delete(c.requests, request.id)
}

func (c *clientImpl) nextRequestID() uint64 {
	c.nextRequest++
	return c.nextRequest
}

func chanClosed[T interface{}](c <-chan T) bool {
	select {
	case <-c:
		return true
	default:
		return false
	}
}
