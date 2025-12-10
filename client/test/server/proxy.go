/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Original source: https://github.com/pretty66/websocketproxy
// Modified to allow dropping packets on the fly for unit testing.

package main

import (
	"crypto/tls"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"sync/atomic"
)

const (
	WsScheme   = "ws"
	WssScheme  = "wss"
	BufSize    = 1024 * 32
	DropNone   = 0
	DropActive = 1
	DropAll    = 2
)

var (
	byteSlicePool = sync.Pool{
		New: func() interface{} {
			return []byte{}
		},
	}
	byteSliceChan = make(chan []byte, 10)
)

func ByteSliceGet(length int) (data []byte) {
	select {
	case data = <-byteSliceChan:
	default:
		data = byteSlicePool.Get().([]byte)[:0]
	}

	if cap(data) < length {
		data = make([]byte, length)
	} else {
		data = data[:length]
	}

	return data
}

func ByteSlicePut(data []byte) {
	select {
	case byteSliceChan <- data:
	default:
		//lint:ignore SA6002 ignore
		byteSlicePool.Put(data)
	}
}

var ErrFormatAddr = errors.New("remote websockets addr format error")

type WebsocketProxy struct {
	// ws, wss
	scheme string
	// The target address: host:port
	remoteAddr string
	// path
	defaultPath string
	tlsc        *tls.Config
	logger      *log.Logger
	// Send handshake before callback
	beforeHandshake func(r *http.Request) error
	// whether proxied packets should be dropped
	drop atomic.Int32
}

type dropReader struct {
	conn net.Conn
	drop func() bool
}

func (r *dropReader) Read(p []byte) (int, error) {
	n, err := r.conn.Read(p)
	if r.drop() {
		return n, nil
	}
	return n, err
}

type dropWriter struct {
	conn net.Conn
	drop func() bool
}

func (w *dropWriter) Write(p []byte) (int, error) {
	if w.drop() {
		return len(p), nil
	}
	return w.conn.Write(p)
}

type Options func(wp *WebsocketProxy)

// You must carry a port number，ws://ip:80/ssss, wss://ip:443/aaaa
// ex: ws://ip:port/ajaxchattest
func NewProxy(addr string, beforeCallback func(r *http.Request) error, options ...Options) (*WebsocketProxy, error) {
	u, err := url.Parse(addr)
	if err != nil {
		return nil, ErrFormatAddr
	}
	host, port, err := net.SplitHostPort(u.Host)
	if err != nil {
		return nil, ErrFormatAddr
	}
	if u.Scheme != WsScheme && u.Scheme != WssScheme {
		return nil, ErrFormatAddr
	}
	wp := &WebsocketProxy{
		scheme:          u.Scheme,
		remoteAddr:      fmt.Sprintf("%s:%s", host, port),
		defaultPath:     u.Path,
		beforeHandshake: beforeCallback,
		logger:          log.New(os.Stderr, "", log.LstdFlags),
	}
	if u.Scheme == WssScheme {
		wp.tlsc = &tls.Config{InsecureSkipVerify: true}
	}
	for op := range options {
		options[op](wp)
	}
	return wp, nil
}

func (wp *WebsocketProxy) ServeHTTP(writer http.ResponseWriter, request *http.Request) {
	wp.Proxy(writer, request)
}

func (wp *WebsocketProxy) Proxy(writer http.ResponseWriter, request *http.Request) {
	if strings.ToLower(request.Header.Get("Connection")) != "upgrade" ||
		strings.ToLower(request.Header.Get("Upgrade")) != "websocket" {
		_, _ = writer.Write([]byte(`Must be a websocket request`))
		return
	}
	hijacker, ok := writer.(http.Hijacker)
	if !ok {
		return
	}
	conn, _, err := hijacker.Hijack()
	if err != nil {
		return
	}
	defer conn.Close()

	// Stop dropping packets on new connections, when only active connections
	// were configured to drop packets. This will stop the dropping of packets
	// for any currently active connections, but unit tests currently only test
	// one connection at a time, so it's okay.
	wp.drop.CompareAndSwap(DropActive, DropNone)

	req := request.Clone(request.Context())
	req.URL.Path, req.URL.RawPath, req.RequestURI = wp.defaultPath, wp.defaultPath, wp.defaultPath
	req.Host = wp.remoteAddr
	if wp.beforeHandshake != nil {
		// Add headers, permission authentication + masquerade sources
		err = wp.beforeHandshake(req)
		if err != nil {
			_, _ = writer.Write([]byte(err.Error()))
			return
		}
	}
	var remoteConn net.Conn
	switch wp.scheme {
	case WsScheme:
		remoteConn, err = net.Dial("tcp", wp.remoteAddr)
	case WssScheme:
		remoteConn, err = tls.Dial("tcp", wp.remoteAddr, wp.tlsc)
	}
	if err != nil {
		_, _ = writer.Write([]byte(err.Error()))
		return
	}
	defer remoteConn.Close()
	err = req.Write(remoteConn)
	if err != nil {
		wp.logger.Println("remote write err:", err)
		return
	}
	errChan := make(chan error, 2)
	copyConn := func(a, b net.Conn) {
		buf := ByteSliceGet(BufSize)
		defer ByteSlicePut(buf)
		reader := &dropReader{conn: b, drop: func() bool {
			return wp.drop.Load() != DropNone
		}}
		writer := &dropWriter{conn: a, drop: func() bool {
			return wp.drop.Load() != DropNone
		}}
		_, err := io.CopyBuffer(writer, reader, buf)
		errChan <- err
	}
	go copyConn(conn, remoteConn) // response
	go copyConn(remoteConn, conn) // request
	err = <-errChan
	if err != nil {
		log.Println(err)
	}
}

func (wp *WebsocketProxy) Drop(state int32) {
	wp.drop.Store(state)
}

func SetTLSConfig(tlsc *tls.Config) Options {
	return func(wp *WebsocketProxy) {
		wp.tlsc = tlsc
	}
}

func SetLogger(l *log.Logger) Options {
	return func(wp *WebsocketProxy) {
		if l != nil {
			wp.logger = l
		}
	}
}
