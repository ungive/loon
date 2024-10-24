package server_test

import (
	"crypto/hmac"
	"crypto/sha256"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/stretchr/testify/assert"
	"google.golang.org/protobuf/proto"

	"github.com/ungive/loon/pkg/pb"
	. "github.com/ungive/loon/pkg/server"
)

const (
	testTimeout     = 250 * time.Millisecond
	testAfterWait   = 10 * time.Millisecond
	testContentType = "text/plain"
	testPath        = "example.txt"
	testAddress     = "http://example.com"
)

var (
	errTestTimeout       = errors.New("test action timed out")
	errNotABinaryMessage = errors.New("websocket message is not a binary message")
)

var defaultIntervals = &ProtocolIntervals{
	WriteTimeout: 10 * time.Second,
	PongTimeout:  40 * time.Millisecond,
	PingInterval: 5 * time.Millisecond,
	// Note that these should be smaller than the test timeout,
	// especially when testing protocol timeouts.
	ClientTimeout:         60 * time.Millisecond,
	ClientTimeoutInterval: 10 * time.Millisecond,
}

var defaultConstraints = &pb.Constraints{
	MaxContentSize: 1024, // 1 KiB
	ChunkSize:      64,   // 64 B
	AcceptedContentTypes: []string{
		testContentType,
	},
	CacheDuration: 0,
}

func Test_server_sends_Hello_when_client_connects(t *testing.T) {
	_, conn, client, done := getServerConnClient(t)
	defer done()
	m := conn.readHello()
	assert.Equal(t, testAddress, m.BaseUrl)
	assert.Equal(t, client.ID().UrlEncode(), m.ClientId)
	assert.Equal(t, MAC_KEY_SIZE, len(m.ConnectionSecret))
	assert.Equal(t, defaultConstraints.MaxContentSize, m.Constraints.MaxContentSize)
	assert.Equal(t, defaultConstraints.ChunkSize, m.Constraints.ChunkSize)
	assert.Equal(t, defaultConstraints.AcceptedContentTypes, m.Constraints.AcceptedContentTypes)
	assert.Equal(t, defaultConstraints.CacheDuration, m.Constraints.CacheDuration)
}

func Test_base_URL_of_Hello_message_does_not_end_in_trailing_slash_when_client_connects(t *testing.T) {
	server := newWebsocketServer(t, testAddress+"/")
	conn, _, done := getConnClient(server)
	defer done()
	m := conn.readHello()
	assert.False(t, strings.HasSuffix(m.BaseUrl, "/"),
		"base URL should not end in a trailing slash")
}

func Test_server_sends_Request_when_calling_Client_Request(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	now := time.Now()
	_, err := client.request(testPath)
	assert.NoError(t, err)
	m := conn.readRequest()
	assert.Greater(t, m.Id, uint64(0))
	assert.Equal(t, testPath, m.Path)
	assert.WithinDuration(t, now, m.Timestamp.AsTime(), testTimeout)
	client.expectActiveRequests(1)
}

func Test_server_sends_Request_with_unique_IDs_when_calling_Client_Request_twice(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	_, err := client.request(testPath)
	assert.NoError(t, err)
	_, err = client.request("x" + testPath)
	assert.NoError(t, err)
	m1 := conn.readRequest()
	m2 := conn.readRequest()
	assert.NotEqual(t, m1.Id, m2.Id)
	assert.NotEqual(t, m1.Path, m2.Path)
	client.expectActiveRequests(2)
}

func Test_server_sends_RequestClosed_when_calling_Request_Close(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	message := "MSG"
	request.Close(message)
	m1 := conn.readRequest()
	m2 := conn.readRequestClosed()
	assert.Equal(t, m1.Id, m2.RequestId)
	assert.Equal(t, message, m2.Message)
	assertNoChanValue(t, request.Completed())
	client.expectActiveRequests(1)
}

func Test_Request_Response_chan_yields_nil_when_client_sends_EmptyResponse(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID(),
	})
	close := conn.waitCloseErr()
	response := waitForNullableChanValue(t, request.Response(), close)
	assert.Nil(t, response)
	waitForChanClose(t, request.Completed(), close)
	assertNoChanValue(t, request.Closed())
	client.expectActiveRequests(0)
}

func Test_Request_Response_chan_yields_Response_when_client_sends_ContentHeader(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(1),
	})
	close := conn.waitCloseErr()
	response := waitForChanValue(t, request.Response(), close)
	assert.Equal(t, request.ID(), response.Header().RequestId)
	assert.Equal(t, testContentType, response.Header().ContentType)
	assert.Equal(t, uint64(1), response.Header().ContentSize)
	assert.Equal(t, request, response.Request())
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte("_"),
	})
	waitForChanClose(t, request.Completed(), close)
	assertNoChanValue(t, request.Closed())
	client.expectActiveRequests(1)
}

func Test_server_sends_Closed_when_client_sends_empty_ContentHeader(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: 0,
	})
	time.Sleep(server.intervals.ClientTimeout)
	conn.expectClose(pb.Close_REASON_INVALID_CONTENT_SIZE)
	client.waitForExit()
}

func Test_Response_Chunks_chan_yields_single_chunk_when_client_sends_ContentChunk(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	payload := "Hello"
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(len(payload)),
	})
	close := conn.waitCloseErr()
	response := waitForChanValue(t, request.Response(), close)
	assertNoChanValue(t, request.Completed())
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte(payload),
	})
	waitForChanClose(t, request.Completed(), close)
	chunk := waitForChanValue(t, response.Chunks(), close)
	assert.Equal(t, len(payload), len(chunk))
	waitForChanClose(t, response.Chunks(), close)
	assertNoChanValue(t, request.Closed())
	client.expectActiveRequests(1)
}

