package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"flag"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
	"github.com/ungive/loon/pkg/pb"
	"google.golang.org/protobuf/proto"
)

// CLI interface (future):
// loon server localhost:8080  # serves http and websocket server on port 8080
// loon config server http://localhost:8080  # sets server in config
// loon online ./example.txt  # serves a single file with the configured server

// CLI interface (test):
// loon-client --addr http://localhost:8080 ./example.txt

const (
	urlFormat    = "{base_url}/{client_id}/{mac}/{path}"
	pingInterval = 20 * time.Second
	pongWait     = 60 * time.Second
	writeWait    = 10 * time.Second
)

var addr = flag.String("addr", "http://localhost:8080", "server address")

func main() {
	flag.Parse()
	conn := dial()
	hello := conn.readHello()
	handler := newHandler(conn, hello)
	go handler.run()

	log.Printf("Hello: %v", hello)
	log.Printf("URL: %v", makeUrl(hello, "example.txt", ""))

	// Block until the user hits CTRL+C
	done := make(chan os.Signal, 1)
	signal.Notify(done, syscall.SIGINT, syscall.SIGTERM)
	<-done
}

type handler struct {
	conn  *clientConn
	hello *pb.Hello
}

func newHandler(conn *clientConn, hello *pb.Hello) *handler {
	return &handler{
		conn:  conn,
		hello: hello,
	}
}

func (h *handler) run() {
	for {
		message := h.conn.read()
		log.Printf("recv: %T %v\n", message.Data, message)
		switch value := message.Data.(type) {
		case *pb.ServerMessage_Request:
			h.handleRequest(value.Request)
		case *pb.ServerMessage_Success:
			log.Printf("successfully forwarded request %v!\n",
				value.Success.RequestId)
		default:
			log.Fatalf("unexpected server message: %T %v", value, value)
		}
	}
}

func (h *handler) handleRequest(request *pb.Request) {
	testPayload := []byte("<h1>It works!")
	h.conn.write(&pb.ClientMessage{
		Data: &pb.ClientMessage_ContentHeader{
			ContentHeader: &pb.ContentHeader{
				RequestId:   request.Id,
				ContentType: "text/html",
				ContentSize: uint64(len(testPayload)),
				// Filename:    strPtr("index.html"),
			},
		},
	})
	h.conn.write(&pb.ClientMessage{
		Data: &pb.ClientMessage_ContentChunk{
			ContentChunk: &pb.ContentChunk{
				RequestId: request.Id,
				Sequence:  0,
				Data:      testPayload,
			},
		},
	})
}

func makeUrl(hello *pb.Hello, path string, query string) string {
	path = strings.TrimPrefix(path, "/")
	query = strings.TrimPrefix(query, "?")
	mac, err := computeMac(hello.ClientId, hello.ConnectionSecret, path, query)
	if err != nil {
		log.Fatalf("failed to compute MAC: %v\n", err)
	}
	macEncoded := hex.EncodeToString(mac)
	url := formatString(urlFormat,
		"base_url", *addr,
		"client_id", hello.ClientId,
		"mac", macEncoded,
		"path", path)
	if len(query) > 0 {
		url += "?" + query
	}
	return url
}

func dial() *clientConn {
	// TODO properly parse URL and replace protocol
	// FIXME handle https
	address := "ws" + strings.TrimPrefix(*addr, "http")
	address = strings.TrimSuffix(address, "/") + "/ws"
	log.Printf("dialing websocket address: %v\n", address)
	conn, _, err := websocket.DefaultDialer.Dial(address, nil)
	if err != nil {
		log.Fatalf("failed to dial websocket: %v\n", err)
	}
	conn.SetPongHandler(func(string) error {
		conn.SetReadDeadline(time.Now().Add(pongWait))
		return nil
	})
	// FIXME data race: writing ping while writing response
	go func() {
		ticker := time.NewTicker(pingInterval)
		for range ticker.C {
			conn.SetWriteDeadline(time.Now().Add(writeWait))
			err := conn.WriteMessage(websocket.PingMessage, []byte{})
			if err != nil {
				log.Fatalf("failed to write ping message: %v\n", err)
			}
		}
	}()
	return &clientConn{
		conn: conn,
	}
}

type clientConn struct {
	conn *websocket.Conn
}

func (c *clientConn) readHello() *pb.Hello {
	return getServerMessage[pb.ServerMessage_Hello](c.read()).Hello
}

func (c *clientConn) write(message *pb.ClientMessage) {
	data, err := proto.Marshal(message)
	if err != nil {
		log.Fatalf("failed to marshal client message: %v\n", err)
	}
	c.conn.SetWriteDeadline(time.Now().Add(writeWait))
	err = c.conn.WriteMessage(websocket.BinaryMessage, data)
	if err != nil {
		log.Fatalf("failed to write client message: %v", err)
	}
}

func (c *clientConn) read() *pb.ServerMessage {
	typ, data, err := c.conn.ReadMessage()
	if err != nil {
		log.Fatalf("failed to read message from server: %v\n", err)
	}
	if typ != websocket.BinaryMessage {
		log.Fatalf("received an unexpected message type: %v\n", typ)
	}
	msg := &pb.ServerMessage{}
	err = proto.Unmarshal(data, msg)
	if err != nil {
		log.Fatalf("failed to unmarshal server message: %v\n", err)
	}
	return msg
}

func getServerMessage[T interface{}, P *T](message *pb.ServerMessage) P {
	switch m := message.Data.(type) {
	case P:
		return m
	default:
		log.Fatalf("unexpected server message: %T %v\n", m, m)
	}
	panic("unreachable")
}

// FIXME copied from client.go
func computeMac(
	clientId string,
	clientSecret []byte,
	path string,
	query string,
) ([]byte, error) {
	mac := hmac.New(sha256.New, clientSecret)
	items := [][]byte{
		[]byte(clientId),
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

// https://stackoverflow.com/a/40811635/6748004
func formatString(format string, args ...string) string {
	for i, v := range args {
		if i%2 == 0 {
			args[i] = "{" + v + "}"
		}
	}
	return strings.NewReplacer(args...).Replace(format)
}

func strPtr(s string) *string {
	return &s
}
