#include <iostream>
#include <sstream>
#include <thread>

#include <google/protobuf/text_format.h>
#include <hv/base64.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "loon/loon.h"

#define RECONNECT_DELAY_POLICY_EXPONENTIAL 2

#define _DEFAULT_CONNECT_TIMEOUT 5000
#define _DEFAULT_PING_INTERVAL 20000
#define _DEFAULT_RECONNECT_MIN_DELAY 1000
#define _DEFAULT_RECONNECT_MAX_DELAY 30000
#define _DEFAULT_RECONNECT_DELAY_POLICY RECONNECT_DELAY_POLICY_EXPONENTIAL

loon::Client::Client(
    std::string const& address, std::optional<std::string> const& auth)
    : m_address{ address }, m_auth{ auth }
{
    m_conn.onopen = std::bind(&loon::Client::on_websocket_open, this);
    m_conn.onmessage = std::bind(
        &loon::Client::on_websocket_message, this, std::placeholders::_1);
    m_conn.onclose = std::bind(&loon::Client::on_websocket_close, this);
    m_conn.setConnectTimeout(_DEFAULT_CONNECT_TIMEOUT);
    m_conn.setPingInterval(_DEFAULT_PING_INTERVAL);
}

void loon::Client::start()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_connected.exchange(true)) {
        return;
    }
    internal_start();
}

void loon::Client::stop()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected.exchange(false)) {
        return;
    }
    internal_stop();
}

static std::string hmac_sha256(std::string_view msg, std::string_view key)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash;
    unsigned int size;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
        reinterpret_cast<unsigned char const*>(msg.data()),
        static_cast<int>(msg.size()), hash.data(), &size);
    return std::string{ reinterpret_cast<char const*>(hash.data()), size };
}

static std::string base64_raw_url_encode(std::string const& text)
{
    auto data = reinterpret_cast<const unsigned char*>(text.data());
    auto result = hv::Base64Encode(data, text.size());
    size_t new_size = result.size();
    for (size_t i = 0; i < result.size(); i++) {
        if (result[i] == '=') {
            // Strip the padding at the end.
            new_size = i;
            break;
        }
        switch (result[i]) {
        case '+':
            result[i] = '-';
            break;
        case '/':
            result[i] = '_';
            break;
        }
    }
    result.resize(new_size);
    return result;
}

std::string loon::Client::make_url(std::string const& path)
{
    std::ostringstream oss;
    oss << m_hello->client_id() << "/" << path;
    auto mac = hmac_sha256(oss.str(), m_hello->connection_secret());
    auto mac_encoded = base64_raw_url_encode(mac);
    oss.str("");
    oss.clear();
    oss << m_hello->base_url() << "/" << m_hello->client_id() << "/"
        << mac_encoded << "/" << path;
    return oss.str();
}

std::shared_ptr<loon::Client::ContentHandle> loon::Client::register_content(
    std::shared_ptr<ContentSource> source, ContentInfo const& info)
{
    // TODO the content must be registered permanently, across restarts.
    //   actually, no: notify caller of restart, then invalidate.
    //   caller needs to register again or not at all.
    // TODO derive URL from: client ID, client secret, computed MAC, path
    //   using HMAC-SHA256 (use openssl?)
    // TODO check that the source and info is within the constraints.
    // TODO check that no content is already registered under this path.
    // TODO store the content handle in the internal map.
    // TODO destroy all request handles on: on_close() or internal_stop().
    // TODO spawn serve thread once hello has been received, if it hasn't yet.

    const std::lock_guard<std::mutex> lock(m_mutex);
    auto request_handle =
        std::make_shared<RequestHandle>(info, source, m_hello.value(),
            std::bind(&loon::Client::send, this, std::placeholders::_1));
    request_handle->spawn_serve_thread();
    auto handle = std::make_shared<InternalContentHandle>(
        make_url(info.path), request_handle);
    m_content[info.path] = handle;
    return handle;
}

void loon::Client::unregister_content(std::shared_ptr<ContentHandle> handle)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    // TODO
    // TODO cancel all ongoing and pending requests.
    // TODO "cancel then exit", graceful exit
}