func Test_Response_Chunks_chan_yields_two_chunks_when_client_sends_two_ContentChunks(t *testing.T) {
	server := newWebsocketServer(t, testAddress)
	server.constraints.ChunkSize = uint64(8)
	conn, client, _, done := getConnClientHello(server)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	payload := "Hello world!"
	payload1 := payload[:server.constraints.ChunkSize]
	payload2 := payload[server.constraints.ChunkSize:]
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(len(payload)),
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte(payload1),
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  1,
		Data:      []byte(payload2),
	})
	close := conn.waitCloseErr()
	response := waitForChanValue(t, request.Response(), close)
	chunk1 := waitForChanValue(t, response.Chunks(), close)
	chunk2 := waitForChanValue(t, response.Chunks(), close)
	assert.Equal(t, payload, string(chunk1)+string(chunk2))
	waitForChanClose(t, response.Chunks(), close)
	client.expectActiveRequests(1)
}

func Test_Response_Chunks_chan_is_closed_when_all_chunks_have_been_received(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.sendCompletedResponse(request.ID())
	close := conn.waitCloseErr()
	response := waitForChanValue(t, request.Response(), close)
	waitForChanValue(t, response.Chunks(), close)
	waitForChanClose(t, response.Chunks(), close)
}

func Test_Request_Closed_channel_is_closed_when_calling_Client_Close(t *testing.T) {
	server := newWebsocketServer(t, testAddress)
	// Make sure the Request.Closed() channel is not closed because of a timeout.
	server.intervals.ClientTimeout = time.Minute
	conn, client, _, done := getConnClientHello(server)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	client.Close()
	waitForChanClose(t, request.Closed(), nil)
	conn.readClose()
}

func Test_Request_Closed_channel_is_closed_when_calling_Request_Close(t *testing.T) {
	server := newWebsocketServer(t, testAddress)
	// Make sure the Request.Closed() channel is not closed because of a timeout.
	server.intervals.ClientTimeout = time.Minute
	conn, client, _, done := getConnClientHello(server)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	request.Close("")
	waitForChanClose(t, request.Closed(), nil)
	conn.readRequestClosed()
	client.expectActiveRequests(1)
}

func Test_Request_Closed_channel_is_closed_when_client_sends_CloseResponse(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(16),
	})
	conn.writeCloseResponse(&pb.CloseResponse{
		RequestId: request.ID(),
	})
	close := conn.waitCloseErr()
	waitForChanValue(t, request.Response(), close)
	waitForChanClose(t, request.Closed(), close)
	client.expectActiveRequests(0)
}

func Test_Request_Closed_channel_is_closed_when_client_times_out(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.readRequestClosed()
	waitForChanClose(t, request.Closed(), nil)
	client.expectActiveRequests(1)
}

func Test_Request_Closed_channel_is_closed_when_client_disconnects(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.close()
	waitForChanClose(t, request.Closed(), nil)
	client.waitForExit()
}

func Test_Response_chan_yields_nil_when_client_times_out_after_EmptyResponse(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID(),
	})
	response := waitForNullableChanValue(t, request.Response(), nil)
	assert.Nil(t, response)
	waitForChanClose(t, request.Completed(), nil)
	assertNoChanValue(t, request.Closed())
	client.expectActiveRequests(0)
}

func Test_server_sends_RequestClosed_when_client_times_out_after_ContentHeader(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(16),
	})
	conn.readRequestClosed()
	client.expectActiveRequests(1)
}

func Test_server_sends_Closed_when_client_does_not_send_CloseResponse_after_server_sent_RequestClosed(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(16),
	})
	conn.readRequestClosed()
	time.Sleep(server.intervals.ClientTimeout)
	conn.expectClose(pb.Close_REASON_TIMED_OUT)
	client.waitForExit()
}

func Test_server_sends_Success_when_calling_Request_Success_after_receiving_all_chunks(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	payload := "you are loved"
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(len(payload)),
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte(payload),
	})
	response := waitForChanValue(t, request.Response(), nil)
	chunk := waitForChanValue(t, response.Chunks(), nil)
	assert.Equal(t, payload, string(chunk))
	err = request.Success()
	assert.NoError(t, err)
	m := conn.readSuccess()
	assert.Equal(t, request.ID(), m.RequestId)
	client.expectActiveRequests(0)
}

func Test_server_sends_no_Success_when_calling_Request_Success_after_receiving_EmptyResponse(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID(),
	})
	response := waitForNullableChanValue(t, request.Response(), nil)
	assert.Nil(t, response)
	err = request.Success()
	assert.Error(t, err)
}

func Test_Request_Success_returns_error_after_receiving_EmptyResponse(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID(),
	})
	assert.Nil(t, waitForNullableChanValue(t, request.Response(), nil))
	err = request.Success()
	assert.ErrorIs(t, err, ErrRequestDeleted)
	client.expectActiveRequests(0)
}

func Test_Request_Success_returns_error_when_some_chunks_are_pending(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(1),
	})
	waitForChanValue(t, request.Response(), nil)
	err = request.Success()
	assert.ErrorIs(t, err, ErrRequestNotCompleted)
	client.expectActiveRequests(1)
}

func Test_Request_Success_returns_error_after_calling_Request_Close(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(1),
	})
	waitForChanValue(t, request.Response(), nil)
	request.Close("")
	conn.readRequestClosed()
	err = request.Success()
	assert.ErrorIs(t, err, ErrRequestClosed)
	client.expectActiveRequests(1)
}

