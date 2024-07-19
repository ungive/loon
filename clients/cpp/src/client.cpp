#include "client.h"

#include <cassert>
#include <memory>
#include <sstream>
#include <thread>

#include <google/protobuf/text_format.h>

#include "logging.h"
#include "loon/client.h"
#include "util.h"

using namespace loon;
using namespace std::chrono_literals;

#define var loon::log_var
#define log(level)                           \
    if (LogLevel::level < loon::log_level()) \
        ;                                    \
    else                                     \
        make_logger(LogLevel::level)

Client::Client(std::string const& address, ClientOptions options)
    : m_impl{ std::make_unique<ClientImpl>(address, options) }
{
}

ClientImpl::ClientImpl(std::string const& address, ClientOptions options)
    : m_conn{ std::make_unique<websocket::Client>(address, options.websocket) },
      m_options{ options }
{
    if (m_options.min_cache_duration.has_value() &&
        m_options.min_cache_duration.value() <=
            std::chrono::milliseconds::zero()) {
        throw std::runtime_error(
            "min_cache_duration must be greater than zero");
    }
    if (m_options.no_content_request_limit.has_value()) {
        auto limit = m_options.no_content_request_limit.value();
        if (limit.first == -1 &&
            limit.second == std::chrono::milliseconds::zero()) {
            // TODO: perhaps make this a warning in the future. for now
            // it's better to not give this field a default value though.
            throw std::runtime_error(
                "no_content_request_limit must be explicitly set to a value");
        }
        if (limit.first <= 0 ||
            limit.second <= std::chrono::milliseconds::zero()) {
            throw std::runtime_error("no_content_request_limit pair values "
                                     "must be greater than zero");
        }
    }
    if (m_options.max_upload_speed.has_value() &&
        m_options.max_upload_speed.value() == 0) {
        throw std::runtime_error("max_upload_speed may not be zero");
    }
    if (m_options.max_upload_speed.has_value()) {
        throw std::runtime_error("max_upload_speed is not yet implemented");
    }
    // Event handlers
    m_conn->on_open(std::bind(&ClientImpl::on_websocket_open, this));
    m_conn->on_close(std::bind(&ClientImpl::on_websocket_close, this));
    m_conn->on_message(std::bind(
        &ClientImpl::on_websocket_message, this, std::placeholders::_1));
    // Logging
    loon::init_logging();
    // Start the send pump thread
    m_send_pump_thread = std::thread(&ClientImpl::send_pump, this);
    // Start the manager loop thread
    m_manager_loop_thread = std::thread(&ClientImpl::manager_loop, this);
}

ClientImpl::~ClientImpl()
{
    stop();
    // Stop the send pump thread
    {
        const std::lock_guard<std::mutex> lock(m_send_pump_comm_mutex);
        m_stop_send_pump = true;
        m_cv_send_pump.notify_all();
    }
    // Stop the manager loop thread
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stop_manager_loop = true;
        m_cv_manager.notify_all();
    }
    // Join all running threads
    m_send_pump_thread.join();
    m_manager_loop_thread.join();
}

void ClientImpl::start()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    internal_start();
}

void ClientImpl::stop()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    internal_stop(lock);
}

std::string ClientImpl::make_url(std::string const& path)
{
    std::ostringstream oss;
    oss << m_hello->client_id() << "/" << path;
    auto mac = util::hmac_sha256(oss.str(), m_hello->connection_secret());
    auto mac_encoded = util::base64_raw_url_encode(mac);
    oss.str("");
    oss.clear();
    oss << m_hello->base_url() << "/" << m_hello->client_id() << "/"
        << mac_encoded << "/" << path;
    return oss.str();
}

loon::Logger loon::ClientImpl::make_logger(LogLevel level)
{
    Logger logger(level);
    if (m_hello.has_value()) {
        logger.after() << var("cid", m_hello->client_id());
    }
    return logger;
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

bool loon::ClientImpl::wait_until_connected(
    std::unique_lock<std::mutex>& lock, std::chrono::milliseconds timeout)
{
    if (!m_started) {
        throw ClientNotStartedException("the client must be started");
    }
    return m_cv_connection_ready.wait_for(lock, timeout, [this] {
        return m_connected;
    });
}

std::shared_ptr<ContentHandle> ClientImpl::register_content(
    std::shared_ptr<loon::ContentSource> source, loon::ContentInfo const& info)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    // Check if the path is already in use.
    auto const& path = info.path;
    auto it = m_content.find(path);
    if (it != m_content.end()) {
        throw ContentNotRegisteredException(
            "content under this path is already registered with this client");
    }

    // Wait until the connection is ready for content registration.
    wait_until_ready(lock);

    // Check that the content is within the server's constraints.
    check_content_constraints(source, info);

    // Serve requests for this content, register it and return a handle.
    auto send = std::bind(&ClientImpl::send, this, std::placeholders::_1);
    RequestHandler::Options options;
#ifdef LOON_TEST
    options.chunk_sleep = m_chunk_sleep_duration;
#endif
    auto request_handle = std::make_shared<RequestHandler>(
        info, source, m_hello.value(), options, send);
    request_handle->spawn_serve_thread();
    auto handle = std::make_shared<InternalContentHandle>(
        make_url(info.path), request_handle);
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
    wait_until_ready(lock);

    // Notify that the content is being unregistered (rather early than late).
    it->second->unregistered();

    // Exit the request handle's serve thread
    // and wait for it to have terminated, then remove the handle.
    it->second->request_handler()->exit_gracefully();
    m_content.erase(it);
}

