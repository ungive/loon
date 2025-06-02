#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

#include "logging.h"
#include "loon/client.h"
#include "loon/messages.pb.h"
#include "request_handler.h"
#include "websocket/client.h"

namespace loon
{
class InternalContentHandle;

class ClientImpl : public IClient
{
public:
    ClientImpl(std::string const& address, ClientOptions options = {});

    ClientImpl(ClientImpl const& other) = delete;

    ClientImpl(ClientImpl&& other) = delete;

    ~ClientImpl();

    void start() override;

    void stop() override;

    void terminate() override;

    inline bool started() override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_started;
    }

    inline void idle(bool state) override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        set_idle(state);
    }

    inline void idle() override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        set_idle(true);
    }

    inline bool wait_until_ready() override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return wait_until_ready(lock, connect_timeout());
    }

    inline bool wait_until_ready(std::chrono::milliseconds timeout) override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return wait_until_ready(lock, timeout);
    }

    inline void on_ready(std::function<void()> callback) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ready_callback = callback;
    }

    inline void on_disconnect(std::function<void()> callback) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_disconnect_callback = callback;
    }

    inline void on_failed(std::function<void()> callback) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failed_callback = callback;
    }

    std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info,
        std::chrono::milliseconds timeout) override;

    inline std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info) override
    {
        return register_content(source, info, connect_timeout());
    }

    void unregister_content(std::shared_ptr<ContentHandle> handle) override;

    std::vector<std::shared_ptr<ContentHandle>> content() override;

    bool is_registered(std::shared_ptr<ContentHandle> handle) override;

protected:
    // Methods that should be accessible from tests.

    /**
     * @brief Serializes a client message and sends it to the websocket peer.
     *
     * Returns true if the message was successfully sent.
     * Returns false if an error occured and the connection is restarted.
     * If false is returned, any handling function should terminate immediately,
     * without doing any further operations on the current connection.
     *
     * Safe to be called from within a request handler,
     * as the send operation is performed on a separate thread
     * and the connection is restarted after this method returned,
     * in case the send operation failed.
     *
     * @param message The client message to send.
     * @returns A boolean indicating that the message has been sent
     * or that an error occured and the connection is in an invalid state.
     */
    bool send(ClientMessage const& message);

#ifdef LOON_TEST
    // Methods and fields that are only needed for testing
    // and which shouldn't be part of release builds.

    /**
     * @brief Counts the number of active requests that are being handled.
     *
     * Includes requests that have and have not been fully sent.
     * Excludes requests that have been acknowledged by the server
     * with a Success message.
     *
     * @returns The number of requests that are currently being handled.
     */
    inline size_t active_requests()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_requests.size();
    }

    /**
     * @brief Returns the Hello message for the current connection.
     *
     * Waits until the Hello message has been received,
     * if the server did not send it yet while the connection is active.
     *
     * @returns The Hello message from the server.
     */
    inline Hello wait_for_hello()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        wait_until_ready(lock, connect_timeout());
        return m_hello.value();
    }

    /**
     * @brief Injects a Hello message modifer into the server communication.
     *
     * After calling this method, when start() is called,
     * the server's Hello message may be modified with the given function.
     *
     * @param hello A function that modifies the server's hello message.
     */
    inline void inject_hello_modifier(std::function<void(Hello&)> modifier)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_injected_hello_modifer = modifier;
    }

    /**
     * @brief Injects a send error, to simulate send failure.
     *
     * The send function will return 0
     *
     * @param trigger_error
     */
    inline void inject_send_error(bool trigger_error)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_inject_send_error = trigger_error;
    }

    /**
     * @brief How long to sleep inbetween chunks.
     *
     * A zero value indicates no sleeping inbetween chunks, the default.
     * Must be set before registering content.
     */
    inline void chunk_sleep(
        std::chrono::milliseconds duration = std::chrono::milliseconds::zero())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_chunk_sleep_duration = duration;
    }

    /**
     * @brief How long to sleep before processing incoming messages.
     *
     * A zero value indicates no sleeping, the default.
     */
    inline void incoming_sleep(
        std::chrono::milliseconds duration = std::chrono::milliseconds::zero())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_incoming_sleep_duration = duration;
    }

    /**
     * @brief Triggers a restart and returns once the client is restarted.
     */
    inline void restart_and_wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        internal_restart(lock);
    }

    /**
     * @brief Whether the client is connected and ready for content.
     *
     * @returns If the client is ready for registering content.
     */
    inline bool connected()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_connected;
    }

    inline bool idling()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_idle_waiting || m_track_idling.value_or(false);
    }

private:
    std::function<void(Hello&)> m_injected_hello_modifer{};
    bool m_inject_send_error{ false };
    std::chrono::milliseconds m_chunk_sleep_duration{
        std::chrono::milliseconds::zero()
    };
    std::chrono::milliseconds m_incoming_sleep_duration{
        std::chrono::milliseconds::zero()
    };
#endif