func Test_Request_Success_returns_error_after_request_timed_out(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		// The request is only closed after timeout if there are pending chunks.
		ContentSize: uint64(1),
	})
	time.Sleep(1 * server.intervals.ClientTimeout)
	conn.readRequestClosed()
	err = request.Success()
	assert.ErrorIs(t, err, ErrRequestClosed)
	client.expectActiveRequests(1)
}

func Test_server_has_one_active_request_when_client_timed_out_after_non_empty_ContentHeader(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(1),
	})
	time.Sleep(server.intervals.ClientTimeout)
	conn.readRequestClosed()
	client.expectActiveRequests(1)
}

func Test_server_has_zero_active_requests_when_client_timed_out_after_sending_EmptyResponse(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID(),
	})
	time.Sleep(2 * server.intervals.ClientTimeout)
	client.expectActiveRequests(0)
}

func Test_server_has_zero_active_requests_when_client_timed_out_after_sending_last_ContentChunk(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.sendCompletedResponse(request.ID())
	time.Sleep(2 * server.intervals.ClientTimeout)
	client.expectActiveRequests(0)
}

func Test_server_sends_RequestClosed_when_Request_Success_is_not_called_within_timeout_period_for_completed_request(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(16),
	})
	time.Sleep(server.intervals.ClientTimeout)
	conn.readRequestClosed()
}

func Test_Client_Request_returns_no_error_when_requesting_empty_path(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	_, err := client.request("")
	assert.NoError(t, err)
	conn.readRequest()
}

func Test_Client_Request_returns_no_error_when_requesting_path_without_leading_slash(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	_, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
}

func Test_server_sends_Request_without_leading_slash_in_path_when_calling_Client_Request_with_it(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	_, err := client.request("/" + testPath)
	assert.NoError(t, err)
	m := conn.readRequest()
	assert.Equal(t, strings.TrimLeft(m.Path, "/"), m.Path,
		"the path in the request must not start with a slash")
}

func Test_Client_Request_returns_error_when_requesting_with_invalid_MAC_hash(t *testing.T) {
	_, _, client, hello, done := getServerConnClientHello(t)
	defer done()
	mac, err := computeMac(testPath, client.ID().UrlEncode(), hello.ConnectionSecret)
	assert.NoError(t, err)
	for i := range mac {
		mac[i] = 0
	}
	_, err = client.Request(testPath, mac)
	assert.ErrorIs(t, err, ErrBadMac)
}

func Test_server_closes_connection_after_sending_Close_message(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID() + 1, // invalid request ID, should close
	})
	conn.readClose()
	client.waitForExit()
	time.Sleep(10 * time.Millisecond)
	assert.True(t, conn.isClosed(), "expected the connection to be closed")
}

func Test_server_sends_Close_when_client_sends_text_websocket_message(t *testing.T) {
	_, conn, _, _, done := getServerConnClientHello(t)
	defer done()
	err := conn.conn.WriteMessage(websocket.TextMessage, []byte("Hello"))
	assert.NoError(t, err)
	conn.expectClose(pb.Close_REASON_INVALID_CLIENT_MESSAGE)
}

func Test_server_sends_Close_when_client_sends_badly_encoded_protobuf_message(t *testing.T) {
	_, conn, _, _, done := getServerConnClientHello(t)
	defer done()
	err := conn.conn.WriteMessage(websocket.BinaryMessage, []byte("g32jhg4kjhg1"))
	assert.NoError(t, err)
	conn.expectClose(pb.Close_REASON_INVALID_CLIENT_MESSAGE)
}

func Test_server_sends_Close_when_client_sends_CloseResponse_after_response_is_completed(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.sendCompletedResponse(request.ID())
	waitForChanClose(t, request.Completed(), nil)
	time.Sleep(2 * server.intervals.ClientTimeout)
	conn.writeCloseResponse(&pb.CloseResponse{
		RequestId: request.ID(),
	})
	conn.expectClose(pb.Close_REASON_INVALID_REQUEST_ID)
}

func Test_server_sends_Close_when_client_sends_CloseResponse_before_first_response_message(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeCloseResponse(&pb.CloseResponse{
		RequestId: request.ID(),
	})
	conn.expectClose(pb.Close_REASON_INVALID_CLIENT_MESSAGE)
}

func Test_server_sends_Close_when_client_sends_CloseResponse_after_EmptyResponse(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID(),
	})
	conn.writeCloseResponse(&pb.CloseResponse{
		RequestId: request.ID(),
	})
	// The request is immediately concluded after an EmptyResponse,
	// so the connection should be closed with an invalid request ID error.
	conn.expectClose(pb.Close_REASON_INVALID_REQUEST_ID)
}

func Test_server_sends_Close_when_client_sends_CloseResponse_after_last_ContentChunk(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID(),
	})
	conn.writeCloseResponse(&pb.CloseResponse{
		RequestId: request.ID(),
	})
	// The request is concluded after an EmptyResponse,
	// so the connection should be closed with an invalid request ID error.
	conn.expectClose(pb.Close_REASON_INVALID_REQUEST_ID)
}

func Test_server_sends_Close_when_client_sends_CloseResponse_with_unknown_request_ID(t *testing.T) {
	server := newWebsocketServer(t, testAddress)
	server.constraints.ChunkSize = 1
	conn, client, _, done := getConnClientHello(server)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(2),
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte("_"),
	})
	conn.writeCloseResponse(&pb.CloseResponse{
		RequestId: request.ID() + 1, // invalid request ID
	})
	conn.expectClose(pb.Close_REASON_INVALID_REQUEST_ID)
}

