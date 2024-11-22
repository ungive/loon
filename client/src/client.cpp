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
#define log(level) \
    loon_log_macro(level, loon::log_level(), loon::log_message, logger_factory)

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
    if (m_options.disconnect_after_idle.has_value() &&
        m_options.disconnect_after_idle.value() <=
            std::chrono::milliseconds::zero()) {
        throw std::runtime_error(
            "the disconnect_after_idle timeout must be greater than zero");
    }
    if (m_options.max_upload_speed.has_value() &&
        m_options.max_upload_speed.value() == 0) {
        throw std::runtime_error("max_upload_speed may not be zero");
    }
    if (m_options.max_upload_speed.has_value()) {
        throw std::runtime_error("max_upload_speed is not yet implemented");
    }
    if (options.websocket.connect_timeout.has_value() &&
        options.websocket.connect_timeout.value() <=
            std::chrono::milliseconds::zero()) {
        throw std::runtime_error(
            "the connect timeout must be greater than zero");
    }
    if (options.websocket.ping_interval.has_value() &&
        options.websocket.ping_interval.value() <=
            std::chrono::milliseconds::zero()) {
        throw std::runtime_error("the ping interval must be greater than zero");
    }
    if (options.websocket.reconnect_delay.has_value() &&
        options.websocket.reconnect_delay.value() <=
            std::chrono::milliseconds::zero()) {
        throw std::runtime_error(
            "the reconnect delay must be greater than zero");
    }
    if (options.websocket.max_reconnect_delay.has_value() &&
        !options.websocket.reconnect_delay.has_value()) {
        throw std::runtime_error("the maximum reconnect delay may only be set"
                                 "when a reconnect delay is set");
    }
    if (options.websocket.max_reconnect_delay.has_value() &&
        options.websocket.reconnect_delay.has_value() &&
        options.websocket.max_reconnect_delay.value() <=
            options.websocket.reconnect_delay.value()) {
        throw std::runtime_error("the maximum reconnect delay must be greater "
                                 "than the reconnect delay");
    }
    // Event handlers
    m_conn->on_open(std::bind(&ClientImpl::on_websocket_open, this));
    m_conn->on_close(std::bind(&ClientImpl::on_websocket_close, this));
    m_conn->on_message(std::bind(
        &ClientImpl::on_websocket_message, this, std::placeholders::_1));
    // Logging
    loon::init_logging();
    // Start the manager loop thread
    m_manager_loop_thread = std::thread([this] {
        util::log_exception_and_rethrow("ClientImpl::manager_loop",
            std::bind(&ClientImpl::manager_loop, this));
    });
}

ClientImpl::~ClientImpl()
{
    // Stop the client
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        internal_stop(lock);
    }
    // Stop the manager loop thread
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stop_manager_loop = true;
        m_cv_manager.notify_all();
    }
    m_manager_loop_thread.join();
}

void ClientImpl::start()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    set_idle(false);
    if (m_started) {
        ensure_started();
        return;
    }
    m_was_explicitly_started = true;
    m_started = true;
    internal_start();
}

void ClientImpl::stop()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_started) {
        return;
    }
    m_was_explicitly_stopped = true;
    m_started = false;
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

inline loon::Logger ClientImpl::logger_factory(
    LogLevel level, log_handler_t handler)
{
    Logger logger(level, handler);
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
    // Ensure that the underlying connection is started.
    ensure_started();
    // Wait until we are connected or not connecting anymore.
    return m_cv_connection_ready.wait_for(lock, timeout, [this] {
        return !m_connecting || m_connected;
    });
}

