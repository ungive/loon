package main

import (
	"bufio"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log"
	"mime"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
	"github.com/ungive/loon/pkg/pb"
	"github.com/ungive/loon/pkg/server"
	"google.golang.org/protobuf/proto"
)

var (
	addr        = flag.String("server", "", "server address")
	contentType = flag.String("type", "", "explicitly set the HTTP content type")
	attachment  = flag.String("download", "", "set the download filename")
	help        = flag.Bool("help", false, "print help")
	stop        = atomic.Bool{}
)

const (
	pingInterval = 20 * time.Second
	pongWait     = 60 * time.Second
	writeWait    = 10 * time.Second
)

func init() {
	log.SetFlags(0)
}

func main() {
	flag.Parse()
	if *help {
		fmt.Println(filepath.Base(os.Args[0]) +
			" -server <address> [options] <path>")
		flag.PrintDefaults()
		return
	}
	if len(*addr) == 0 || flag.NArg() == 0 || len(flag.Arg(0)) == 0 {
		log.Fatalf("invalid arguments")
	}
	path := flag.Arg(0)
	file, err := os.Open(path)
	if err != nil {
		log.Fatalf("failed to open file: %v", err)
	}
	defer file.Close()

	// // TODO: read hello in client
	// // TODO: check that content type is ok
	// // TODO: proper content type from file?

	// source, err := client.NewFileContentSource(file, nil)
	// if err != nil {
	// 	log.Fatalf("failed to create file content source: %v", err)
	// }
	// cli, err := client.NewClient(*addr)
	// if err != nil {
	// 	log.Fatalf("failed to create new client: %v", err)
	// }
	// defer cli.Close()
	// go func() {
	// 	err := cli.Run()
	// 	if err != nil {
	// 		log.Printf("Run() exited with error: %v\n", err)
	// 	} else {
	// 		log.Println("Run() exited")
	// 	}
	// }()
	// var filename *string
	// if len(*attachment) > 0 {
	// 	filename = attachment
	// }
	// content, err := cli.Register(source, &client.ContentInfo{
	// 	Path:               file.Name(),
	// 	AttachmentFilename: filename,
	// 	MaxCacheDuration:   0,
	// 	UploadLimit:        0,
	// })
	// defer cli.Unregister(content)
	// if err != nil {
	// 	log.Fatalf("failed to register content: %v", err)
	// }
	// log.Printf("URL: %v\n", content.URL())

	info, err := file.Stat()
	if err != nil {
		log.Fatalf("failed to stat file: %v", err)
	}
	size := info.Size()
	conn := connect(*addr)
	defer conn.close()
	hello := conn.readHello()
	if uint64(size) > hello.Constraints.MaxContentSize {
		log.Fatalf("server file size limit exceeded: maximum content size"+
			" is %v bytes, got %v bytes, %v bytes over limit",
			hello.Constraints.MaxContentSize, size,
			uint64(size)-hello.Constraints.MaxContentSize)
	}
	fileType := getContentType(file)
	if fileType.HasWildcard() {
		log.Fatalf("the content type may not contain any wildcards: %v",
			fileType)
	}
	contentTypes, err := server.NewContentTypeRegistryFromStrings(
		hello.Constraints.AcceptedContentTypes)
	if err != nil {
		log.Fatalf("failed to interpret allowed content types: %v", err)
	}
	if !contentTypes.Contains(fileType) {
		log.Fatalf("content type not allowed by the server: %v",
			fileType)
	}
	// Handle incoming requests.
	handler := newHandler(conn, hello, file, info, fileType)
	go handler.run()
	// Print the URL under which the file is served.
	log.Println(buildUrl(hello, info.Name()))
	// Block until the user hits CTRL+C
	done := make(chan os.Signal, 1)
	signal.Notify(done, syscall.SIGINT, syscall.SIGTERM)
	<-done
	stop.Store(true)
}

func buildUrl(hello *pb.Hello, path string) string {
	mac, err := server.ComputeMac(
		hello.ClientId, path, hello.ConnectionSecret)
	if err != nil {
		log.Fatalf("failed to compute MAC: %v\n", err)
	}
	macEncoded := mac.UrlEncode()
	result := formatString(
		"{base_url}/{client_id}/{mac}/{path}",
		"base_url", *addr,
		"client_id", hello.ClientId,
		"mac", macEncoded,
		"path", url.PathEscape(path))
	return result
}

type handler struct {
	conn  *clientConn
	hello *pb.Hello
	file  *os.File
	info  fs.FileInfo
	mime  *server.ContentType
}

func newHandler(
	conn *clientConn,
	hello *pb.Hello,
	file *os.File,
	info fs.FileInfo,
	mime *server.ContentType,
) *handler {
	return &handler{
		conn:  conn,
		hello: hello,
		file:  file,
		info:  info,
		mime:  mime,
	}
}

func (h *handler) run() {
	answer := make(chan *pb.Request, 16)
	go func() {
		for {
			request := <-answer
			h.sendContent(request)
		}
	}()
	for {
		message := h.conn.read()
		switch m := message.Data.(type) {
		case *pb.ServerMessage_Request:
			if m.Request.Path != h.info.Name() {
				log.Fatalf("unexpected path in request: %+v", m.Request.Path)
			}
			log.Printf("received request (%v): %v",
				m.Request.Id, m.Request.Path)
			answer <- m.Request
		case *pb.ServerMessage_Success:
		case *pb.ServerMessage_RequestClosed:
			log.Printf("request closed (%v): %v",
				m.RequestClosed.RequestId, m.RequestClosed.Message)
		case *pb.ServerMessage_Close:
			parts := strings.Split(m.Close.Reason.String(), "_")
			for i, part := range parts {
				parts[i] = strings.ToLower(part)
			}
			reason := strings.Join(parts[1:], " ")
			log.Fatalf("connection closed: %v (%v)", m.Close.Message, reason)
		default:
			log.Fatalf("unexpected server message: %T %v", m, m)
		}
	}
}

