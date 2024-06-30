#include "client.h"

#include <cassert>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include <google/protobuf/text_format.h>
#include <hv/base64.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "loon/client.h"

using namespace loon;

#define RECONNECT_DELAY_POLICY_EXPONENTIAL 2

#define _DEFAULT_CONNECT_TIMEOUT 5000
#define _DEFAULT_PING_INTERVAL 20000
#define _DEFAULT_RECONNECT_MIN_DELAY 1000
#define _DEFAULT_RECONNECT_MAX_DELAY 30000
#define _DEFAULT_RECONNECT_DELAY_POLICY RECONNECT_DELAY_POLICY_EXPONENTIAL

ClientImpl::ClientImpl(
    std::string const& address, std::optional<std::string> const& auth)
    : m_address{ address }, m_auth{ auth }
{
    m_conn.onopen = std::bind(&ClientImpl::on_websocket_open, this);
    m_conn.onmessage = std::bind(
        &ClientImpl::on_websocket_message, this, std::placeholders::_1);
    m_conn.onclose = std::bind(&ClientImpl::on_websocket_close, this);
    m_conn.setConnectTimeout(_DEFAULT_CONNECT_TIMEOUT);
    m_conn.setPingInterval(_DEFAULT_PING_INTERVAL);
}

void ClientImpl::start()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    internal_start();
}

void ClientImpl::stop()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
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

std::string ClientImpl::make_url(std::string const& path)
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

void ClientImpl::check_content_constraints(
    std::shared_ptr<loon::ContentSource> source, loon::ContentInfo const& info)
{
    // Maximum content size.
    auto max_content_size = m_hello->constraints().max_content_size();
    if (source->size() > max_content_size) {
        throw UnacceptableContentException(
            "content of this size is not accepted by the server: " +
            std::to_string(source->size()) + " bytes");
    }

    // Allowed content types.
    // NOTE server does not support wildcard content types yet,
    // but it should and will in the future, which needs to be handled here.
    std::string content_type = source->content_type();
    auto separator_index = content_type.find(';');
    if (separator_index != std::string::npos) {
        content_type.resize(separator_index);
    }
    auto const& accepted_content_types =
        m_hello->constraints().accepted_content_types();
    bool accepted = false;
    for (auto const& accepted_type : accepted_content_types) {
        if (content_type == accepted_type) {
            accepted = true;
            break;
        }
    }
    if (!accepted) {
        throw UnacceptableContentException(
            "the content type is not accepted by the server");
    }

    // Attachment filename may not be empty.
    if (info.attachment_filename.has_value() &&
        info.attachment_filename.value().empty()) {
        throw UnacceptableContentException(
            "the attachment filename may not be empty");
    }
}

std::shared_ptr<ContentHandle> ClientImpl::register_content(
    std::shared_ptr<loon::ContentSource> source, loon::ContentInfo const& info)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    // TODO the content must be registered permanently, across restarts.
    //   actually, no: notify caller of restart, then invalidate.
    //   caller needs to register again or not at all.
    // TODO configurable timeout until Hello should be received.
    // TODO configurable "failed too much" count,
    //   after which the client stops reconnecting
    // TODO upload speed limit (will help with testing too!)

    // Check that the content is within the server's constraints.
    check_content_constraints(source, info);

    // Check if the path is already in use.
    auto const& path = info.path;
    auto it = m_content.find(path);
    if (it != m_content.end()) {
        throw ContentNotRegisteredException(
            "content under this path is already registered with this client");
    }

    // Wait until the connection is ready for content registration.
    m_cv_connection_ready.wait(lock, [this] {
        return !m_connected.load() || m_hello.has_value();
    });
    if (!m_connected.load()) {
        throw ClientNotConnectedException("the client is not connected");
    }
    assert(m_hello.has_value());

    // Serve requests for this content, register it and return a handle.
    auto send = std::bind(&ClientImpl::send, this, std::placeholders::_1);
    auto request_handle =
        std::make_shared<RequestHandle>(info, source, m_hello.value(), send);
    request_handle->spawn_serve_thread();
    auto handle = std::make_shared<InternalContentHandle>(
        make_url(info.path), path, request_handle);
    m_content.emplace(path, handle);
    return handle;
}

