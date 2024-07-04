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

#include <hv/WebSocketClient.h>

#include "loon/client.h"
#include "loon/messages.pb.h"
#include "request_handler.h"

namespace loon
{
class InternalContentHandle;

class ClientImpl : public IClient
{
public:
    ClientImpl(std::string const& address,
        std::optional<std::string> const& auth,
        ClientOptions options = {});

    ~ClientImpl();

    void start() override;

    void stop() override;

    inline void failed(std::function<void()> callback) override
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_failed_callback = callback;
    }

    std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info) override;

    void unregister_content(std::shared_ptr<ContentHandle> handle) override;

protected:
    // Methods that should be accessible from tests.

    /**
     * Serializes a client message and sends to the websocket peer.
     * Returns true if the message was successfully sent.
     * Returns false if an error occured and the connection is restarted.
     * If false is returned, any handling function should terminate immediately,
     * without doing any further operations on the current connection.
     *
     * @param message The client message to send.
     * @returns A boolean indicating that the message has been sent
     * or that an error occured and the connection is in an invalid state.
     */
    bool send(ClientMessage const& message);

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
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
    inline Hello current_hello()
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        wait_until_ready(lock);
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
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        m_injected_hello_modifer = modifier;
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
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_chunk_sleep_duration = duration;
    }

private:
    using request_id_t = uint64_t;
    using request_path_t = std::string;

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
    void wait_until_ready(std::unique_lock<std::recursive_mutex>& lock);
    void check_content_constraints(std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info);

    std::string make_url(std::string const& path);

    void reset_connection_state();
    void internal_start();
    void internal_stop();
    void internal_restart();

    /**
     * @brief Puts the client in a failed state and closes any connection.
     *
     * After calling this method, the client is guaranteed to not be connected
     * and not attempt any reconnects.
     *
     * @param message A message describing what happened.
     */
    void fail(std::string const& message);

    const std::string m_address{};
    const std::optional<std::string> m_auth{};
    const ClientOptions m_options{};
    std::chrono::milliseconds m_chunk_sleep_duration{
        std::chrono::milliseconds::zero()
    };
    std::deque<std::chrono::system_clock::time_point> request_history{};
    std::function<void(Hello&)> m_injected_hello_modifer{};
    std::function<void()> m_failed_callback{};

    std::atomic<bool> m_connected{ false };
    // Use a recursive mutex, since many methods could trigger a reconnect,
    // which would call close and would trigger the close callback,
    // which locks this mutex again.
    std::recursive_mutex m_mutex{};
    // Connection write mutex. For calls to m_conn.send().
    std::mutex m_write_mutex{};
    // Requests mutex. For reads/writes from/to m_requests.
    std::mutex m_request_mutex{};
    // Use an "any" condition variable, so it works with a recursive mutex.
    // It must only be used if it is known that the mutex is only locked once.
    std::condition_variable_any m_cv_connection_ready{};

    // Per-connection state fields

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
    hv::WebSocketClient m_conn{};

    void call_served_callback(decltype(m_requests)::iterator it);
};

class InternalContentHandle : public ContentHandle
{
public:
    InternalContentHandle(std::string const& url,
        std::string const& path,
        std::shared_ptr<RequestHandler> request_handle)
        : m_url{ url }, m_path{ path }, m_request_handler{ request_handle }
    {
    }

    inline std::string const& url() const override { return m_url; }

    inline std::string const& path() const { return m_path; }

    inline std::shared_ptr<RequestHandler> request_handler()
    {
        return m_request_handler;
    }

    void served(std::function<void()> callback) override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_served_callback = callback;
    }

    void unregistered(std::function<void()> callback) override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_registered) {
            return callback();
        }
        m_unregistered_callback = callback;
    }

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

private:
    std::string m_url{};
    std::string m_path{};
    std::shared_ptr<RequestHandler> m_request_handler;

    std::mutex m_mutex{};
    std::function<void()> m_served_callback{};
    std::function<void()> m_unregistered_callback{};
    bool m_registered{ true /* considered registered until unregistered */ };
};
} // namespace loon