private:
    using request_id_t = uint64_t;
    using request_path_t = std::string;

    /**
     * @brief Restarts the connection.
     *
     * Triggers the restarting process on another thread,
     * so that the calling function can return immediately.
     *
     * Should be preceded by a log message describing what happened,
     * using the "Error" log level, since that level is used
     * to communicate that the connection is restarted.
     *
     * A lock for the data mutex must be held when calling this method.
     */
    void restart();

    /**
     * @brief Puts the client in a failed state and closes the connection.
     *
     * Triggers the restarting process on another thread,
     * so that the calling function can return immediately.
     *
     * Should be preceded by a log message describing what happened,
     * using the "Fatal" log level, since that level is used
     * to communicate that the connection has failed.
     *
     * A lock for the data mutex must be held when calling this method.
     */
    void fail();

    Logger logger_factory(LogLevel level, log_handler_t handler);

    void on_websocket_open();
    void on_websocket_close();
    void on_websocket_message(std::string const& message);
    void handle_message(ServerMessage const& message);
    void response_sent(uint64_t request_id);
    void on_hello(Hello const& request);
    void on_request(Request const& request);
    void on_success(Success const& success);
    void on_request_closed(RequestClosed const& request_closed);
    void on_close(Close const& close);
    bool update_connected(bool state);
    bool wait_until_connected(
        std::unique_lock<std::mutex>& lock, std::chrono::milliseconds timeout);
    bool wait_until_ready(
        std::unique_lock<std::mutex>& lock, std::chrono::milliseconds timeout);
    void check_content_constraints(std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info);

    std::string make_url(std::string const& path);

    // clang-format off
    struct ManagerAction
    {
        struct Nothing {};
        struct Restart {};
        struct Fail {};

        using variant = std::variant<
            ManagerAction::Nothing,
            ManagerAction::Restart,
            ManagerAction::Fail>;
    }; // clang-format on

    void manager_loop();

    std::thread m_manager_loop_thread{};
    std::condition_variable m_cv_manager{};
    ManagerAction::variant m_manager_action{};
    std::optional<bool> m_track_idling{};
    bool m_stop_manager_loop{ false };

    void reset_connection_state();
    void internal_start();
    void internal_stop(
        std::unique_lock<std::mutex>& lock, bool terminate = false);
    void internal_stop_and_reset(std::unique_lock<std::mutex>& lock);
    void internal_restart(std::unique_lock<std::mutex>& lock);

    bool m_idle_waiting{ false };
    void idle_stop(std::unique_lock<std::mutex>& lock);
    void ensure_started();

    inline void set_idle(bool state)
    {
        m_track_idling = state;
        m_cv_manager.notify_one();
    }

    inline std::chrono::milliseconds connect_timeout() const
    {
        return m_options.websocket.connect_timeout.value_or(
            loon::websocket::default_connect_timeout);
    }

    const ClientOptions m_options{};
    std::deque<std::chrono::steady_clock::time_point>
        m_no_content_request_history{};
    std::function<void()> m_ready_callback{};
    std::function<void()> m_disconnect_callback{};
    std::function<void()> m_failed_callback{};

    bool m_started{ false };
    bool m_connected{ false };
    bool m_connecting{ false };
    bool m_was_explicitly_started{ false };
    bool m_was_explicitly_stopped{ false };
    // Mutex for data fields.
    std::mutex m_mutex{};
    // Mutex for writing to the connection.
    std::mutex m_write_mutex{};
    // Requests mutex. For reads/writes from/to m_requests.
    std::mutex m_request_mutex{};
    // Use an "any" condition variable, so it works with a recursive mutex.
    // It must only be used if it is known that the mutex is only locked once.
    std::condition_variable m_cv_connection_ready{};

    std::optional<Hello> m_hello{};
    std::unordered_map<request_path_t, std::shared_ptr<InternalContentHandle>>
        m_content{};
    // Stores ongoing requests and whether they have been sent or not.
    std::unordered_map<request_id_t,
        std::pair<std::shared_ptr<InternalContentHandle>, bool>>
        m_requests{};

    // The underlying websocket client is constructed last.
    // This must be done, so that it is destructed first,
    // since its callbacks might use data of this class instance.
    // Destructing other fields first would cause undefined behaviour.
    std::unique_ptr<websocket::Client> m_conn;

    void call_served_callback(decltype(m_requests)::iterator it);
    bool check_request_limit(decltype(m_content)::iterator it);
};

class InternalContentHandle : public ContentHandle
{
public:
    InternalContentHandle(
        std::string const& url, std::shared_ptr<RequestHandler> request_handle)
        : m_url{ url }, m_request_handler{ request_handle }
    {
    }

    inline std::string const& url() const override { return m_url; }

    inline std::string const& path() const
    {
        return m_request_handler->info().path;
    }

    inline std::shared_ptr<RequestHandler> request_handler()
    {
        return m_request_handler;
    }

    void on_served(std::function<void()> callback) override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_served_callback = callback;
    }

    void on_unregistered(std::function<void()> callback) override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_registered) {
            return callback();
        }
        m_unregistered_callback = callback;
    }

    // internal

    /**
     * @brief Call this method if the content has been fully served once.
     */
    void served()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_served_callback) {
            m_served_callback();
        }
    }

    /**
     * @brief Call this method if the content handle has been unregistered.
     */
    void unregistered()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_registered && m_unregistered_callback) {
            m_unregistered_callback();
        }
        m_registered = false;
        m_unregistered_callback = nullptr;
    }

    /**
     * @brief The point in time when the last request has been received.
     *
     * Has no value if not request was received before.
     */
    std::optional<std::chrono::steady_clock::time_point> last_request{};

private:
    std::string m_url{};
    std::shared_ptr<RequestHandler> m_request_handler;

    std::mutex m_mutex{};
    std::function<void()> m_served_callback{};
    std::function<void()> m_unregistered_callback{};
    bool m_registered{ true /* considered registered until unregistered */ };
};
} // namespace loon