std::shared_ptr<ContentHandle> ClientImpl::register_content(
    std::shared_ptr<loon::ContentSource> source, loon::ContentInfo const& info,
    std::chrono::milliseconds timeout)
{
    if (source == nullptr) {
        throw MalformedContentException("the content handle cannot be null");
    }
    if (source->size() == 0) {
        // The specification disallows empty content.
        throw MalformedContentException(
            "the content cannot have a size of zero");
    }

    std::unique_lock<std::mutex> lock(m_mutex);

    // The connection is not idling anymore.
    set_idle(false);

    // Idling if no content is registered when execution ends.
    std::shared_ptr<void> scope_guard(nullptr, std::bind([this] {
        // Make sure we notify both true and false values,
        // as it's possible that idling has been enabled in the meantime,
        // while we want it to be disabled.
        set_idle(m_content.empty());
    }));

    // Check if the path is already in use.
    auto const& path = info.path;
    auto it = m_content.find(path);
    if (it != m_content.end()) {
        throw PathAlreadyRegisteredException(
            "content under this path is already registered with this client");
    }

    // Wait until the connection is ready for content registration.
    wait_until_ready(lock, timeout);

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
    if (handle == nullptr) {
        throw MalformedContentException("the content handle cannot be null");
    }
    // Verify that the content is valid.
    auto ptr = std::dynamic_pointer_cast<InternalContentHandle>(handle);
    if (!ptr) {
        throw MalformedContentException(
            "the content handle has the wrong type");
    }

    std::unique_lock<std::mutex> lock(m_mutex);

    // Idling if no content is registered when execution ends.
    std::shared_ptr<void> scope_guard(nullptr, std::bind([this] {
        if (m_content.empty() && m_options.automatic_idling) {
            set_idle(true);
        }
    }));

    // Check if the content is registered.
    auto it = m_content.find(ptr->path());
    if (it == m_content.end()) {
        return;
    }

    // Notify that the content is being unregistered (rather early than late).
    it->second->unregistered();

    // Exit the request handle's serve thread
    // and wait for it to have terminated, then remove the handle.
    it->second->request_handler()->exit_gracefully();
    m_content.erase(it);
}

std::vector<std::shared_ptr<ContentHandle>> loon::ClientImpl::content()
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<std::shared_ptr<ContentHandle>> result;
    result.reserve(m_content.size());
    std::transform(m_content.begin(), m_content.end(),
        std::back_inserter(result),
        [](decltype(m_content)::value_type const& value) {
            return value.second;
        });
    return result;
}

bool loon::ClientImpl::is_registered(std::shared_ptr<ContentHandle> handle)
{
    if (handle == nullptr) {
        throw MalformedContentException("the content handle cannot be null");
    }
    auto ptr = std::dynamic_pointer_cast<InternalContentHandle>(handle);
    if (ptr == nullptr) {
        throw MalformedContentException(
            "the content handle has the wrong type");
    }

    const std::lock_guard<std::mutex> lock(m_mutex);

    return m_content.find(ptr->path()) != m_content.end();
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

    // The connection is idling if no content was registered yet.
    if (m_content.empty() && m_options.automatic_idling) {
        set_idle(true);
    }

    log(Info) << "ready" << var("base_url", m_hello->base_url());

    // Call the callback before notifying that the connection is ready.
    if (m_ready_callback) {
        m_ready_callback();
    }
    m_cv_connection_ready.notify_all();
}

