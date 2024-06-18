package main

import (
	"encoding/hex"
	"flag"
	"fmt"
	"log"
	"net/http"
	"time"

	"github.com/gorilla/websocket"
	"github.com/ungive/loon/pkg/pb"
	"github.com/ungive/loon/pkg/server"
)

// Definitions
// client: device that connects to the websocket instance and provides content
// server: this program, accepting websocket client connections
//   and accepting HTTP requests which fetch content from connected clients
// user: anyone making HTTP endpoint requests to retrieve client content
//   via the server

// HTTP endpoints
// /ws: websocket endpoint for clients to connect to
// /<client_id>/<path>: content request URL
//   client_id: hex-encoded bytes for a client ID
//   path: request path for content from the client connection

var constraints = &pb.Constraints{
	MaxContentSize: 8 * 1024 * 1024, // 8 MiB
	ChunkSize:      64 * 1024,       // 64 KiB
	AcceptedContentTypes: []string{
		"image/png",
		"image/jpeg",
		"text/plain",
		"text/html",
	},
}

var intervals = &server.ProtocolIntervals{
	WriteWait:       10 * time.Second,
	PongWait:        60 * time.Second,
	PingInterval:    48 * time.Second,
	TimeoutDuration: 30 * time.Second,
	TimeoutInterval: 8 * time.Second,
}

var addr = flag.String("addr", ":8080", "http service address")

var upgrader = websocket.Upgrader{}

func serveWs(w http.ResponseWriter, r *http.Request, m server.ClientManager) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("failed to upgrade websocket request: %v\n", err)
		return
	}
	client, err := server.NewClient(conn, r.URL.Host, constraints, intervals)
	if err != nil {
		log.Printf("failed to create client: %v\n", err)
		return
	}
	m.Register(client)
	log.Printf("registered client: %v\n", client.ID())
	go func() {
		client.Run()
		m.Unregister(client)
		log.Printf("unregistered client: %v\n", client.ID())
	}()
}

func serve(
	w http.ResponseWriter,
	r *http.Request,
	m server.ClientManager,
	clientID server.UUID,
	mac []byte,
	path string,
	query string,
) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		panic("expected http.ResponseWriter to be an http.Flusher")
	}

	fmt.Printf("Serving: %s %s %s %s\n",
		clientID.String(), hex.EncodeToString(mac), path, query)
	// fmt.Fprintf(w, "Serving: %s %s %s %s",
	// 	clientID.String(), hex.EncodeToString(mac), path, query)

	client, err := m.Get(clientID)
	if err != nil {
		log.Printf("failed to retrieve client: %v\n", err)
		return
	}
	request, err := client.Request(path, query, mac)
	if err != nil {
		log.Printf("failed to forward request to client: %v\n", err)
		return
	}
	response, ok := <-request.Response()
	if !ok {
		log.Println("did not get a response, cancelling")
		return
	}
	log.Printf("header: %v", response.Header())
	var chunk []byte
	select {
	case chunk, ok = <-response.Chunks():
		if !ok {
			log.Println("response chunk channel closed, cancelling")
			return
		}
	case <-response.Closed():
		log.Println("response closed, cancelling")
		return
	}
	log.Printf("chunk: %v", chunk)
	log.Printf("chunk: %v", string(chunk))

	w.Header().Set("Connection", "Keep-Alive")
	w.Header().Set("Content-Type", response.Header().ContentType)
	if filename := response.Header().Filename; filename != nil {
		w.Header().Set("Content-Disposition",
			`attachment; filename="`+*filename+`"`)
	}
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Transfer-Encoding", "chunked")
	n, err := w.Write(chunk)
	if err != nil {
		log.Printf("failed to write chunk: %v\n", err)
		return
	}
	if n != len(chunk) {
		log.Printf("did not write expected number of bytes: %v of %v", n, len(chunk))
		return
	}
	flusher.Flush()
	request.Success()

	// TODO
}

func main() {
	// uuid, err := server.NewUUID()
	// if err != nil {
	// 	panic(err)
	// }
	// fmt.Println(uuid.String())

	flag.Parse()
	manager := server.NewClientManager()
	go manager.Run()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /{client_id}/{mac}/{path...}",
		func(w http.ResponseWriter, r *http.Request) {
			clientIdStr := r.PathValue("client_id")
			if len(clientIdStr) <= 0 {
				log.Printf("client ID cannot be empty\n")
				return
			}
			clientID, err := server.ParseUUID(clientIdStr)
			if err != nil {
				log.Printf("failed to parse UUID: %q\n", err)
				return
			}
			macStr := r.PathValue("mac")
			if len(macStr) <= 0 {
				log.Printf("MAC cannot be empty\n")
				return
			}
			mac, err := hex.DecodeString(macStr)
			if err != nil {
				log.Printf("failed to decode hex-encoded MAC: %q\n", err)
				return
			}
			path := "/" + r.PathValue("path")
			query := r.URL.RawQuery
			if len(query) > 0 {
				query = "?" + query
			}
			serve(w, r, manager, clientID, mac, path, query)
		})
	mux.HandleFunc("/ws",
		func(w http.ResponseWriter, r *http.Request) {
			serveWs(w, r, manager)
		})
	err := http.ListenAndServe(*addr, mux)
	if err != nil {
		log.Fatal("ListenAndServe: ", err)
	}
}
