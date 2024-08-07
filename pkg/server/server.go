package server

import (
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"reflect"
	"strings"
	"sync/atomic"

	"github.com/gorilla/websocket"
)

// TODO: use prometheus time series values
var (
	activeClientCount = atomic.Int64{}
	requestIndex      = atomic.Int64{}
)

type Server interface {
	// Run loop for the server.
	Run()
	// Returns the HTTP handler for this server.
	Handler() http.Handler
}

type serverImpl struct {
	config       *Options
	log          *slog.Logger
	manager      ClientManager
	upgrader     websocket.Upgrader
	contentTypes *ContentTypeRegistry
}

func NewServer(config *Options, log *slog.Logger) (Server, error) {
	server := &serverImpl{
		config:       config,
		log:          log,
		manager:      NewClientManager(),
		upgrader:     websocket.Upgrader{},
		contentTypes: nil,
	}
	acceptedContentTypes := config.Protocol.Constraints.AcceptedContentTypes
	registry, err := NewContentTypeRegistryFromStrings(acceptedContentTypes)
	if err != nil {
		return nil, err
	}
	for i, value := range registry.Index.ContentTypes() {
		if len(value.Params) > 0 {
			return nil, fmt.Errorf("an accepted content type"+
				" may not have parameters: %q at index %d",
				value, i)
		}
		// Usually we would not modify the config instance,
		// but here we just make sure it has a properly formatted value
		// without any extraneous characters.
		config.Protocol.Constraints.AcceptedContentTypes[i] = value.Type
	}
	server.contentTypes = registry
	return server, nil
}

func (s *serverImpl) Run() {
	s.manager.Run()
}

func (s *serverImpl) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", s.serveWs)
	mux.HandleFunc("GET /{client_id}/{mac}/{path...}", s.serveRequest)
	mux.HandleFunc("HEAD /{client_id}/{mac}/{path...}",
		func(w http.ResponseWriter, r *http.Request) {
			// Ignore HEAD requests
			w.WriteHeader(http.StatusNotFound)
		})
	return mux
}

func (s *serverImpl) serveWs(w http.ResponseWriter, r *http.Request) {
	log := s.log.With("context", "websocket")
	conn, err := s.upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Warn("failed to upgrade websocket request", "err", err)
		return
	}
	client, err := NewClient(conn, s.config.Protocol)
	if err != nil {
		log.Error("failed to create client instance", "err", err)
		return
	}
	s.manager.Register(client)
	activeClientCount.Add(1)
	log = log.With("client_id", client.ID().UrlEncode())
	log.Info("client connected", "active_clients", activeClientCount.Load())
	go func() {
		defer func() {
			s.manager.Unregister(client)
			activeClientCount.Add(-1)
			log.Info("client disconnected",
				"active_clients", activeClientCount.Load())
		}()
		client.Run()
	}()
}

func (s *serverImpl) serveRequest(w http.ResponseWriter, r *http.Request) {
	index := requestIndex.Add(1)
	log := s.log.With("context", "request", "index", index)
	accept := r.Header.Get("Accept")
	if len(accept) > 0 && !s.contentTypes.CanServeAccept(accept) {
		log := s.log.With("accept", accept)
		status(w, http.StatusNotAcceptable,
			"not serving any acceptable content type", log, nil)
		return
	}
	clientIdStr := r.PathValue("client_id")
	log = log.With("client_id", clientIdStr)
	clientID, err := UrlDecodeUUID(clientIdStr)
	if err != nil {
		status(w, http.StatusBadRequest, "invalid client ID", log, err)
		return
	}
	macStr := r.PathValue("mac")
	mac, err := UrlDecodeMAC(macStr)
	if err != nil {
		log := log.With("mac", macStr)
		status(w, http.StatusBadRequest, "invalid MAC", log, err)
		return
	}
	client, err := s.manager.Get(clientID)
	if err != nil {
		statusNotFound(w, log, err)
		return
	}
	path := r.PathValue("path")
	log = log.With("path", path)
	clientRequest, err := client.Request(path, mac)
	if err != nil {
		if errors.Is(err, ErrClientClosed) {
			// The client has been closed in the meantime,
			// which is equivalent to not having it found it in the manager.
			statusNotFound(w, log, err)
		} else if errors.Is(err, ErrBadMac) {
			// Don't tell the HTTP client that it knows a valid client ID,
			// by sending a 404 status code here as well (see specification).
			log := log.With("mac", macStr)
			statusNotFound(w, log, err)
		} else {
			log := log.With("mac", macStr)
			statusInternalServerError(w, log, fmt.Errorf(
				"error while trying to make a client request: %w", err))
		}
		return
	}
	s.serve(w, r, clientRequest, log)
}

func (s *serverImpl) serve(
	w http.ResponseWriter,
	r *http.Request,
	request Request,
	log *slog.Logger,
) {
	// Make sure the request is closed, if it isn't already.
	defer request.Close("request closed")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	var response Response
	select {
	case response = <-request.Response():
		if reflect.ValueOf(response).IsNil() {
			// Return a 404 response, since this is an empty response.
			// Make sure that this response is cached, so that clients
			// cannot be spammed with requests that yield an empty response.
			// It is okay that merely this 404 response is cached
			// and not any of the other ones.
			// While an attacker could tell that a client sits behind this URL,
			// the outside world was meant to be able to tell anyway,
			// since this URL has been generated by the client and shared.
			// And since it's cached, there is no possibility
			// for client abuse anyway.
			statusNotFoundCached(w, log, nil)
			return
		}
	case <-request.Closed():
		statusTimeout(w, log, nil)
		return
	}
	contentType, err := NewContentType(response.Header().ContentType)
	if err != nil {
		// this should never happen, since the content type in the header
		// is compared against the accepted content types in the contraints,
		// which were validated at server startup.
		request.Close("unexpected invalid response content type")
		statusInternalServerError(w, log, fmt.Errorf(
			"client response content type not expected to be invalid: %w", err))
		return
	}
	// Create a registry with a single content type,
	// to check if the accept header allows us to serve this file.
	registry := NewContentTypeRegistry(NewSingleContentTypeIndex(contentType))
	accept := r.Header.Get("Accept")
	if len(accept) > 0 && !registry.CanServeAccept(accept) {
		request.Close("response content type not accepted by HTTP client")
		log := log.With("content_type", contentType.Type, "accept", accept)
		status(w, http.StatusNotAcceptable,
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
		maxAge = min(*cacheFor, s.config.Protocol.Constraints.CacheDurationInt())
	}
	w.Header().Set("Age", "0") // The data is always recent
	if maxAge == 0 {
		w.Header().Set("Cache-Control", "no-store")
	} else {
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

func statusNotFoundCached(
	w http.ResponseWriter,
	log *slog.Logger,
	err error,
) {
	// TODO make cache duration of 404 pages configurable.
	w.Header().Set("Age", "0")
	w.Header().Set("Cache-Control", fmt.Sprintf("max-age=%d", 10))
	statusNotFound(w, log, err)
}

func statusNotFound(
	w http.ResponseWriter,
	log *slog.Logger,
	err error,
) {
	status(w, http.StatusNotFound, "not found", log, err)
}

func statusInternalServerError(
	w http.ResponseWriter,
	log *slog.Logger,
	err error,
) {
	status(w, http.StatusInternalServerError, "internal server error", log, err)
}

func statusTimeout(
	w http.ResponseWriter,
	log *slog.Logger,
	err error,
) {
	status(w, http.StatusGatewayTimeout, "response timed out", log, err)
}

func status(
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