func Test_server_sends_Close_when_client_sends_CloseResponse_twice(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(1),
	})
	conn.writeCloseResponse(&pb.CloseResponse{
		RequestId: request.ID(),
	})
	conn.writeCloseResponse(&pb.CloseResponse{
		RequestId: request.ID(),
	})
	conn.expectClose(pb.Close_REASON_INVALID_REQUEST_ID)
}

func Test_server_does_not_send_Close_when_client_sends_CloseResponse_before_receiving_RequestClosed(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(1),
	})
	request.Close("")
	conn.writeCloseResponse(&pb.CloseResponse{
		RequestId: request.ID(),
	})
	conn.readRequestClosed()
	time.Sleep(2 * server.intervals.ClientTimeout)
	client.expectActiveRequests(0)
}

func Test_server_sends_Close_when_client_sends_EmptyResponse_with_unknown_request_ID(t *testing.T) {
	server := newWebsocketServer(t, testAddress)
	conn, _, _, done := getConnClientHello(server)
	defer done()
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: 100,
	})
	conn.expectClose(pb.Close_REASON_INVALID_REQUEST_ID)
}

func Test_server_sends_Close_when_client_sends_ContentHeader_with_unknown_request_ID(t *testing.T) {
	server := newWebsocketServer(t, testAddress)
	conn, _, _, done := getConnClientHello(server)
	defer done()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   100,
		ContentType: testContentType,
		ContentSize: 1,
	})
	conn.expectClose(pb.Close_REASON_INVALID_REQUEST_ID)
}

func Test_server_sends_Close_when_client_sends_ContentChunk_with_unknown_request_ID(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: 1,
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID() + 1,
		Sequence:  0,
		Data:      []byte("_"),
	})
	conn.expectClose(pb.Close_REASON_INVALID_REQUEST_ID)
}

func Test_server_sends_Close_when_client_sends_EmptyResponse_twice_for_request_ID(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID(),
	})
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID(),
	})
	conn.expectClose(pb.Close_REASON_INVALID_REQUEST_ID)
}

func Test_server_sends_Close_when_client_sends_ContentHeader_twice_for_request_ID(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(1),
	})
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(1),
	})
	conn.expectClose(pb.Close_REASON_INVALID_CLIENT_MESSAGE)
}

func Test_server_sends_Close_when_client_sends_EmptyResponse_after_ContentHeader(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(1),
	})
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID(),
	})
	conn.expectClose(pb.Close_REASON_INVALID_CLIENT_MESSAGE)
}

func Test_server_does_not_send_Close_when_client_response_content_size_is_at_limit(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: server.constraints.MaxContentSize,
	})
	conn.readRequestClosed()
}

func Test_server_sends_Close_when_client_response_content_size_exceeds_constraints(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: server.constraints.MaxContentSize + 1,
	})
	conn.expectClose(pb.Close_REASON_INVALID_CONTENT_SIZE)
}

func Test_server_does_not_send_Close_when_client_response_filename_is_non_empty(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: 1,
		Filename:    strPtr("_"),
	})
	conn.readRequestClosed()
}

func Test_server_sends_Close_when_client_response_filename_is_empty(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: 1,
		Filename:    strPtr(""),
	})
	conn.expectClose(pb.Close_REASON_INVALID_FILENAME)
}

func Test_server_sends_Close_when_client_response_content_type_is_not_in_constraints(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: "invalid/type",
		ContentSize: 1,
	})
	conn.expectClose(pb.Close_REASON_FORBIDDEN_CONTENT_TYPE)
}

func Test_server_does_not_send_Close_when_client_response_content_type_has_parameters(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType + "; charset=utf-8",
		ContentSize: 1,
	})
	conn.readRequestClosed()
}

func Test_server_sends_Close_when_client_sends_ContentChunk_before_ContentHeader(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte("_"),
	})
	conn.expectClose(pb.Close_REASON_INVALID_CLIENT_MESSAGE)
}

func Test_server_sends_Close_when_client_sends_the_same_ContentChunk_twice(t *testing.T) {
	server := newWebsocketServer(t, testAddress)
	server.constraints.ChunkSize = uint64(1)
	conn, client, _, done := getConnClientHello(server)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: 2,
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte("_"),
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte("_"),
	})
	conn.expectClose(pb.Close_REASON_CONTENT_CHUNK_OUT_OF_SEQUENCE)
}

func Test_server_sends_Close_when_client_sends_an_additional_ContentChunk(t *testing.T) {
	server := newWebsocketServer(t, testAddress)
	server.constraints.ChunkSize = uint64(1)
	conn, client, _, done := getConnClientHello(server)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: 1,
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte("_"),
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  1,
		Data:      []byte("_"),
	})
	conn.expectClose(pb.Close_REASON_INVALID_CLIENT_MESSAGE)
}

func Test_server_sends_Close_when_client_sends_out_of_sequence_ContentChunk(t *testing.T) {
	server := newWebsocketServer(t, testAddress)
	server.constraints.ChunkSize = uint64(1)
	conn, client, _, done := getConnClientHello(server)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: 2,
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  1,
		Data:      []byte("_"),
	})
	conn.expectClose(pb.Close_REASON_CONTENT_CHUNK_OUT_OF_SEQUENCE)
}

func Test_server_sends_Close_when_client_sends_ContentChunk_with_invalid_size(t *testing.T) {
	server := newWebsocketServer(t, testAddress)
	server.constraints.ChunkSize = uint64(4)
	conn, client, _, done := getConnClientHello(server)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: 6,
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte("123"),
	})
	conn.expectClose(pb.Close_REASON_INVALID_CHUNK_SIZE)
}