std::vector<std::shared_ptr<ContentHandle>> loon::ClientImpl::content()
{
    std::vector<std::shared_ptr<ContentHandle>> result;
    result.reserve(m_content.size());
    std::transform(m_content.begin(), m_content.end(),
        std::back_inserter(result),
        [](decltype(m_content)::value_type const& value) {
            return value.second;
        });
    return result;
}

void ClientImpl::on_hello(Hello const& hello)
{
#ifdef LOON_TEST
    if (m_injected_hello_modifer) {
        Hello modified = hello;
        m_injected_hello_modifer(modified);
        m_hello = modified;
    } else {
        m_hello = hello;
    }
#else
    m_hello = hello;
#endif

    if (!m_hello->has_constraints()) {
        log(Fatal) << "the server did not send any constraints";
        return fail();
    }
    if (m_options.min_cache_duration.has_value()) {
        auto min = m_options.min_cache_duration.value().count();
        auto value = m_hello->constraints().cache_duration();
        if (value == 0) {
            log(Fatal) << "the server does not support response caching";
            return fail();
        }
        if (min > value) {
            log(Fatal) << "the server does not cache responses long enough";
            return fail();
        }
    }

    m_cv_connection_ready.notify_all();
    log(Info) << "ready" << var("base_url", m_hello->base_url());
}

bool ClientImpl::check_request_limit(decltype(m_content)::iterator it)
{
    auto has_content = it != m_content.end();
    auto now = std::chrono::system_clock::now();
    if (has_content && m_options.min_cache_duration.has_value()) {
        auto& last_request = it->second->last_request;
        if (last_request.has_value() &&
            now - last_request.value() <=
                m_options.min_cache_duration.value()) {
            return true;
        }
        last_request = now;
    } else if (!has_content && m_options.no_content_request_limit.has_value()) {
        auto& history = m_no_content_request_history;
        auto const& limit = m_options.no_content_request_limit.value();
        for (auto it = history.begin(); it != history.end();) {
            auto then = *it;
            assert(then <= now);
            if (now - then <= limit.second) {
                break;
            }
            it = history.erase(it);
        }
        history.push_back(now);
        if (history.size() > limit.first) {
            return true;
        }
    }
    return false;
}

void ClientImpl::on_request(Request const& request)
{
    std::lock_guard<std::mutex> lock(m_request_mutex);

    if (m_requests.find(request.id()) != m_requests.end()) {
        log(Error) << "protocol: request id already in use"
                   << var("rid", request.id());
        return restart();
    }

    auto it = m_content.find(request.path());
    if (check_request_limit(it)) {
        log(Error) << "too many requests" << var("rid", request.id())
                   << var("path", request.path());
        return restart();
    }

    if (it == m_content.end()) {
        ClientMessage message;
        auto empty_response = message.mutable_empty_response();
        empty_response->set_request_id(request.id());
        send(message);
        return;
    }

    auto content = it->second;
    m_requests.emplace(request.id(), std::make_pair(content, false));
    content->request_handler()->serve_request(
        request, std::bind(&ClientImpl::response_sent, this, request.id()));
}

inline void ClientImpl::call_served_callback(decltype(m_requests)::iterator it)
{
    if (!it->second.second) {
        it->second.second = true;
        return;
    }
    auto request_id = it->first;
    auto content = it->second.first;
    content->served();
    log(Info) << "served request" << var("rid", request_id)
              << var("size", content->request_handler()->source()->size())
              << var("path", content->path());
    m_requests.erase(it);
}

