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
class ContentSource
{
public:
    virtual std::istream& data() = 0;
    virtual size_t size() const = 0;
    virtual std::string const& content_type() const = 0;
    virtual void close() = 0;
};

class MemoryStreamBuffer : public std::basic_streambuf<char>
{
public:
    MemoryStreamBuffer(char* p, size_t l) { setg(p, p, p + l); }
};

class MemoryStream : public std::istream
{
public:
    MemoryStream(char* p, size_t l) : std::istream(&m_buffer), m_buffer(p, l)
    {
        rdbuf(&m_buffer);
    }

private:
    MemoryStreamBuffer m_buffer;
};

class BufferContentSource : public loon::ContentSource
{
public:
    BufferContentSource(
        std::vector<char> const& buffer, std::string const& content_type)
        : m_data{ buffer }, m_content_type{ content_type },
          m_stream{ new_stream() }
    {
    }

    inline std::istream& data() override
    {
        reset_stream();
        return m_stream;
    }

    inline size_t size() const override { return m_data.size(); }

    inline std::string const& content_type() const override
    {
        return m_content_type;
    }

    inline void close() override {}

private:
    inline MemoryStream new_stream()
    {
        return MemoryStream(const_cast<char*>(m_data.data()), m_data.size());
    }

    inline void reset_stream() { m_stream = std::move(new_stream()); }

    std::vector<char> m_data;
    MemoryStream m_stream;
    std::string m_content_type;
};

struct ContentInfo
{
    ContentInfo(std::string const& path) : path{ path } {}

    ContentInfo(std::string const& path, uint64_t max_cache_duration)
        : path{ path }, max_cache_duration{ max_cache_duration }
    {
    }

    ContentInfo(std::string const& path, std::string const& attachment_filename)
        : path{ path }, attachment_filename{ attachment_filename }
    {
    }

    ContentInfo(std::string const& path, uint64_t max_cache_duration,
        std::string const& attachment_filename)
        : path{ path }, max_cache_duration{ max_cache_duration },
          attachment_filename{ attachment_filename }
    {
    }

    std::string path;
    std::optional<std::string> attachment_filename;
    std::optional<uint64_t> max_cache_duration;
};

class Client
{
public:
    /**
     * A handle for content registered with a Client.
     */
    class ContentHandle
    {
    public:
        virtual std::string const& url() const = 0;
    };

    /**
     * Creates a new loon client.
     *
     * @param address The websocket address to connect to.
     * @param auth The HTTP Basic authentication string,
     * a username and a password, separated by a colon.
     */
    Client(std::string const& address, std::optional<std::string> const& auth);

    /**
     * Connects to the server and maintains a websocket connection.
     * Attempts to reconnect on connection failure or disconnect,
     * until the stop method is called.
     * Returns immediately and does nothing, if already connecting or connected.
     */
    void start();

    /**
     * Disconnects from the server.
     * Returns immediately and does nothing, if already disconnected.
     * Notifies all handles that registered content is not available anymore.
     */
    void stop();

    /**
     * Registers content with this client and returns a handle for it.
     * The content remains registered across websocket reconnects,
     * until it is unregistered with unregister_content().
     * Calls to any of the methods of the source are synchronized.
     * The source's data() method is only called once per request
     * and while no other requests are being handled
     * that are using a previous return value of it.
     * If the source reads from a file e.g.,
     * the data() method can safely seek to the beginning of the file
     * without having to worry about corrupting other ongoing requests.
     *
     * @param source The source for the content.
     * @throws NotConnectedException if the client is not connected.
     */
    std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<ContentSource> source, ContentInfo const& info);

    /**
     * Unregisters content from this client that is under the given handle.
     *
     * @param handle The handle for which the content should be unregistered.
     * @throws ContentNotRegisteredException
     * if the content is not registered with this client.
     */
    void unregister_content(std::shared_ptr<ContentHandle> handle);

private:
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
        RequestHandle(ContentInfo const& info,
            std::shared_ptr<ContentSource> source, Hello const& hello,
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
        ContentInfo info;
        std::shared_ptr<ContentSource> source;
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

class NotConnectedException : public std::runtime_error
{
public:
    using runtime_error::runtime_error;
};

class ContentNotRegisteredException : public std::runtime_error
{
public:
    using runtime_error::runtime_error;
};
} // namespace loon