func Test_server_sends_Close_when_client_sends_last_ContentChunk_with_invalid_size(t *testing.T) {
	server := newWebsocketServer(t, testAddress)
	server.constraints.ChunkSize = uint64(4)
	conn, client, _, done := getConnClientHello(server)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: 6,
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte("1234"),
	})
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  1,
		Data:      []byte("5"),
	})
	conn.expectClose(pb.Close_REASON_INVALID_CHUNK_SIZE)
}

func Test_server_sends_Close_after_calling_Client_Close(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	client.assertRunning()
	client.Close()
	close := conn.readClose()
	assert.Equal(t, pb.Close_REASON_CLOSED, close.Reason)
}

func Test_Client_terminates_after_calling_Client_Close(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	client.assertRunning()
	client.Close()
	conn.readClose()
	client.waitForExit()
	waitForChanClose(t, client.Closed(), nil)
}

func Test_Client_Close_does_nothing_when_Client_is_already_closed(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	client.assertRunning()
	client.Close()
	conn.readClose()
	client.waitForExit()
	client.Close()
}

func Test_server_does_not_send_RequestClosed_when_client_response_is_completed_and_times_out(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.sendCompletedResponse(request.ID())
	// Wait long enough that a Close message may have been sent.
	// The deferred done() above will check that there are no more messages.
	time.Sleep(2 * server.intervals.ClientTimeout)
}

func Test_Request_Success_returns_error_after_client_sent_CloseResponse(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: 1,
	})
	conn.writeCloseResponse(&pb.CloseResponse{
		RequestId: request.ID(),
	})
	time.Sleep(server.intervals.ClientTimeout / 2)
	err = request.Success()
	assert.ErrorIs(t, err, ErrRequestClosed)
	client.expectActiveRequests(0)
}

func Test_Request_Success_returns_error_after_calling_Client_Close(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: 1,
	})
	client.Close()
	time.Sleep(server.intervals.ClientTimeout / 2)
	err = request.Success()
	assert.ErrorIs(t, err, ErrClientClosed)
	conn.readClose()
}

func Test_Request_Success_returns_error_when_called_twice(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.sendCompletedResponse(request.ID())
	waitForChanValue(t, request.Response(), nil)
	err = request.Success()
	assert.NoError(t, err)
	err = request.Success()
	assert.ErrorIs(t, err, ErrRequestDeleted)
	conn.readSuccess()
}

func Test_Request_Close_returns_no_error_when_called_twice(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	assert.NoError(t, request.Close(""))
	assert.NoError(t, request.Close(""))
	conn.readRequestClosed()
}

func Test_Request_Close_returns_no_error_after_client_sent_CloseResponse(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeCloseResponse(&pb.CloseResponse{
		RequestId: request.ID(),
	})
	waitForChanClose(t, request.Closed(), nil)
	assert.NoError(t, request.Close(""))
	conn.readClose()
	assertNoChanValue(t, request.Completed())
}

func Test_Request_Close_returns_error_after_request_is_completed(t *testing.T) {
	_, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.sendCompletedResponse(request.ID())
	waitForChanValue(t, request.Response(), nil)
	waitForChanClose(t, request.Completed(), nil)
	err = request.Close("")
	assert.ErrorIs(t, err, ErrRequestCompleted)
}

func Test_server_does_not_send_Close_when_client_sends_response_after_request_is_closed(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	request.Close("")
	conn.readRequestClosed()
	conn.sendCompletedResponse(request.ID())
	time.Sleep(2 * server.intervals.ClientTimeout)
	client.expectActiveRequests(0)
}

func Test_server_does_not_send_Close_when_client_sends_last_ContentChunk_after_request_is_closed(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	conn.writeContentHeader(&pb.ContentHeader{
		RequestId:   request.ID(),
		ContentType: testContentType,
		ContentSize: uint64(1),
	})
	waitForChanValue(t, request.Response(), nil)
	request.Close("")
	conn.readRequestClosed()
	conn.writeContentChunk(&pb.ContentChunk{
		RequestId: request.ID(),
		Sequence:  0,
		Data:      []byte("_"),
	})
	time.Sleep(2 * server.intervals.ClientTimeout)
	client.expectActiveRequests(0)
}

func Test_server_does_not_send_Close_when_client_sends_EmptyResponse_after_request_is_closed(t *testing.T) {
	server, conn, client, _, done := getServerConnClientHello(t)
	defer done()
	request, err := client.request(testPath)
	assert.NoError(t, err)
	conn.readRequest()
	request.Close("")
	conn.readRequestClosed()
	conn.writeEmptyResponse(&pb.EmptyResponse{
		RequestId: request.ID(),
	})
	time.Sleep(2 * server.intervals.ClientTimeout)
	client.expectActiveRequests(0)
}

// ---

func assertNoChanValue[T interface{}](
	t *testing.T,
	valueChan <-chan T,
) {
	select {
	case <-valueChan:
		assert.Fail(t, "Expected channel to not supply a value")
	default:
	}
}

// Same as waitFor(), but expects the channel to get closed.
func waitForChanClose[T interface{}](
	t *testing.T,
	valueChan <-chan T,
	errChan <-chan error,
) {
	_, ok := waitForChan(t, valueChan, errChan)
	assert.False(t, ok, "Expected channel to be closed")
}

// Same as waitForNullableValue(),
// but expects the value to never be nil.
func waitForChanValue[T interface{}](
	t *testing.T,
	valueChan <-chan T,
	errChan <-chan error,
) T {
	value := waitForNullableChanValue(t, valueChan, errChan)
	assert.NotNil(t, value, "Expected channel value to not be nil")
	return value
}

