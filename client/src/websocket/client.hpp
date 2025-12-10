#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "loon/client.hpp"

// TODO proper version string
#define LOON_USER_AGENT "loon-cpp-client/0.1"

namespace loon::websocket
{

constexpr loon::LogLevel default_log_level = LogLevel::Error;

void log_level(LogLevel level);
void log_handler(log_handler_t handler);
log_handler_t log_handler();

extern std::chrono::milliseconds default_connect_timeout;
extern std::chrono::milliseconds default_ping_interval;

class IClient
{
public:
    virtual ~IClient() = default;

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
     * If a mutex is held while this method is called,
     * it can safely be locked from any of the event handlers.
     */
    virtual void start() = 0;

    /**
     * @brief Stops the websocket client.
     *
     * Does nothing, if the client is not started or already stopped.
     *
     * This method is guaranteed to return immediately,
     * it will not block until any of the event handlers
     * have been called and returned.
     * If a mutex is held while this method is called,
     * it can safely be locked from any of the event handlers.
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

    virtual int64_t send_binary(const char* data, size_t length) override = 0;

    virtual int64_t send_text(const char* data, size_t length) override = 0;

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
        m_connected.store(true);
        if (m_open_callback) {
            try {
                m_open_callback();
            }
            catch (std::exception const& e) {
                log_handler()(LogLevel::Error,
                    std::string("uncaught exception in on_websocket_open: ") +
                        e.what());
            }
            catch (...) {
                log_handler()(LogLevel::Error,
                    std::string("uncaught exception in on_websocket_open"));
            }
        }
    }

    /**
     * @brief Call this method when the websocket connection is closed.
     */
    inline void on_websocket_close()
    {
        m_active.store(false);
        m_connected.store(false);
        if (m_close_callback) {
            try {
                m_close_callback();
            }
            catch (std::exception const& e) {
                log_handler()(LogLevel::Error,
                    std::string("uncaught exception in on_websocket_close: ") +
                        e.what());
            }
            catch (...) {
                log_handler()(LogLevel::Error,
                    std::string("uncaught exception in on_websocket_close"));
            }
        }
    }

    /**
     * @brief Call this method when a websocket message is received.
     */
    inline void on_websocket_message(std::string const& message)
    {
        if (m_message_callback) {
            try {
                m_message_callback(message);
            }
            catch (std::exception const& e) {
                log_handler()(LogLevel::Error,
                    std::string(
                        "uncaught exception in on_websocket_message: ") +
                        e.what());
            }
            catch (...) {
                log_handler()(LogLevel::Error,
                    std::string("uncaught exception in on_websocket_message"));
            }
        }
    }

    /**
     * @brief Call this method when there is a fatal error.
     *
     * Stops any running connection by calling internal_stop().
     * Prevents the client from reconnecting, if reconnects are enabled
     * and puts the client in a stopped state.
     * After calling this method, active() returns false.
     */
    inline void fail()
    {
        m_active.exchange(false);
        internal_stop();
    }

    inline WebsocketOptions const& options() const { return m_options; }

    inline bool active() const { return m_active.load(); }

    inline bool active(bool new_value) { return m_active.exchange(new_value); }

    inline bool connected() const { return m_connected.load(); }

private:
    const std::string m_address{};
    const WebsocketOptions m_options{};

    std::atomic<bool> m_active{ false };
    std::atomic<bool> m_connected{ false };
    std::function<void()> m_open_callback{};
    std::function<void()> m_close_callback{};
    std::function<void(std::string const& message)> m_message_callback{};
};

} // namespace loon::websocket
