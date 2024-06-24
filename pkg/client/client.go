package client

import (
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/ungive/loon/pkg/pb"
	"github.com/ungive/loon/pkg/server"
	"google.golang.org/protobuf/proto"
)

type Client interface {

	// Run loop for the client.
	Run() error

	// Registers content with this client and returns a handle for it.
	// The content is available until it is unregistered with Unregister.
	// Calls to any of the methods of the source are synchronized.
	// The source's Data() method is only called
	// while no requests are being handled that are using
	// a previous return value of it.
	// If the source reads from a file e.g.,
	// the Data() method can safely seek to the beginning
	// without having to worry about corrupting other ongoing requests.
	Register(source ContentSource, info *ContentInfo) (ContentHandle, error)

	// Unregisters content from this client.
	// The client will not respond with the data anymore,
	// if the server makes a request for it.
	// Content may still be cached on the server or elsewhere.
	// The Close() method will be called on the ContentSource instance
	// that is associated with the passed ContentHandle.
	Unregister(handle ContentHandle) error

	// Closes the client's connection to the server.
	// Calls Close() on any registered ContentSource objects.
	Close()
}

type clientImpl struct {
	baseUrl string
	conn    *websocket.Conn
	hello   *pb.Hello
	content map[server.UUID]*contentHandle
	forPath map[string]*contentHandle

	send    chan []byte
	ping    chan struct{}
	stop    chan struct{}
	done    chan struct{}
	lastErr chan error

	register   chan *contentHandle
	unregister chan *contentHandle
}

type contentHandle struct {
	client *clientImpl
	id     server.UUID
	url    string
	src    ContentSource
	info   *ContentInfo
	serve  chan *pb.Request
	cancel chan struct{}
}

func (h *contentHandle) URL() string {
	return h.url
}

func (h *contentHandle) responsePump() {
	for {
		select {
		case request := <-h.serve:
			h.respond(request)
		case <-h.client.done:
			return
		}
	}
}

func (h *contentHandle) respond(request *pb.Request) {
	cancel := make(chan struct{})
	done := make(chan struct{})
	go func() {
		select {
		case <-h.cancel:
			close(cancel)
		case <-done:
		case <-h.client.done:
		}
	}()
	defer func() {
		close(done)
		select {
		case <-cancel:
			h.client.write(&pb.ClientMessage{
				Data: &pb.ClientMessage_CloseResponse{
					CloseResponse: &pb.CloseResponse{
						RequestId: request.Id,
					},
				},
			})
		default:
		}
	}()
	reader, err := h.src.Data()
	if err != nil {
		log.Printf("failed to get reader for source: %v\n", err)
		close(cancel)
		return
	}
	err = h.client.write(&pb.ClientMessage{
		Data: &pb.ClientMessage_ContentHeader{
			ContentHeader: &pb.ContentHeader{
				RequestId:        request.Id,
				ContentType:      h.src.ContentType(),
				ContentSize:      uint64(h.src.Size()),
				Filename:         h.info.AttachmentFilename,
				MaxCacheDuration: &h.info.MaxCacheDuration,
			},
		},
	})
	if err != nil {
		log.Printf("failed to write response: %v\n", err)
		close(cancel)
		return
	}
	chunkSize := h.client.hello.Constraints.ChunkSize
	buf := make([]byte, chunkSize)
	for sequence := uint64(0); ; sequence++ {
		sent := sequence * chunkSize
		if sent >= h.src.Size() {
			break
		}
		toRead := min(chunkSize, h.src.Size()-sent)
		select {
		case <-cancel:
			return
		default:
		}
		_, err := io.ReadAtLeast(reader, buf, int(toRead))
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			log.Printf("failed to read content data: %v\n", err)
			close(cancel)
			return
		}
		data := buf[0:toRead]
		err = h.client.write(&pb.ClientMessage{
			Data: &pb.ClientMessage_ContentChunk{
				ContentChunk: &pb.ContentChunk{
					RequestId: request.Id,
					Sequence:  sequence,
					Data:      data,
				},
			},
		})
		if err != nil {
			log.Printf("failed to write response: %v\n", err)
			close(cancel)
			return
		}
	}
}