void ClientImpl::response_sent(uint64_t request_id)
{
    const std::lock_guard<std::mutex> lock(m_request_mutex);

    auto it = m_requests.find(request_id);
    if (it == m_requests.end()) {
        log(Warning) << "served a request that is not registered anymore";
        assert(false);
        return;
    }
    call_served_callback(it);
}

void ClientImpl::on_success(Success const& success)
{
    const std::lock_guard<std::mutex> lock(m_request_mutex);

    auto request_id = success.request_id();
    auto it = m_requests.find(request_id);
    if (it == m_requests.end()) {
        log(Warning) << "received success for an unknown request id";
        assert(false);
        return;
    }
    call_served_callback(it);
}

void ClientImpl::on_request_closed(RequestClosed const& request_closed)
{
    const std::lock_guard<std::mutex> lock(m_request_mutex);

    auto request_id = request_closed.request_id();
    auto it = m_requests.find(request_id);
    if (it == m_requests.end()) {
        log(Warning)
            << "protocol: received request closed for an unknown request id";
        assert(false);
        return;
    }

    auto content = it->second.first;
    content->request_handler()->cancel_request(request_id);
    log(Warning) << "request failed" << var("rid", request_closed.request_id())
                 << var("message", request_closed.message());
    m_requests.erase(it);
}

void ClientImpl::on_close(Close const& close)
{
    // The server closed the connection.
    log(Error) << "connection closed by server"
               << var("message", close.message());
    restart();
}

void ClientImpl::wait_until_ready(std::unique_lock<std::mutex>& lock)
{
    // Use the connect timeout here, as it makes the perfect timeout value.
    // Receiving the Hello message should not take longer than connecting.
    m_cv_connection_ready.wait_for(lock, connect_timeout(), [this] {
        return !m_connected || m_hello.has_value();
    });
    if (!m_connected) {
        throw ClientNotConnectedException("the client is not connected");
    }
    if (!m_hello.has_value()) {
        throw ClientFailedException(
            "did not receive initial server message in time");
    }
}

void ClientImpl::on_websocket_open()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    update_connected(true);
    log(Debug) << "connected";
}

void ClientImpl::on_websocket_close()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    update_connected(false);
    reset_connection_state();
    log(Info) << "disconnected";
}

void ClientImpl::on_websocket_message(std::string const& message)
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    ServerMessage server_message;
    if (!server_message.ParseFromString(message)) {
        log(Error) << "failed to parse server message";
        return restart();
    }

    switch (server_message.data_case()) {
    case ServerMessage::kHello:
        if (m_hello.has_value()) {
            log(Error) << "protocol: received more than one Hello message";
            return restart();
        }
        break;
    default:
        if (!m_hello.has_value()) {
            log(Error) << "protocol: first message is not a Hello message";
            return restart();
        }
        break;
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
        log(Error) << "protocol: unrecognized server message"
                   << var("data_case", message.data_case());
        return restart();
    }
}

bool ClientImpl::send(ClientMessage const& message)
{
    const std::lock_guard<std::mutex> send_lock(m_send_pump_send_mutex);
    std::unique_lock<std::mutex> comm_lock(m_send_pump_comm_mutex);
    m_send_pump_message = &message;
    m_cv_send_pump.notify_one();            // notify message
    m_cv_send_pump.wait(comm_lock, [this] { // wait until sent
        return m_stop_send_pump || m_send_pump_message == nullptr;
    });
    if (m_stop_send_pump) {
        return false;
    }
    return m_send_pump_result;
}

void ClientImpl::send_pump()
{
    std::unique_lock<std::mutex> lock(m_send_pump_comm_mutex);
    while (true) {
        m_cv_send_pump.wait(
            lock, [this] { // wait for a message or until stopped
                return m_stop_send_pump || m_send_pump_message != nullptr;
            });
        if (m_stop_send_pump) {
            return;
        }
        bool is_sent = internal_send(*m_send_pump_message);
        m_send_pump_result = is_sent;
        m_send_pump_message = nullptr;
        m_cv_send_pump.notify_one(); // notify done
        {
            // While restarting, the mutex has to be unlocked,
            // so that it can be locked by the send method again,
            // once it receives the above condition variable signal.
            lock.unlock();
            if (!is_sent) {
                // If the send operation failed, restart the connection.
                // Do this after (!) the send operation has finished,
                // such that the request handler can unlock its mutex
                // and the restart operation can close the request handler.
                // Otherwise a deadlock would occur.

                // FIXME: no locking
                restart();
            }
            lock.lock();
        }
    }
}

