#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "logging.h"
#include "loon/client.h"

// TODO proper version string
#define LOON_USER_AGENT "loon-cpp-client/0.1"

namespace loon::websocket
{
extern std::chrono::milliseconds default_connect_timeout;

class IClient
{
public:
    virtual ~IClient() {};

    /**
     * @brief The websocket server address the client connects to.
     *
     * @returns The websocket address.
     */
    virtual std::string const& address() const = 0;

    /**
     * @brief Sets the callback for when the connection is opened.
     *
     * May not be called after start().
     *
     * @param callback The callback function.
     */
    virtual void on_open(std::function<void()> callback) = 0;

    /**
     * @brief Sets the callback for when the connection is closed.
     *
     * May not be called after start().
     *
     * @param callback The callback function.
     */
    virtual void on_close(std::function<void()> callback) = 0;

    /**
     * @brief Sets the callback for when a message is received.
     *
     * May not be called after start().
     *
     * @param callback The callback function.
     */
    virtual void on_message(
        std::function<void(std::string const& message)> callback) = 0;

    /**
     * @brief Sends binary data to the websocket peer.
     *
     * The data is sent with OP code binary.
     */

    /**
     * @brief Sends binary data to the websocket peer.
     *
     * The data is sent with OP code binary.
     *
     * @param data The data to send.
     * @param length The length of the data.
     * @returns The number of bytes actually sent.
     */
    virtual int64_t send_binary(const char* data, size_t length) = 0;

    /**
     * @brief Sends text data to the websocket peer.
     *
     * The data is sent with OP code text.
     *
     * @param data The data to send.
     * @param length The length of the data.
     * @returns The number of bytes actually sent.
     */
    virtual int64_t send_text(const char* data, size_t length) = 0;

    /**
     * @brief Starts the websocket client.
     *
     * Attempts to reconnect until stop() is called,
     * if a reconnect delay is configured in the options.
     * Does nothing, if the client is already started.
     *
     * This method is guaranteed to return immediately,
     * it will not block until any of the event handlers
     * have been called and returned.
     * If a mutex is held while start() is called,
     * it can safely be locked from any of the event handlers.
     */
    virtual void start() = 0;

    /**
     * @brief Stops the websocket client.
     *
     * Does nothing, if the client is not started or already stopped.
     *
     * Blocks until the client is fully stopped
     * and until all resources have been deallocated.
     * If a mutex is locked in any of the event handlers,
     * no lock should be held for it when calling this method.
     */
    virtual void stop() = 0;
};

class Client : public IClient
{
public:
    Client(std::string const& address, WebsocketOptions const& options);
    ~Client();

    inline std::string const& address() const override
    {
        return m_impl->address();
    }

    inline void on_open(std::function<void()> callback) override
    {
        m_impl->on_open(callback);
    }

    inline void on_close(std::function<void()> callback) override
    {
        m_impl->on_close(callback);
    }

    inline void on_message(
        std::function<void(std::string const& message)> callback) override
    {
        m_impl->on_message(callback);
    }

    inline int64_t send_binary(const char* data, size_t length) override
    {
        return m_impl->send_binary(data, length);
    }

    inline int64_t send_text(const char* data, size_t length) override
    {
        return m_impl->send_text(data, length);
    }

    inline void start() override { return m_impl->start(); }

    inline void stop() override { return m_impl->stop(); }

private:
    std::unique_ptr<IClient> m_impl;
};

class BaseClient : public IClient
{
public:
    BaseClient(std::string const& address, WebsocketOptions const& options);

    ~BaseClient();

    inline std::string const& address() const override { return m_address; }

    void on_open(std::function<void()> callback) override;

    void on_close(std::function<void()> callback) override;

    void on_message(
        std::function<void(std::string const& message)> callback) override;

    virtual int64_t send_binary(const char* data, size_t length) = 0;

    virtual int64_t send_text(const char* data, size_t length) = 0;

    void start() override;

    void stop() override;

protected:
    virtual void internal_start() = 0;
    virtual void internal_stop() = 0;

    /**
     * @brief Call this method when the websocket connection is opened.
     */
    inline void on_websocket_open()
    {
        // Note: No mutex used for performance reasons
        // and since it is prohibited in the client code
        // that the callback can be overwritten while the client is started.
        if (m_open_callback) {
            m_open_callback();
        }
    }

    /**
     * @brief Call this method when the websocket connection is closed.
     */
    inline void on_websocket_close()
    {
        // Note: No mutex, for the same reason as in on_websocket_open.
        if (m_close_callback) {
            m_close_callback();
        }
    }

    /**
     * @brief Call this method when a websocket message is received.
     */
    inline void on_websocket_message(std::string const& message)
    {
        // Note: No mutex, for the same reason as in on_websocket_open.
        if (m_message_callback) {
            m_message_callback(message);
        }
    }

    const std::string m_address{};
    WebsocketOptions m_options{};

    std::mutex m_mutex{};
    bool m_started{ false };
    std::function<void()> m_open_callback{};
    std::function<void()> m_close_callback{};
    std::function<void(std::string const& message)> m_message_callback{};
};

constexpr loon::LogLevel default_log_level = LogLevel::Error;

void log_level(LogLevel level);
void log_handler(log_handler_t handler);

} // namespace loon::websocket