var ErrClientClosed = errors.New("the Client is closed")
var ErrInvalidScheme = errors.New("invalid scheme in URL")
var ErrNoHello = errors.New("server did not send a Hello message")

const (
	pingInterval  = 20 * time.Second
	pongWait      = 60 * time.Second
	writeWait     = 10 * time.Second
	websocketPath = "/ws"
	requestBuffer = 16
)

func websocketUrlFor(baseUrl string) (string, error) {
	address := strings.TrimSuffix(baseUrl, "/")
	u, err := url.Parse(address)
	if err != nil {
		log.Fatalf("failed to parse address: %v", err)
	}
	switch u.Scheme {
	case "http":
		u.Scheme = "ws"
	case "https":
		u.Scheme = "wss"
	default:
		return "", fmt.Errorf("base URL must be HTTP or HTTPS: %v", u)
	}
	return u.String() + "/ws", nil
}

func verifyBaseUrl(connectedWith string, helloContains string) error {
	a, err := url.Parse(connectedWith)
	if err != nil {
		return fmt.Errorf("failed to parse URL %v: %v", connectedWith, err)
	}
	b, err := url.Parse(helloContains)
	if err != nil {
		return fmt.Errorf("failed to parse URL %v: %v", helloContains, err)
	}
	aAddrs, err := net.LookupHost(a.Hostname())
	if err != nil {
		return fmt.Errorf("failed to lookup host %v: %v", a.Hostname(), err)
	}
	bAddrs, err := net.LookupHost(b.Hostname())
	if err != nil {
		return fmt.Errorf("failed to lookup host %v: %v", b.Hostname(), err)
	}
	aMap := make(map[string]struct{})
	for _, aAddr := range aAddrs {
		aMap[aAddr] = struct{}{}
	}
	for _, bAddr := range bAddrs {
		if _, ok := aMap[bAddr]; ok {
			return nil
		}
	}
	return fmt.Errorf("could not verify the server's base URL: "+
		"%v and %v do not point to the same host", connectedWith, helloContains)
}

func NewClient(baseUrl string, httpBasicAuth *string) (Client, error) {
	address, err := websocketUrlFor(baseUrl)
	if err != nil {
		return nil, err
	}
	var headers http.Header
	if httpBasicAuth != nil {
		auth := base64.StdEncoding.EncodeToString([]byte(*httpBasicAuth))
		headers = http.Header{"Authorization": {"Basic " + auth}}
	}
	conn, _, err := websocket.DefaultDialer.Dial(address, headers)
	if err != nil {
		log.Fatalf("failed to dial websocket at %v: %v", address, err)
	}
	conn.SetPongHandler(func(string) error {
		conn.SetReadDeadline(time.Now().Add(pongWait))
		return nil
	})
	client := &clientImpl{
		baseUrl:    baseUrl,
		conn:       conn,
		hello:      nil,
		content:    make(map[server.UUID]*contentHandle),
		forPath:    make(map[string]*contentHandle),
		send:       make(chan []byte),
		ping:       make(chan struct{}),
		stop:       make(chan struct{}),
		done:       make(chan struct{}),
		lastErr:    make(chan error, 1),
		register:   make(chan *contentHandle),
		unregister: make(chan *contentHandle),
	}
	message, err := client.read()
	if err != nil {
		return nil, err
	}
	switch m := message.Data.(type) {
	case *pb.ServerMessage_Hello:
		// Make sure to verify that the base URL points to the same server.
		err := verifyBaseUrl(baseUrl, m.Hello.BaseUrl)
		if err != nil {
			return nil, err
		}
		client.baseUrl = m.Hello.BaseUrl
		client.hello = m.Hello
	default:
		return nil, ErrNoHello
	}
	return client, nil
}