bool ClientImpl::internal_send(ClientMessage const& message)
{
    auto result = message.SerializeAsString();
    if (result.empty()) {
        log(Error) << "failed to serialize client message"
                   << var("data_case", message.data_case());
        return false;
    }
#ifdef LOON_TEST
    int64_t n = 0;
    if (!m_inject_send_error) {
        n = m_conn->send_binary(result.data(), result.size());
    }
#else
    int64_t n = m_conn->send_binary(result.data(), result.size());
#endif
    if (n <= 0) {
        log(Error) << "failed to send message" << var("retval", n);
        return false;
    }
    return true;
}

bool ClientImpl::update_connected(bool state)
{
    auto old_state = m_connected;
    m_connected = state;
    // Notify any thread that might be waiting for connection state changes.
    // Just to be sure, always notify, even if the state might not have changed.
    m_cv_connection_ready.notify_all();
    return old_state;
}

void ClientImpl::internal_start()
{
    if (m_started) {
        return;
    }
    m_started = true;
    if (!m_conn->start()) {
        auto retrying = m_options.websocket.reconnect_delay.has_value();
        log(Error) << (retrying ? "initial connection attempt failed"
                                : "connection failed")
                   << var("retrying", retrying)
                   << var("address", m_conn->address())
                   << var("conn_timeout", m_options.websocket.connect_timeout)
                   << var("reconn_delay", m_options.websocket.reconnect_delay)
                   << var("max_reconn_delay",
                          m_options.websocket.max_reconnect_delay);
    }
}

void ClientImpl::reset_connection_state()
{
    for (auto& [_, content] : m_content) {
        // Make sure all spawned request handler threads exit
        // and wait until they exited.
        // This method is used from the destructor,
        // so after this method returns, the object will be destroyed.
        // Request handlers use pointers to data in this object,
        // so this object must remain valid until each handler exited.
        content->request_handler()->exit_gracefully();
        // Notify that content is not registered anymore.
        content->unregistered();
    }
    m_content.clear();
    {
        const std::lock_guard<std::mutex> lock(m_request_mutex);
        m_requests.clear();
    }
    m_hello = std::nullopt;
    m_no_content_request_history.clear();
}

void ClientImpl::internal_stop(std::unique_lock<std::mutex>& lock)
{
    if (!m_started) {
        return;
    }
    // Note: the order of the following operations is critical.
    {
        // Unlock the lock while stopping the websocket connection,
        // since this might trigger a call to on_websocket_close(),
        // which would lock the mutex again, causing a deadlock.
        // It is safe to read m_conn without holding the lock
        // because m_conn is never changed after object construction.
        lock.unlock();
        m_conn->stop();
        lock.lock();
    }
    // Unregister all content and stop all request handlers.
    reset_connection_state();
    // Notify any client methods that are waiting for state changes.
    update_connected(false);
    // Reset started state.
    m_started = false;
    // Reset any manager state by removing pending actions.
    // We do not want to carry those into a fresh connection.
    m_manager_action = ManagerAction::Nothing{};
}

void ClientImpl::internal_restart(std::unique_lock<std::mutex>& lock)
{
    internal_stop(lock);
    internal_start();
    log(Warning) << "restarted";
}

void ClientImpl::restart()
{
    if (std::holds_alternative<ManagerAction::Restart>(m_manager_action)) {
        log(Warning) << "manager: already restarting, skipping restart";
        return;
    }
    if (std::holds_alternative<ManagerAction::Fail>(m_manager_action)) {
        log(Warning) << "manager: already failing, skipping restart";
        return;
    }
    ManagerAction::Restart restart{};
    m_manager_action = restart;
    m_cv_manager.notify_one();
}

void loon::ClientImpl::fail()
{
    if (std::holds_alternative<ManagerAction::Restart>(m_manager_action)) {
        log(Warning) << "manager: failing, overwriting pending restart";
    }
    if (std::holds_alternative<ManagerAction::Fail>(m_manager_action)) {
        log(Warning) << "manager: already failing, skipping";
        return;
    }
    ManagerAction::Fail fail{};
    m_manager_action = fail;
    m_cv_manager.notify_one();
}

void ClientImpl::manager_loop()
{
    using Action = ManagerAction;
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
        m_cv_manager.wait(lock, [this] {
            return m_stop_manager_loop ||
                !std::holds_alternative<Action::Nothing>(m_manager_action);
        });
        if (m_stop_manager_loop) {
            return;
        }
        if (auto* restart = std::get_if<Action::Restart>(&m_manager_action)) {
            internal_restart(lock);
        } else if (auto* fail = std::get_if<Action::Fail>(&m_manager_action)) {
            internal_stop(lock);
            if (m_failed_callback) {
                m_failed_callback();
            }
        } else {
            assert(false);
        }
        m_manager_action = Action::Nothing{};
    }
}