func (h *handler) sendContent(request *pb.Request) {
	_, err := h.file.Seek(0, io.SeekStart)
	if err != nil {
		log.Fatalf("failed to seek to beginning of file: %v", err)
	}
	var filename *string
	if len(*attachment) > 0 {
		filename = attachment
	}
	h.conn.write(&pb.ClientMessage{
		Data: &pb.ClientMessage_ContentHeader{
			ContentHeader: &pb.ContentHeader{
				RequestId:        request.Id,
				ContentType:      h.mime.String(),
				ContentSize:      uint64(h.info.Size()),
				Filename:         filename,
				MaxCacheDuration: nil,
			},
		},
	})
	reader := bufio.NewReader(h.file)
	buf := make([]byte, h.hello.Constraints.ChunkSize)
	for sequence := uint64(0); ; sequence++ {
		n, err := reader.Read(buf)
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			log.Fatalf("failed to read file: %v", err)
		}
		data := buf[0:n]
		h.conn.write(&pb.ClientMessage{
			Data: &pb.ClientMessage_ContentChunk{
				ContentChunk: &pb.ContentChunk{
					RequestId: request.Id,
					Sequence:  sequence,
					Data:      data,
				},
			},
		})
	}
}

func connect(address string) *clientConn {
	address = strings.TrimSuffix(address, "/")
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
		log.Fatalf("address must either have protocol HTTP or HTTPS: %v", u)
	}
	address = u.String() + "/ws"
	rawConn, _, err := websocket.DefaultDialer.Dial(address, nil)
	if err != nil {
		log.Fatalf("failed to dial websocket at %v: %v", address, err)
	}
	log.Printf("connected to %v", address)
	rawConn.SetPongHandler(func(string) error {
		rawConn.SetReadDeadline(time.Now().Add(pongWait))
		return nil
	})
	conn := &clientConn{
		conn: rawConn,
		send: make(chan []byte),
		ping: make(chan struct{}),
	}
	go func() {
		ticker := time.NewTicker(pingInterval)
		for range ticker.C {
			conn.ping <- struct{}{}
		}
	}()
	go conn.writePump()
	return conn
}

type clientConn struct {
	conn *websocket.Conn
	send chan []byte
	ping chan struct{}
}

func (c *clientConn) writePump() {
	defer c.conn.Close()
	for {
		select {
		case <-c.ping:
			c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			err := c.conn.WriteMessage(websocket.PingMessage, []byte{})
			if err != nil {
				if stop.Load() {
					return
				}
				log.Fatalf("failed to write ping message: %v", err)
			}
		case data := <-c.send:
			c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			err := c.conn.WriteMessage(websocket.BinaryMessage, data)
			if err != nil {
				if stop.Load() {
					return
				}
				log.Fatalf("failed to write client message: %v", err)
			}
		}
	}
}

func (c *clientConn) write(message *pb.ClientMessage) {
	data, err := proto.Marshal(message)
	if err != nil {
		log.Fatalf("failed to marshal client message: %v", err)
	}
	c.send <- data
}

func (c *clientConn) read() *pb.ServerMessage {
	typ, data, err := c.conn.ReadMessage()
	if err != nil {
		if stop.Load() {
			os.Exit(0)
		}
		log.Fatalf("failed to read message from server: %v", err)
	}
	if typ != websocket.BinaryMessage {
		log.Fatalf("received an unexpected message type: %v", typ)
	}
	msg := &pb.ServerMessage{}
	err = proto.Unmarshal(data, msg)
	if err != nil {
		log.Fatalf("failed to unmarshal server message: %v", err)
	}
	return msg
}

func (c *clientConn) readHello() *pb.Hello {
	return getServerMessage[pb.ServerMessage_Hello](c.read()).Hello
}

func (c *clientConn) close() {
	c.conn.Close()
}

func getServerMessage[T interface{}, P *T](message *pb.ServerMessage) P {
	switch m := message.Data.(type) {
	case P:
		return m
	default:
		log.Fatalf("unexpected server message: %T %v", m, m)
	}
	panic("unreachable")
}

func getContentType(file *os.File) *server.ContentType {
	if len(*contentType) > 0 {
		result := parseContentType(*contentType)
		log.Printf("using explicit content type: %v\n", result)
		return result
	}
	extType := mime.TypeByExtension(filepath.Ext(file.Name()))
	if len(extType) > 0 {
		result := parseContentType(extType)
		log.Printf("detected content type from extension: %v\n", result)
		return result
	}
	_, err := file.Seek(0, io.SeekStart)
	if err != nil {
		log.Fatalf("failed to seek to beginning of file: %v", err)
	}
	reader := bufio.NewReader(file)
	buf := make([]byte, 512)
	n, err := reader.Read(buf)
	if errors.Is(err, io.EOF) {
		log.Fatalf("cannot detect content type of empty file")
	}
	if err != nil {
		log.Fatalf("failed to read file: %v", err)
	}
	data := buf[0:n]
	result := parseContentType(http.DetectContentType(data))
	log.Printf("detected content type from content: %v\n", result)
	return result
}

func parseContentType(value string) *server.ContentType {
	result, err := server.NewContentType(value)
	if err != nil {
		log.Fatalf("failed to parse content type %v: %v", value, err)
	}
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