func (c *clientImpl) Run() error {
	var wg sync.WaitGroup
	wg.Add(2)
	go func() { c.writePump(); wg.Done() }()
	go func() { c.readPump(); wg.Done() }()
	wg.Wait()
	return <-c.lastErr
}

func (c *clientImpl) Close() {
	select {
	case c.lastErr <- ErrClientClosed:
	default:
	}
	select {
	case c.stop <- struct{}{}:
	case <-c.done:
		return
	}
}

func (c *clientImpl) Register(
	source ContentSource,
	info *ContentInfo,
) (ContentHandle, error) {
	uuid, err := server.NewUUID()
	if err != nil {
		return nil, err
	}
	handle := &contentHandle{
		client: c,
		id:     uuid,
		url:    c.buildUrl(info.Path),
		src:    source,
		info:   info,
		serve:  make(chan *pb.Request, requestBuffer),
		cancel: make(chan struct{}),
	}
	select {
	case c.register <- handle:
	case <-c.done:
		return nil, ErrClientClosed
	}
	go handle.responsePump()
	return handle, nil
}

func (c *clientImpl) Unregister(handle ContentHandle) error {
	select {
	case c.unregister <- handle.(*contentHandle):
	case <-c.done:
		return ErrClientClosed
	}
	return nil
}

func (c *clientImpl) report(err error) {
	select {
	case c.lastErr <- err:
	default:
	}
}

func (c *clientImpl) writePump() {
	ticker := time.NewTicker(pingInterval)
	defer func() {
		close(c.done)
		for _, content := range c.content {
			content.src.Close()
		}
		ticker.Stop()
		c.conn.WriteMessage(websocket.CloseMessage, []byte{})
		c.conn.Close()
	}()
	for {
		select {
		case data := <-c.send:
			c.conn.SetWriteDeadline(time.Now().Add(writeWait))

			// switch m := message.Data.(type) {
			// case *pb.ClientMessage_ContentChunk:
			// 	hash := sha256.New()
			// 	hash.Write(m.ContentChunk.Data)
			// 	bs := hash.Sum(nil)
			// 	log.Println(len(m.ContentChunk.Data), hex.EncodeToString(bs))
			// }

			err := c.conn.WriteMessage(websocket.BinaryMessage, data)
			if err != nil {
				c.report(err)
				return
			}
		case <-ticker.C:
			c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			err := c.conn.WriteMessage(websocket.PingMessage, []byte{})
			if err != nil {
				c.report(err)
				return
			}
		case content := <-c.register:
			if _, ok := c.content[content.id]; ok {
				panic("content ID already registered")
			}
			c.content[content.id] = content
			c.forPath[content.info.Path] = content
		case content := <-c.unregister:
			if _, ok := c.content[content.id]; ok {
				delete(c.content, content.id)
				delete(c.forPath, content.info.Path)
				content.src.Close()
			}
		case <-c.stop:
			return
		}
	}
}

func (c *clientImpl) readPump() {
	defer func() {
		select {
		case c.stop <- struct{}{}:
		case <-c.done:
		}
	}()
	requests := make(map[uint64]*pb.Request)
	for {
		message, err := c.read()
		select {
		case <-c.done:
			return
		default:
		}
		if err != nil {
			c.report(err)
			return
		}
		switch m := message.Data.(type) {
		case *pb.ServerMessage_Hello:
			c.report(errors.New("unexpected Hello message from server"))
			return
		case *pb.ServerMessage_Request:
			requests[m.Request.Id] = m.Request
			c.handleRequest(m.Request)
		case *pb.ServerMessage_Success:
			delete(requests, m.Success.RequestId)
			// TODO: report to handle
			log.Printf("response for request %v was successfully forwarded\n",
				m.Success.RequestId)
		case *pb.ServerMessage_RequestClosed:
			id := m.RequestClosed.RequestId
			log.Printf("request closed (%v): %v\n", id, m.RequestClosed.Message)
			request, ok := requests[id]
			if !ok {
				panic(fmt.Sprintf("request not registered: %v", id))
			}
			delete(requests, id)
			c.cancelResponse(request)
		case *pb.ServerMessage_Close:
			parts := strings.Split(m.Close.Reason.String(), "_")
			c.report(fmt.Errorf("connection closed: %v (%v)",
				m.Close.Message, strings.Join(parts[1:], "_")))
			return
		default:
			c.report(fmt.Errorf("unexpected server message: %T %v", m, m))
			return
		}
		select {
		case <-c.done:
			return
		default:
		}
	}
}

