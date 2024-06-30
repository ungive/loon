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
#include "request_handle.h"

namespace loon
{
class InternalContentHandle;

class ClientImpl : public IClient
{
public:
    ClientImpl(
        std::string const& address, std::optional<std::string> const& auth);

    void start() override;

    void stop() override;

    std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info) override;

    void unregister_content(std::shared_ptr<ContentHandle> handle) override;

private:
    using request_path_t = std::string;

    void on_websocket_open();
    void on_websocket_close();
    void on_websocket_message(std::string const& message);
    void handle_message(ServerMessage const& message);
    void on_hello(Hello const& request);
    void on_request(Request const& request);
    void on_success(Success const& success);
    void on_request_closed(RequestClosed const& request_closed);
    void on_close(Close const& close);
    bool update_connected(bool state);
    void check_content_constraints(std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info);

    std::string make_url(std::string const& path);

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

    void reset_connection_state();
    void internal_start();
    void internal_stop();
    void internal_restart();

    const std::string m_address{};
    const std::optional<std::string> m_auth{};

    std::atomic<bool> m_connected{ false };
    std::mutex m_mutex{};
    std::condition_variable m_cv_connection_ready{};
    hv::WebSocketClient m_conn{};

    // Per-connection state fields

    std::optional<Hello> m_hello{};
    std::unordered_map<request_path_t, std::shared_ptr<InternalContentHandle>>
        m_content{};
};

class InternalContentHandle : public ContentHandle
{
public:
    InternalContentHandle(std::string const& url,
        std::string const& path,
        std::shared_ptr<RequestHandle> request_handle)
        : m_url{ url }, m_path{ path }, m_request_handle{ request_handle }
    {
    }

    inline std::string const& url() const override { return m_url; }

    inline std::string const& path() const { return m_path; }

    inline std::shared_ptr<RequestHandle> request_handle()
    {
        return m_request_handle;
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
    std::shared_ptr<RequestHandle> m_request_handle;

    std::mutex m_mutex{};
    std::function<void()> m_served_callback{};
    std::function<void()> m_unregistered_callback{};
    bool m_registered{ true /* considered registered until unregistered */ };
};
} // namespace loon