bool ClientImpl::check_request_limit(decltype(m_content)::iterator it)
{
    auto has_content = it != m_content.end();
    auto now = std::chrono::steady_clock::now();
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
    log(Debug) << "received request" << var("rid", request.id())
               << var("path", request.path());

    if (check_request_limit(it)) {
        log(Error) << "too many requests" << var("rid", request.id())
                   << var("path", request.path());
        return restart();
    }

    if (it == m_content.end()) {
        ClientMessage message;
        auto empty_response = message.mutable_empty_response();
        empty_response->set_request_id(request.id());
        log(Debug) << "empty response" << var("rid", request.id())
                   << var("path", request.path());
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
            << "protocol: received request closed for an unknown request id"
            << var("rid", request_closed.request_id())
            << var("message", request_closed.message());
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

bool ClientImpl::wait_until_ready(
    std::unique_lock<std::mutex>& lock, std::chrono::milliseconds timeout)
{
    using namespace std::chrono;

    // Return if the client is already ready and we don't need to wait.
    if (m_started && m_connected && m_hello.has_value()) {
        return false;
    }

    // Wait until the connection is opened.
    auto start = high_resolution_clock::now();
    if (!wait_until_connected(lock, timeout)) {
        throw ClientNotConnectedException("the client is not connected");
    }
    auto delta = high_resolution_clock::now() - start;
    auto remaining = std::max(timeout - delta, nanoseconds::zero());

    // Wait until Hello has been received and the connection is ready.
    m_cv_connection_ready.wait_for(lock, remaining, [this] {
        return !m_connected || m_hello.has_value();
    });
    if (!m_connected) {
        throw ClientNotConnectedException("the client is not connected");
    }
    if (!m_hello.has_value()) {
        throw TimeoutException(
            "did not receive initial server message in time");
    }
    return true;
}

void ClientImpl::on_websocket_open()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    update_connected(true);
    m_was_explicitly_started = false;
}

void ClientImpl::on_websocket_close()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    update_connected(false);
    reset_connection_state();
    if (m_was_explicitly_started) {
        m_was_explicitly_started = false;
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
    if (m_was_explicitly_stopped) {
        m_was_explicitly_stopped = false;
        log(Info) << "client stopped";
    }
    if (m_disconnect_callback) {
        m_disconnect_callback();
    }
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
            log(Fatal) << "protocol: received more than one Hello message";
            return fail();
        }
        break;
    default:
        if (!m_hello.has_value()) {
            log(Fatal) << "protocol: first message is not a Hello message";
            return fail();
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
    const std::lock_guard<std::mutex> send_lock(m_write_mutex);
    auto result = message.SerializeAsString();
    if (result.empty()) {
        log(Error) << "failed to serialize client message"
                   << var("data_case", message.data_case());
        restart();
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
        restart();
        return false;
    }
    return true;
}

bool ClientImpl::update_connected(bool state)
{
    auto old_state = m_connected;
    m_connected = state;
    m_connecting = false;
    // Notify any thread that might be waiting for connection state changes.
    // Just to be sure, always notify, even if the state might not have changed.
    m_cv_connection_ready.notify_all();
    return old_state;
}

void ClientImpl::internal_start()
{
    m_connecting = true;
    m_conn->start();
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
    // Not idling anymore.
    set_idle(false);
    // Notify any client methods that are waiting for state changes.
    update_connected(false);
    // Reset any manager state by removing pending actions.
    // We do not want to carry those into a fresh connection.
    m_manager_action = ManagerAction::Nothing{};
}

void ClientImpl::internal_stop_and_reset(std::unique_lock<std::mutex>& lock)
{
    internal_stop(lock);
    // Make sure the user can start the client again manually.
    m_started = false;
}

void ClientImpl::internal_restart(std::unique_lock<std::mutex>& lock)
{
    internal_stop(lock);
    internal_start();
}

void loon::ClientImpl::internal_ping(std::chrono::milliseconds timeout)
{
    //
}

void loon::ClientImpl::idle_stop(std::unique_lock<std::mutex>& lock)
{
    log(Info) << "disconnecting after idle"
              << var("duration", m_options.disconnect_after_idle);
    internal_stop(lock);
}

void loon::ClientImpl::ensure_started()
{
    if (!m_connected && !m_connecting) {
        internal_start();
    }
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
    using namespace std::chrono;

    std::unique_lock<std::mutex> lock(m_mutex);
    auto idle_time_point = steady_clock::time_point::max();
    while (true) {
        m_cv_manager.wait_until(lock, idle_time_point, [&] {
            return m_stop_manager_loop || m_track_idling.has_value() ||
                m_idle_waiting && steady_clock::now() >= idle_time_point ||
                !std::holds_alternative<Action::Nothing>(m_manager_action);
        });
        if (m_stop_manager_loop) {
            return;
        }
        // It's important to check if idling should not be tracked anymore
        // before checking if the idle timeout expired, in case idling
        // is being cancelled.
        if (m_track_idling.has_value()) {
            bool do_track = m_track_idling.value();
            m_track_idling = std::nullopt;
            if (!do_track || !m_started || !m_connected) {
                // No need to idle when:
                // - Idling is being disabled
                // - The client is not started
                // - The client is started, but in the process of reconnecting
                //   and will be put into idle as soon as it has reconnected,
                //   if automatic idling is enabled, see on_hello()
                idle_time_point = steady_clock::time_point::max();
                m_idle_waiting = false;
                continue;
            }
            if (m_idle_waiting) {
                // Already idling.
                continue;
            }
            if (m_options.disconnect_after_idle.has_value()) {
                // Only idle if an idle timeout is configured.
                auto duration = m_options.disconnect_after_idle.value();
                idle_time_point = steady_clock::now() + duration;
                m_idle_waiting = true;
            }
            continue;
        }
        if (m_idle_waiting && steady_clock::now() >= idle_time_point) {
            idle_stop(lock);
            idle_time_point = steady_clock::time_point::max();
            m_idle_waiting = false;
            continue;
        }
        if (auto* restart = std::get_if<Action::Restart>(&m_manager_action)) {
            internal_restart(lock);
        } else if (auto* fail = std::get_if<Action::Fail>(&m_manager_action)) {
            internal_stop_and_reset(lock);
            if (m_failed_callback) {
                m_failed_callback();
            }
        }
        m_manager_action = Action::Nothing{};
    }
}