func (c *clientImpl) handleRequest(request *pb.Request) {
	handle, ok := c.forPath[request.Path]
	if !ok {
		c.write(&pb.ClientMessage{
			Data: &pb.ClientMessage_EmptyResponse{
				EmptyResponse: &pb.EmptyResponse{
					RequestId: request.Id,
				},
			},
		})
		return
	}
	select {
	case handle.serve <- request:
	case <-c.done:
		return
	}
}

func (c *clientImpl) cancelResponse(request *pb.Request) {
	handle, ok := c.forPath[request.Path]
	if !ok {
		return
	}
	select {
	case handle.cancel <- struct{}{}:
	case <-c.done:
	}
}

func (c *clientImpl) write(message *pb.ClientMessage) error {
	data, err := proto.Marshal(message)
	if err != nil {
		return fmt.Errorf("failed to marshal client message: %v", err)
	}
	select {
	case c.send <- data:
	case <-c.done:
		return ErrClientClosed
	}
	return nil
}

func (c *clientImpl) read() (*pb.ServerMessage, error) {
	typ, data, err := c.conn.ReadMessage()
	if err != nil {
		return nil, err
	}
	if typ != websocket.BinaryMessage {
		return nil, fmt.Errorf("received an unexpected message type: %v", typ)
	}
	msg := &pb.ServerMessage{}
	err = proto.Unmarshal(data, msg)
	if err != nil {
		return nil, fmt.Errorf("failed to unmarshal server message: %v", err)
	}
	return msg, nil
}

func (c *clientImpl) buildUrl(path string) string {
	mac, err := server.ComputeMac(
		c.hello.ClientId, path, c.hello.ConnectionSecret)
	if err != nil {
		log.Fatalf("failed to compute MAC: %v\n", err)
	}
	macEncoded := mac.UrlEncode()
	result := formatString(
		"{base_url}/{client_id}/{mac}/{path}",
		"base_url", c.baseUrl,
		"client_id", c.hello.ClientId,
		"mac", macEncoded,
		"path", url.PathEscape(path))
	return result
}

// https://stackoverflow.com/a/40811635/6748004
func formatString(format string, args ...string) string {
	for i, v := range args {
		if i%2 == 0 {
			args[i] = "{" + v + "}"
		}
	}
	return strings.NewReplacer(args...).Replace(format)
}

// ---

// func main() {
// 	file, err := os.Open("image.jpg")
// 	if err != nil {
// 		log.Fatalf("failed to open file: %v", err)
// 	}
// 	defer file.Close()
// 	contentType, err := server.NewContentType("image/jpeg")
// 	if err != nil {
// 		log.Fatalf("invalid explicit content type: %v", err)
// 	}
// 	source, err := NewFileContentSource(file, contentType)
// 	if err != nil {
// 		log.Fatalf("failed to create file content source: %v", err)
// 	}
// 	client, err := NewClient()
// 	if err != nil {
// 		log.Fatalf("failed to create new client: %v", err)
// 	}
// 	content, err := client.Register(source, &ContentInfo{
// 		Path:               "P1ml0UXFpCIW",
// 		AttachmentFilename: nil,
// 		MaxCacheDuration:   0,
// 		UploadLimit:        0,
// 	})
// 	defer client.Unregister(content)
// 	if err != nil {
// 		log.Fatalf("failed to register content: %v", err)
// 	}
// 	log.Printf("URL: %v\n", content.URL())
// 	time.Sleep(30 * time.Second)
// }