// Same as waitFor(), but expects the channel to not get closed
// and therefore always return a value.
func waitForNullableChanValue[T interface{}](
	t *testing.T,
	valueChan <-chan T,
	errChan <-chan error,
) T {
	value, ok := waitForChan(t, valueChan, errChan)
	assert.True(t, ok, "Expected channel to not be closed")
	return value
}

// Waits for a value from a channel (or its closure, if it is closed)
// and attempts to read any messages from the websocket connection,
// including Close protocol messages, which might contain an error.
// The expectation is, that no messages are written to the connection,
// so any message other than a Close message that might indicate an error
// is considered unexpected and an error.
func waitForChan[T interface{}](
	t *testing.T,
	valueChan <-chan T,
	errChan <-chan error,
) (value T, ok bool) {
	select {
	case value, ok = <-valueChan:
	case err := <-errChan:
		assert.FailNow(t, err.Error())
	case <-time.After(testTimeout):
		assert.FailNow(t, "Timed out waiting for a channel value")
	}
	return
}

func getConnClient(server *websocketServer) (
	conn *websocketConn,
	client *wrappedClient,
	done func(),
) {
	conn = server.connect()
	client = server.lastClient()
	done = func() {
		time.Sleep(testAfterWait)
		conn.done()
		server.done()
	}
	return
}

func getConnClientHello(server *websocketServer) (
	conn *websocketConn,
	client *wrappedClient,
	hello *pb.Hello,
	done func(),
) {
	conn = server.connect()
	client = server.lastClient()
	hello = conn.readHello()
	client.setSecret(hello.ConnectionSecret)
	done = func() {
		time.Sleep(testAfterWait)
		conn.done()
		server.done()
	}
	return
}

func getServerConnClient(t *testing.T) (
	server *websocketServer,
	conn *websocketConn,
	client *wrappedClient,
	done func(),
) {
	server = newWebsocketServer(t, testAddress)
	conn = server.connect()
	client = server.lastClient()
	done = func() {
		time.Sleep(testAfterWait)
		conn.done()
		server.done()
	}
	return
}

func getServerConnClientHello(t *testing.T) (
	server *websocketServer,
	conn *websocketConn,
	client *wrappedClient,
	hello *pb.Hello,
	done func(),
) {
	server = newWebsocketServer(t, testAddress)
	conn = server.connect()
	client = server.lastClient()
	hello = conn.readHello()
	client.setSecret(hello.ConnectionSecret)
	done = func() {
		time.Sleep(testAfterWait)
		conn.done()
		server.done()
		client.waitForExit()
	}
	return
}

func expectedServerMessage[T interface{}, P *T](
	t *testing.T,
	message *pb.ServerMessage,
) P {
	switch m := message.Data.(type) {
	case P:
		return m
	default:
	}
	switch m := message.Data.(type) {
	case *pb.ServerMessage_Close:
		assert.FailNow(t, "Unexpected close message", "%v", m.Close)
		panic("unreachable")
	default:
		assert.FailNow(t, "Unexpected message type", "%T %#v",
			message.Data, message.Data)
		panic("unreachable")
	}
}

type websocketConn struct {
	t             *testing.T
	conn          *websocket.Conn
	incoming      chan *pb.ServerMessage
	lastError     chan error
	loopDone      chan struct{}
	waitCloseDone chan struct{}
	connClosed    chan struct{}
	hasWaitClose  bool
}

func newWebsocketConn(
	t *testing.T,
	conn *websocket.Conn,
) *websocketConn {
	wsConn := &websocketConn{
		t:             t,
		conn:          conn,
		incoming:      make(chan *pb.ServerMessage, 256),
		lastError:     make(chan error, 1),
		loopDone:      make(chan struct{}),
		waitCloseDone: make(chan struct{}),
		connClosed:    make(chan struct{}),
		hasWaitClose:  false,
	}
	go wsConn.loop()
	return wsConn
}

func (c *websocketConn) isClosed() bool {
	select {
	case <-c.connClosed:
		return true
	default:
		return false
	}
}

func (c *websocketConn) loop() {
	defer func() {
		close(c.loopDone)
		// Close the incoming channel, which will cause any calls to read*()
		// to stop waiting (since waitFor() returns if the channel is closed),
		// and then allow the goroutine of waitCloseErr() to exit.
		close(c.incoming)
	}()
	for {
		message, err := websocketReadServerMessage[pb.ServerMessage](
			c.conn, 2*testTimeout)
		if websocket.IsUnexpectedCloseError(err) {
			close(c.connClosed)
			return
		}
		if err != nil {
			if strings.Contains(err.Error(), "use of closed network connection") {
				return
			}
			c.lastError <- err
			return
		}
		c.incoming <- message
	}
}

func (c *websocketConn) readHello() (message *pb.Hello) {
	value, ok := waitForChan(c.t, c.incoming, nil)
	if !ok {
		return nil
	}
	return expectedServerMessage[pb.ServerMessage_Hello](c.t, value).Hello
}

func (c *websocketConn) readRequest() *pb.Request {
	value, ok := waitForChan(c.t, c.incoming, nil)
	if !ok {
		return nil
	}
	return expectedServerMessage[pb.ServerMessage_Request](c.t, value).Request
}

func (c *websocketConn) readSuccess() *pb.Success {
	value, ok := waitForChan(c.t, c.incoming, nil)
	if !ok {
		return nil
	}
	return expectedServerMessage[pb.ServerMessage_Success](c.t, value).Success
}

func (c *websocketConn) readRequestClosed() *pb.RequestClosed {
	value, ok := waitForChan(c.t, c.incoming, nil)
	if !ok {
		return nil
	}
	return expectedServerMessage[pb.ServerMessage_RequestClosed](c.t, value).RequestClosed
}

