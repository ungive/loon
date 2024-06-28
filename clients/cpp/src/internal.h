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

namespace loon
{
using request_id_t = uint64_t;
using request_path_t = std::string;

struct RequestHandle
{
    /**
     * Creates a request handle for a given content source.
     *
     * @param source The source of the content.
     * @param info The content information.
     * @param send_function A function that should be called
     * to send response client messages to the websocket peer.
     */
    RequestHandle(loon::ContentInfo const& info,
        std::shared_ptr<loon::ContentSource> source, Hello const& hello,
        std::function<bool(ClientMessage const&)> send_func);

    /**
     * Forwards the given request to the serve thread
     * and sends a response to the websocket peer in the background.
     * Concurrent requests are synchronized.
     *
     * @param request The request to serve.
     */
    void serve_request(Request const& request);

    /**
     * Cancels a request with the given ID that is currently being served.
     * Removes the request from the internal list of pending requests,
     * if there is no request being actively served with that ID
     * or if the currently served request has a different ID.
     *
     * @param request_id The ID of the request that should be canceled.
     */
    void cancel_request(uint64_t request_id);

    /**
     * Spawns a new serve thread for this request handle.
     * @throws std::runtime_error if the thread is already spawned
     * or if the request handle has already been destroyed.
     */
    void spawn_serve_thread();

    /**
     * Closes the request handle and its associated serve thread.
     */
    void destroy();

private:
    void serve();

    /**
     * Sends a serving response message with low priority.
     * If a request is being canceled while this message is sent,
     * the thread that called cancel_request() and holds the data lock
     * will be guaranteed execute after the message has been sent.
     * It is therefore guaranteed that any cancellation is applied
     * directly after this message is sent,
     * such that ongoing requests can be cancelled as early as possible.
     *
     * @param message The response client message to send.
     * @returns The return value of the send function.
     * If this value is false, the serve function should terminate.
     */
    bool send_response_message(ClientMessage const& message);

    Hello hello;
    loon::ContentInfo info;
    std::shared_ptr<loon::ContentSource> source;
    std::function<bool(ClientMessage const&)> send_message;

    // The remaining fields are all default-initialized.

    // Using three mutex variables for a thread with priority.
    // Reference: https://stackoverflow.com/a/11673600/6748004
    // When a thread calls cancel_request(), it should always have priority
    // in changing the state of the request handle,
    // so that after sending a message, we always immediately know
    // whether it has been cancelled from the outside or not.

    std::mutex mutex_data{}; // Mutex for data fields.
    std::mutex mutex_next{}; // Mutex for next-to-access threads.
    std::mutex mutex_low{};  // Mutex for low-priority access threads.

    std::condition_variable cv_incoming_request{};
    std::deque<Request> pending_requests{};
    std::optional<request_id_t> handling_request_id{};
    bool cancel_handling_request{ false };
    bool dirty{ false };
    bool stop{ false };
};

class InternalContentHandle : public ContentHandle
{
public:
    InternalContentHandle(
        std::string const& url, std::shared_ptr<RequestHandle> request_handle)
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