bool loon::Client::send(ClientMessage const& message)
{
    auto result = message.SerializeAsString();
    if (result.empty()) {
        std::cerr << "loon/send: failed to serialize message";
        internal_restart();
        return false;
    }
    int n = m_conn.send(result.data(), result.size(), WS_OPCODE_BINARY);
    if (n <= 0) {
        std::cerr << "loon/send: failed to send message";
        internal_restart();
        return false;
    }
    // TODO remove
    std::string output;
    google::protobuf::TextFormat::PrintToString(message, &output);
    std::cerr << "send (" << n << "/" << result.size()
              << "): " << output.substr(0, output.find_first_of('{')) << "\n";
    return true;
}

void loon::Client::on_request(Request const& request)
{
    // TODO
    std::cerr << "request (" << request.id() << "): " << request.path() << "\n";

    auto it = m_content.find(request.path());
    if (it == m_content.end()) {
        ClientMessage message;
        auto empty_response = message.mutable_empty_response();
        empty_response->set_request_id(request.id());
        send(message);
        return;
    }

    auto handle = it->second->request_handle();
    handle->serve_request(request);
}

void loon::Client::on_success(Success const& success)
{
    // TODO
    std::cerr << "success (" << success.request_id() << ")\n";
}

void loon::Client::on_request_closed(RequestClosed const& request_closed)
{
    // TODO
    std::cerr << "request closed (" << request_closed.request_id()
              << "): " << request_closed.message() << "\n";
}

void loon::Client::on_close(Close const& close)
{
    // Restart the connection, if the server closed the connection.
    internal_restart();
    // TODO log close reason
    std::cerr << "close message: " << close.message() << "\n";
}

void loon::Client::on_websocket_open()
{
    // TODO better logging
    std::cerr << "connection opened\n";
}

void loon::Client::on_websocket_close()
{
    // TODO better logging
    std::cerr << "connection closed\n";
}

void loon::Client::on_websocket_message(std::string const& message)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    ServerMessage server_message;
    if (!server_message.ParseFromString(message)) {
        // Failed to parse message, restart the connection.
        // TODO implicitly log error with global protobuf error handler.
        return internal_restart();
    }
    handle_message(server_message);
}

void loon::Client::handle_message(ServerMessage const& message)
{
    if (message.has_hello()) {
        if (m_hello.has_value()) {
            // Already received a Hello message for this connection.
            // TODO log invalid second hello message
            return internal_restart();
        }
        m_hello = message.hello();
        return;
    }
    switch (message.data_case()) {
    case ServerMessage::kRequest:
        return on_request(message.request());
    case ServerMessage::kSuccess:
        return on_success(message.success());
    case ServerMessage::kRequestClosed:
        return on_request_closed(message.request_closed());
    case ServerMessage::kClose:
        return on_close(message.close());
    default:
        // Unexpected server message, restart the connection.
        // TODO log invalid server message (including message type)
        return internal_restart();
    }
}

void loon::Client::internal_start()
{
    // Reconnect
    reconn_setting_t reconnect;
    reconn_setting_init(&reconnect);
    reconnect.min_delay = _DEFAULT_RECONNECT_MIN_DELAY;
    reconnect.max_delay = _DEFAULT_RECONNECT_MAX_DELAY;
    reconnect.delay_policy = _DEFAULT_RECONNECT_DELAY_POLICY;
    m_conn.setReconnect(&reconnect);
    // Authorization
    http_headers headers;
    if (m_auth.has_value()) {
        auto value = m_auth.value();
        std::string auth = hv::Base64Encode(
            reinterpret_cast<const unsigned char*>(value.c_str()),
            value.size());
        headers["Authorization"] = "Basic " + auth;
    }
    // Open the connection
    m_conn.open(m_address.c_str(), headers);
}

inline void loon::Client::internal_stop()
{
    m_conn.close();
    // Reset any per-connection state
    m_hello = std::nullopt;
}

inline void loon::Client::internal_restart()
{
    internal_stop();
    internal_start();
}
