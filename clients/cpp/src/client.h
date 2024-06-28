#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include <hv/WebSocketClient.h>

#include "loon/loon.h"
#include "loon/messages.pb.h"
#include "request_handle.h"

namespace loon
{
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

    class InternalContentHandle : public ContentHandle
    {
    public:
        InternalContentHandle(std::string const& url,
            std::shared_ptr<RequestHandle> request_handle)
            : m_url{ url }, m_request_handle{ request_handle }
        {
        }

        inline std::string const& url() const override { return m_url; }

        inline std::shared_ptr<RequestHandle> request_handle()
        {
            return m_request_handle;
        }

    private:
        std::string m_url{};
        std::shared_ptr<RequestHandle> m_request_handle;
    };

    void on_websocket_open();
    void on_websocket_close();
    void on_websocket_message(std::string const& message);
    void handle_message(ServerMessage const& message);
    void on_request(Request const& request);
    void on_success(Success const& success);
    void on_request_closed(RequestClosed const& request_closed);
    void on_close(Close const& close);

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

    void internal_start();
    void internal_stop();
    void internal_restart();

    std::string m_address{};
    std::optional<std::string> m_auth{};
    std::atomic<bool> m_connected{ false };
    std::mutex m_mutex{};
    hv::WebSocketClient m_conn{};
    std::optional<Hello> m_hello{};
    std::unordered_map<request_path_t, std::shared_ptr<InternalContentHandle>>
        m_content;
};
} // namespace loon