void ClientImpl::unregister_content(std::shared_ptr<ContentHandle> handle)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    // Verify that the content is valid and that it is registered.
    auto ptr = std::dynamic_pointer_cast<InternalContentHandle>(handle);
    if (!ptr) {
        throw ContentNotRegisteredException(
            "failed to cast handle to internal content handle type");
    }
    auto it = m_content.find(ptr->path());
    if (it == m_content.end()) {
        throw ContentNotRegisteredException(
            "this content is not registered with this client");
    }

    // Wait until the connection is ready.
    m_cv_connection_ready.wait(lock, [this] {
        return !m_connected.load() || m_hello.has_value();
    });
    if (!m_connected.load()) {
        throw ClientNotConnectedException("the client is not connected");
    }

    // Notify that the content is being unregistered (rather early than late).
    it->second->unregistered();

    // Exit the request handle's serve thread
    // and wait for it to have terminated, then remove the handle.
    it->second->request_handle()->exit_gracefully();
    m_content.erase(it);
}

bool ClientImpl::send(ClientMessage const& message)
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

void ClientImpl::on_hello(Hello const& hello)
{
    if (m_hello.has_value()) {
        // Already received a Hello message for this connection.
        // TODO log invalid second hello message
        return internal_restart();
    }
    m_hello = hello;
    m_cv_connection_ready.notify_all();
}

void ClientImpl::on_request(Request const& request)
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

void ClientImpl::on_success(Success const& success)
{
    // TODO
    std::cerr << "success (" << success.request_id() << ")\n";

    // TODO call served() on the content handle
    // TODO get content handle by request ID (two maps: path and request ID key)
}

void ClientImpl::on_request_closed(RequestClosed const& request_closed)
{
    // TODO
    std::cerr << "request closed (" << request_closed.request_id()
              << "): " << request_closed.message() << "\n";

    // TODO cancel sending response.
}

void ClientImpl::on_close(Close const& close)
{
    // TODO log close reason
    std::cerr << "close message: " << close.message() << "\n";

    // Restart the connection, if the server closed the connection.
    internal_restart();
}

void ClientImpl::on_websocket_open()
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    // TODO better logging
    std::cerr << "connection opened\n";
}

void ClientImpl::on_websocket_close()
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    // TODO better logging
    std::cerr << "connection closed\n";

    update_connected(false);
    reset_connection_state();
}

void ClientImpl::on_websocket_message(std::string const& message)
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

void ClientImpl::handle_message(ServerMessage const& message)
{
    switch (message.data_case()) {
    case ServerMessage::kHello:
        return on_hello(message.hello());
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

bool ClientImpl::update_connected(bool state)
{
    auto old_state = m_connected.exchange(state);
    // Notify any thread that might be waiting for connection state changes.
    if (old_state != state) {
        m_cv_connection_ready.notify_all();
    }
    return old_state;
}

void ClientImpl::internal_start()
{
    if (update_connected(true)) {
        return;
    }
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

void ClientImpl::reset_connection_state()
{
    for (auto& [_, content] : m_content) {
        // Notify that content is not registered anymore.
        content->unregistered();
        // Make sure all spawned request handler threads exit.
        content->request_handle()->destroy();
    }
    m_content.clear();
    m_hello = std::nullopt;
}

void ClientImpl::internal_stop()
{
    if (!update_connected(false)) {
        return;
    }
    m_conn.close();
    reset_connection_state();
}

inline void ClientImpl::internal_restart()
{
    internal_stop();
    internal_start();
}

Client::Client(std::string const& address)
    : m_impl{ std::make_unique<ClientImpl>(address, std::nullopt) }
{
}

loon::Client::Client(std::string const& address, std::string const& auth)
    : m_impl{ std::make_unique<ClientImpl>(address, auth) }
{
}

void Client::start() { return m_impl->start(); }

void Client::stop() { return m_impl->stop(); }

std::shared_ptr<ContentHandle> Client::register_content(
    std::shared_ptr<loon::ContentSource> source, loon::ContentInfo const& info)
{
    return m_impl->register_content(source, info);
}

void Client::unregister_content(std::shared_ptr<ContentHandle> handle)
{
    return m_impl->unregister_content(handle);
}
