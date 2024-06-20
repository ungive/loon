package main

import (
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"

	"github.com/goccy/go-yaml"
	"github.com/gorilla/websocket"
	"github.com/ungive/loon/pkg/server"
)

var (
	addr       = flag.String("addr", ":8080", "http service address")
	verbose    = flag.Bool("verbose", false, "enable verbose logging")
	configPath = flag.String("config", "", "the path to a config file")
)

var log *slog.Logger
var config *server.Config
var acceptedContentTypes *server.ContentTypeRegistry

// TODO: use prometheus time series values
var (
	activeClientCount = atomic.Int64{}
	requestIndex      = atomic.Int64{}
)

func init() {
	flag.Parse()
	log = newLogger()
	config = readConfig()
	acceptedContentTypes = contentTypeRegistryForConfig(config)
}

func main() {
	handler := newHandler()
	srv := http.Server{
		Addr:         *addr,
		WriteTimeout: config.Http.WriteWait,
		Handler:      handler,
	}
	err := srv.ListenAndServe()
	if err != nil {
		log.Error("failed to listen on address", "addr", *addr, "err", err)
		abort()
	}
}

type handler struct {
	manager  server.ClientManager
	upgrader websocket.Upgrader
}

func newHandler() http.Handler {
	h := &handler{
		manager:  server.NewClientManager(),
		upgrader: websocket.Upgrader{},
	}
	go h.manager.Run()
	return h.handler()
}

func (h *handler) handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", h.serveWs)
	mux.HandleFunc("GET /{client_id}/{mac}/{path...}", h.serve)
	return mux
}

func (h *handler) serveWs(w http.ResponseWriter, r *http.Request) {
	log := log.With("context", "websocket")
	conn, err := h.upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Warn("failed to upgrade websocket request", "err", err)
		return
	}
	client, err := server.NewClient(conn, &server.ClientConfig{
		BaseUrl:     r.URL.Host,
		Constraints: config.Constraints,
		Intervals:   config.Intervals,
	})
	if err != nil {
		log.Error("failed to create client instance", "err", err)
		return
	}
	h.manager.Register(client)
	activeClientCount.Add(1)
	log.Debug("client connected",
		"client_id", client.ID(),
		"active_clients", activeClientCount.Load(),
	)
	go func() {
		defer func() {
			h.manager.Unregister(client)
			activeClientCount.Add(-1)
			log.Debug("client disconnected",
				"client_id", client.ID(),
				"active_clients", activeClientCount.Load(),
			)
		}()
		client.Run()
	}()
}

func (h *handler) serve(w http.ResponseWriter, r *http.Request) {
	index := requestIndex.Add(1)
	log := log.With("context", "request", "index", index)
	if !acceptedContentTypes.CanServeAccept(r.Header.Get("Accept")) {
		close(w, http.StatusNotAcceptable,
			"not serving any acceptable content type", log, nil)
		return
	}
	clientIdStr := r.PathValue("client_id")
	log = log.With("client_id", clientIdStr)
	clientID, err := server.ParseUUID(clientIdStr)
	if err != nil {
		close(w, http.StatusBadRequest, "invalid client ID", log, err)
		return
	}
	macStr := r.PathValue("mac")
	mac, err := hex.DecodeString(macStr)
	if err != nil {
		log := log.With("mac", macStr)
		close(w, http.StatusBadRequest, "invalid MAC", log, err)
		return
	}
	client, err := h.manager.Get(clientID)
	if err != nil {
		closeNotFound(w, log, err)
		return
	}
	path := r.PathValue("path")
	query := r.URL.RawQuery
	log = log.With("path", path)
	if len(query) > 0 {
		log = log.With("query", query)
	}
	clientRequest, err := client.Request(path, query, mac)
	if err != nil {
		if errors.Is(err, server.ErrClientClosed) {
			// The client has been closed in the meantime,
			// which is equivalent to not having it found it in the manager.
			closeNotFound(w, log, err)
		} else if errors.Is(err, server.ErrBadMac) {
			// Don't tell the HTTP client that it knows a valid client ID,
			// by sending a 404 status code here as well (see specification).
			log := log.With("mac", macStr)
			closeNotFound(w, log, err)
		} else if errors.Is(err, server.ErrBadPath) {
			close(w, http.StatusBadRequest, "path is malformed", log, err)
		} else if errors.Is(err, server.ErrBadQuery) {
			close(w, http.StatusBadRequest, "query is malformed", log, err)
		} else {
			log := log.With("mac", macStr)
			closeInternalServerError(w, log, fmt.Errorf(
				"error while trying to make a client request: %w", err))
		}
		return
	}
	h.serveRequest(w, r, clientRequest, log)
}