func (c *websocketConn) readClose() *pb.Close {
	value, ok := waitForChan(c.t, c.incoming, nil)
	if !ok {
		return nil
	}
	return expectedServerMessage[pb.ServerMessage_Close](c.t, value).Close
}

func (c *websocketConn) expectClose(expectedReason pb.Close_Reason) {
	m := c.readClose()
	assert.Equal(c.t, expectedReason, m.Reason)
}

// Reads a possible Close Message from the server and packages it as an error.
// The message will be written to the returned channel,
// if a Close message is ever received.
func (c *websocketConn) waitCloseErr() chan error {
	if c.hasWaitClose {
		assert.FailNow(c.t, "already waiting for Close message")
	}
	c.hasWaitClose = true
	out := make(chan error, 1)
	go func() {
		defer close(c.waitCloseDone)
		message := c.readClose()
		if message != nil {
			out <- fmt.Errorf("Received an unexpected Close message: %v (%v)",
				message.Message, message.Reason)
		}
	}()
	return out
}

func (c *websocketConn) write(message *pb.ClientMessage) {
	data, err := proto.Marshal(message)
	assert.NoError(c.t, err)
	err = c.conn.WriteMessage(websocket.BinaryMessage, data)
	assert.NoError(c.t, err)
}

func (c *websocketConn) writeEmptyResponse(message *pb.EmptyResponse) {
	c.write(&pb.ClientMessage{
		Data: &pb.ClientMessage_EmptyResponse{
			EmptyResponse: message,
		},
	})
}

func (c *websocketConn) writeContentHeader(message *pb.ContentHeader) {
	c.write(&pb.ClientMessage{
		Data: &pb.ClientMessage_ContentHeader{
			ContentHeader: message,
		},
	})
}

func (c *websocketConn) writeContentChunk(message *pb.ContentChunk) {
	c.write(&pb.ClientMessage{
		Data: &pb.ClientMessage_ContentChunk{
			ContentChunk: message,
		},
	})
}

func (c *websocketConn) writeCloseResponse(message *pb.CloseResponse) {
	c.write(&pb.ClientMessage{
		Data: &pb.ClientMessage_CloseResponse{
			CloseResponse: message,
		},
	})
}

// Helper for sending a ContentHeader followed by a single-byte ContentChunk
// which aims to enter the "completed response" request state easily.
func (c *websocketConn) sendCompletedResponse(requestId uint64) {
	data := []byte("!")
	c.writeContentHeader(&pb.ContentHeader{
		RequestId:   requestId,
		ContentType: testContentType,
		ContentSize: uint64(len(data)),
	})
	c.writeContentChunk(&pb.ContentChunk{
		RequestId: requestId,
		Sequence:  0,
		Data:      data,
	})
}

func (c *websocketConn) close() {
	c.conn.Close()
}

// Closes the connection and checks that there are no more server messages,
// since that is what is expected at the end of each test.
func (c *websocketConn) done() {
	c.conn.Close()
	<-c.loopDone
	if c.hasWaitClose {
		<-c.waitCloseDone
	}
	select {
	case message, ok := <-c.incoming:
		if ok {
			assert.FailNowf(c.t, "Did not expect another server message",
				"%v", message)
		}
	default:
	}
	select {
	case err := <-c.lastError:
		assert.FailNowf(c.t, "Error reading messages from connection",
			"%v", err.Error())
	default:
	}
}

func computeMac(
	path string,
	client_id string,
	client_secret []byte,
) ([]byte, error) {
	mac := hmac.New(sha256.New, client_secret)
	path = strings.TrimLeft(path, "/")
	message := client_id + "/" + path
	n, err := mac.Write([]byte(message))
	if err != nil {
		return nil, err
	}
	if n != len(message) {
		return nil, errors.New("write did not write all bytes")
	}
	return mac.Sum(nil), nil
}

type wrappedClient struct {
	Client
	t      *testing.T
	exited chan struct{}
	secret []byte
}

func (c *wrappedClient) setSecret(secret []byte) {
	c.secret = secret
}

// Wrapper for Client.Request(),
// which computes a valid MAC for the given path,
// so that the caller does not have to.
func (c *wrappedClient) request(path string) (Request, error) {
	if c.secret == nil {
		return nil, errors.New("wrapped client secret may not be nil")
	}
	mac, err := computeMac(path, c.ID().UrlEncode(), c.secret)
	if err != nil {
		return nil, err
	}
	return c.Request(path, mac)
}

// Waits for the client to have exited the run loop.
// Useful to make sure it actually stops running,
// when it is expected to stop running.
func (c *wrappedClient) waitForExit() {
	waitForChanClose(c.t, c.exited, nil)
}

func (c *wrappedClient) assertRunning() {
	select {
	case <-c.exited:
		assert.FailNow(c.t, "the client is not running anymore")
	default:
	}
}

func (c *wrappedClient) expectActiveRequests(expected int) {
	actual, err := c.ActiveRequests()
	assert.NoError(c.t, err)
	assert.Equal(c.t, expected, actual)
}

type websocketServer struct {
	t           *testing.T
	clients     chan *wrappedClient
	errors      chan error
	httpServer  *httptest.Server
	upgrader    websocket.Upgrader
	constraints *pb.Constraints
	intervals   *ProtocolIntervals
	mockAddress string
}