func (h *handler) serveRequest(
	w http.ResponseWriter,
	r *http.Request,
	request server.Request,
	log *slog.Logger,
) {
	// Make sure the request is closed, if it isn't already.
	defer request.Close("request closed")
	w.Header().Set("Connection", "Keep-Alive")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	var response server.Response
	select {
	case response = <-request.Response():
	case <-request.Closed():
		closeTimeout(w, log, nil)
		return
	}
	contentType, err := server.NewContentType(response.Header().ContentType)
	if err != nil {
		// this should never happen, since the content type in the header
		// is compared against the accepted content types in the contraints,
		// which were validated at server startup.
		request.Close("unexpected invalid response content type")
		closeInternalServerError(w, log, fmt.Errorf(
			"client response content type not expected to be invalid: %w", err))
		return
	}
	// Create a registry with a single content type,
	// to check if the accept header allows us to serve this file.
	registry := server.NewContentTypeRegistry(
		server.NewSingleContentTypeIndex(contentType))
	if !registry.CanServeAccept(r.Header.Get("Accept")) {
		request.Close("response content type not accepted by HTTP client")
		log := log.With("content_type", contentType.Type, "accept", r.Header.Get("Accept"))
		close(w, http.StatusNotAcceptable,
			"client response content type not acceptable", log, nil)
		return
	}
	// Set the content type headers and the attachment filename, if present.
	w.Header().Set("Content-Type", contentType.String())
	if filename := response.Header().Filename; filename != nil {
		value := strings.ReplaceAll(*filename, "\"", "\\\"")
		w.Header().Set("Content-Disposition",
			"attachment; filename=\""+value+"\"")
	}
	// Set cache control headers.
	maxAge := uint32(0)
	if cacheFor := response.Header().MaxCacheDuration; cacheFor != nil {
		maxAge = *cacheFor
	}
	if maxAge == 0 {
		w.Header().Set("Cache-Control", "no-store")
	} else {
		w.Header().Set("Age", "0")
		w.Header().Set("Cache-Control", fmt.Sprintf("max-age=%d", maxAge))
	}
	// Read the content and forward it to the HTTP client.
	w.WriteHeader(http.StatusOK)
	flusher := w.(http.Flusher)
	written := uint64(0)
recv:
	for {
		var chunk []byte
		select {
		case data, ok := <-response.Chunks():
			if !ok {
				break recv
			}
			chunk = data
		case <-request.Closed():
			log.Debug("client request closed")
			return
		}
		n, err := w.Write(chunk)
		if err != nil {
			request.Close("failed to forward response")
			log.Debug("failed to write chunk", "err", err)
			return
		}
		flusher.Flush()
		written += uint64(n)
	}
	if written != response.Header().ContentSize {
		request.Close("fatal: forwarded less/more data than expected")
		log.Error("wrote less/more data than expected",
			"written", written, "expected", response.Header().ContentSize)
		return
	}
	log.Info("forwarded client content", "status", http.StatusOK)
	// Attempt to send a Success message to the websocket client,
	// but if it returns an error, it does not matter for the HTTP client.
	// Do this in a separate goroutine, since the HTTP request is finished.
	go request.Success()
}

// ---

func newLogger() *slog.Logger {
	options := &slog.HandlerOptions{
		Level: slog.LevelInfo,
	}
	if *verbose {
		options.Level = slog.LevelDebug
	}
	handler := slog.NewJSONHandler(os.Stdout, options)
	return slog.New(handler)
}

func readConfig() *server.Config {
	log := log.With("context", "config")
	if len(*configPath) <= 0 {
		log.Warn("using the default config, only do this during testing")
		return defaultConfig
	}
	path, err := filepath.Abs(*configPath)
	if err != nil {
		log.Error("failed to retrieve absolute path of config", "err", err)
		abort()
	}
	data, err := os.ReadFile(path)
	if err != nil {
		log.Error("failed to open config file:", "err", err)
		abort()
	}
	var v server.Config
	if err := yaml.Unmarshal(data, &v); err != nil {
		log.Error("failed to parse config YAML:", "err", err)
		abort()
	}
	log.Info("loaded config", "path", path)
	return &v
}

func contentTypeRegistryForConfig(config *server.Config) *server.ContentTypeRegistry {
	clog := log.With("context", "config")
	acceptedContentTypes := config.Constraints.AcceptedContentTypes
	contentTypes := make([]*server.ContentType, len(acceptedContentTypes))
	for i, value := range acceptedContentTypes {
		contentType, err := server.NewContentType(value)
		if err != nil {
			clog.Error("failed to parse content type",
				slog.Int("index", i),
				slog.String("value", value),
				slog.Any("err", err))
			abort()
		}
		contentTypes[i] = contentType
	}
	index := server.NewMultiContentTypeIndex(contentTypes)
	registry := server.NewContentTypeRegistry(index)
	for i, value := range contentTypes {
		vlog := clog.With("value", value.Type)
		if len(value.Params) > 0 {
			vlog.Error("content type may not have parameters",
				slog.Int("index", i),
				slog.String("value", value.Type))
			abort()
		}
		config.Constraints.AcceptedContentTypes[i] = value.Type
	}
	return registry
}

func closeNotFound(
	w http.ResponseWriter,
	log *slog.Logger,
	err error,
) {
	close(w, http.StatusNotFound, "not found", log, err)
}

func closeInternalServerError(
	w http.ResponseWriter,
	log *slog.Logger,
	err error,
) {
	close(w, http.StatusInternalServerError, "internal server error", log, err)
}

func closeTimeout(
	w http.ResponseWriter,
	log *slog.Logger,
	err error,
) {
	close(w, http.StatusGatewayTimeout, "response timed out", log, err)
}

func close(
	w http.ResponseWriter,
	status int,
	text string,
	logInfo *slog.Logger,
	err error,
) {
	w.WriteHeader(status)
	w.Write([]byte(text))
	if logInfo != nil {
		logInfo = logInfo.With("status", status, "text", text)
		if err != nil {
			logInfo = logInfo.With("err", err)
		}
		if status >= 500 && status <= 599 {
			logInfo.Error("request failed")
		} else {
			logInfo.Warn("request failed")
		}
	}
}

func abort() {
	exit_code := 1
	log.Error("Exiting", "exit_code", exit_code)
	os.Exit(exit_code)
}