// Creates a new testServer instance.
// Constraints and intervals may be changed before calling any of its methods.
func newWebsocketServer(t *testing.T, mockAddress string) *websocketServer {
	server := &websocketServer{
		t:           t,
		clients:     make(chan *wrappedClient, 256),
		errors:      make(chan error, 256),
		httpServer:  nil,
		upgrader:    websocket.Upgrader{},
		constraints: newConstraints(),
		intervals:   newIntervals(),
		mockAddress: mockAddress,
	}
	server.httpServer = httptest.NewServer(server.getHandler())
	return server
}

// Closes the server and checks that there are no errors queued
// or further clients connected, since that is what is expected
// at the end of each test.
func (s *websocketServer) done() {
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		s.httpServer.Close()
		wg.Done()
	}()
	select {
	case <-s.clients:
		assert.FailNow(s.t, "There are more clients connected to the server than expected")
	case err := <-s.errors:
		assert.Error(s.t, err)
	default:
	}
	wg.Wait()
}

// The test websocket server handler upgrades any http client connection
// to a websocket connection, creates a new Client instance,
// runs its run loop and registers it for retrieval with lastClient().
func (s *websocketServer) getHandler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := s.upgrader.Upgrade(w, r, nil)
		if err != nil {
			s.errors <- err
			return
		}
		options := &ProtocolOptions{
			BaseUrl:         s.mockAddress,
			ChunkBufferSize: 8,
			Constraints:     constraintsFromProto(s.constraints),
			Intervals:       s.intervals,
		}
		if err := options.Validate(); err != nil {
			s.errors <- err
			return
		}
		client, err := NewClient(conn, options)
		if err != nil {
			s.errors <- err
			return
		}
		exited := make(chan struct{})
		c := &wrappedClient{
			Client: client,
			t:      s.t,
			exited: exited,
			secret: nil,
		}
		go func() {
			client.Run()
			close(exited)
		}()
		s.clients <- c
	})
}

// Creates a websocket connection to the server
func (s *websocketServer) connect() *websocketConn {
	h := httptest.NewServer(s.getHandler())
	address := "ws" + strings.TrimPrefix(h.URL, "http")
	conn, _, err := websocket.DefaultDialer.Dial(address, nil)
	assert.NoError(s.t, err)
	return newWebsocketConn(s.t, conn)
}

// Returns the last client that connected to the test websocket server
// or returns a timeout error, if no client connected
// within the test timeout period.
func (s *websocketServer) lastClient() *wrappedClient {
	// First try get a client that might already be registered.
	select {
	case client := <-s.clients:
		return client
	default:
	}
	// Then wait for a client, an error or the test timeout.
	select {
	case client := <-s.clients:
		return client
	case err := <-s.errors:
		assert.NoError(s.t, err)
		assert.FailNow(s.t, "cannot proceed")
	case <-time.After(testTimeout):
		assert.Error(s.t, errTestTimeout)
		assert.FailNow(s.t, "cannot proceed")
	}
	panic("unreachable")
}

type websocketMessageReader[T interface{}, P *T] struct {
	conn        *websocket.Conn
	readTimeout time.Duration
}

func newWebsocketMessageReader[T interface{}, P *T](
	conn *websocket.Conn,
	readTimeout time.Duration,
) *websocketMessageReader[T, P] {
	return &websocketMessageReader[T, P]{
		conn:        conn,
		readTimeout: readTimeout,
	}
}

func (r *websocketMessageReader[T, P]) readMessage() ([]byte, error) {
	err := r.conn.SetReadDeadline(time.Now().Add(r.readTimeout))
	if err != nil {
		return nil, err
	}
	kind, data, err := r.conn.ReadMessage()
	if err != nil {
		return nil, err
	}
	if kind != websocket.BinaryMessage {
		return nil, errNotABinaryMessage
	}
	return data, nil
}

func (r *websocketMessageReader[T, P]) readServerMessage() (P, error) {
	data, err := r.readMessage()
	if err != nil {
		return nil, err
	}
	result, err := unmarshalServerMessage[T](data)
	if err != nil {
		return nil, err
	}
	return result, nil
}

func websocketReadServerMessage[T interface{}, P *T](
	conn *websocket.Conn,
	readTimeout time.Duration,
) (P, error) {
	reader := newWebsocketMessageReader[T](conn, readTimeout)
	message, err := reader.readServerMessage()
	if err != nil {
		return nil, err
	}
	return message, nil
}

func unmarshalServerMessage[T interface{}, P *T](data []byte) (P, error) {
	msg := &pb.ServerMessage{}
	err := proto.Unmarshal(data, msg)
	if err != nil {
		return nil, err
	}
	var msgi interface{} = msg
	switch message := msgi.(type) {
	case P:
		return message, nil
	}
	switch message := msg.Data.(type) {
	case P:
		return message, nil
	}
	switch message := msg.Data.(type) {
	case *pb.ServerMessage_Close:
		return nil, fmt.Errorf("Unexpected close message: %v (%v)",
			message.Close.Message, message.Close.Reason)
	default:
		return nil, fmt.Errorf("Unexpected message %T: %v", message, message)
	}
}

func newConstraints() *pb.Constraints {
	return proto.Clone(defaultConstraints).(*pb.Constraints)
}

func constraintsFromProto(
	c *pb.Constraints,
) *ProtocolConstraints {
	duration := time.Duration(c.CacheDuration) * time.Second
	return &ProtocolConstraints{
		ChunkSize:            c.ChunkSize,
		MaxContentSize:       c.MaxContentSize,
		AcceptedContentTypes: c.AcceptedContentTypes,
		CacheDuration:        &duration,
	}
}

func newIntervals() *ProtocolIntervals {
	copy := *defaultIntervals
	return &copy
}

func strPtr(s string) *string {
	return &s
}
