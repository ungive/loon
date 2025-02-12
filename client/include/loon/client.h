#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "loon/content.h"

namespace loon
{
/**
 * @brief A handle for content registered with a Client.
 *
 */
class ContentHandle
{
public:
    /**
     * @brief The URL under which the content is accessible.
     *
     * @returns A valid HTTP or HTTPS url.
     */
    virtual std::string const& url() const = 0;

    /**
     * @brief Sets a callback for when the content was successfully served.
     *
     * The callback function is called in the event that
     * the registered content has been requested and successfully served.
     *
     * The callback will not be called for past served requests,
     * if it is set after the URL for this content has been requested.
     * The callback should therefore be set first,
     * before calling url() and request or sharing the returned URL.
     *
     * Do not call any client methods within the callback,
     * as client locks are held when it is called,
     * which would cause a deadlock.
     *
     * @param callback The callback function to set.
     */
    virtual void on_served(std::function<void()> callback) = 0;

    /**
     * @brief Sets a callback for when the content is unregistered.
     *
     * The callback function is called in the event that
     * the content has been unregistered from the client manually,
     * the connection to the server has been closed manually,
     * the server has closed the connection to the client,
     * an error occured that prevented the content from being served,
     * or if the content is not registered with the client anymore,
     * when this method was called to set the callback.
     *
     * The callback is called at most once.
     * Do not call any client methods within the callback,
     * as client locks are held when it is called,
     * which would cause a deadlock.
     *
     * @param callback The callback function to set.
     */
    virtual void on_unregistered(std::function<void()> callback) = 0;
};

/**
 * @brief The interface for a loon client.
 */
class IClient
{
public:
    virtual ~IClient(){};

    /**
     * @brief Connects to the server and maintains a connection.
     *
     * Attempts to reconnect on connection failure or disconnect,
     * until stop() is called.
     * Returns immediately and does nothing, if already starting or started.
     * Disables idling, if the client is in idle state.
     */
    virtual void start() = 0;

    /**
     * @brief Stops the client and disconnects in the background.
     *
     * This method has a similar effect as terminate() but returns immediately.
     * The start() method may still be called again after calling this method.
     * Callbacks may still be called after this method returns.
     */
    virtual void stop() = 0;

    /**
     * @brief Aborts any connection and waits until all threads terminated.
     *
     * Terminates the client and closes the connection to the server.
     * Notifies all handles that registered content is not available anymore.
     * Also disables idling, if it has been enabled before.
     * Does nothing, if already disconnected or stopped.
     *
     * Returns once all connections and threads have been terminated. It is
     * guaranteed that no registered callbacks will be called after this method
     * returns. Callbacks may still be called before this method returns.
     *
     * The start() method may still be called again after calling this method.
     */
    virtual void terminate() = 0;

    /**
     * @brief Whether the client is started.
     *
     * Call wait_until_ready() to wait until the client is ready
     * for registering content with register_content().
     *
     * @returns If the client is started.
     */
    virtual bool started() = 0;

    /**
     * @brief Puts the client into idle.
     *
     * Enable idling when the client is not in active use right now
     * and registered content is not required to be available online,
     * but the client might be put to use again in the very near future,
     * where disconnecting immediately might be considered inefficient.
     *
     * Once idling, the client disconnects from the server after the timeout
     * that was configured with ClientOptions::disconnect_after_idle.
     * To put it out of idle again, either call register_content()
     * or call start() again, if content is already registered.
     * Calling stop() also puts the client out of idling.
     *
     * If no timeout period was configured, this method has no effect.
     * If the client is currently stopped, this method has no effect.
     * If the client is disconnected and in the process of reconnecting,
     * the client will be put into idle at the point of reconnecting,
     * if automatic idling has been enabled in the client options.
     *
     * Note that the client might be put into idle automatically,
     * depending on the value of ClientOptions::automatic_idling.
     *
     * @see ClientOptions::disconnect_after_idle
     * @see ClientOptions::automatic_idling
     */
    virtual void idle() = 0;

    /**
     * @brief Wait until the client is ready.
     *
     * Times out after the given timeout duration,
     * if the client has not successfully connected within that period.
     * The timeout duration must be greater or equal to zero.
     *
     * Can be used to check whether the client is ready
     * at a specific moment, by using a timeout duration of zero.
     *
     * Does not throw any exception if the connection is ready.
     *
     * @param timeout The timeout period.
     *
     * @returns if the client was already ready
     * and the method returned immediately.
     *
     * @throws TimeoutException
     * if an operation timed out.
     * @throws ClientNotConnectedException
     * if the client is not started, connected or
     * if the client disconnected while waiting for the connection to be ready.
     */
    virtual bool wait_until_ready(std::chrono::milliseconds timeout) = 0;

    /**
     * @brief Wait until the client is ready.
     *
     * Uses the connect timeout or a sane default value for the timeout.
     *
     * @returns if the client was already ready
     * and the method returned immediately.
     *
     * @see IClient::wait_until_ready(timeout)
     * @see WebsocketOptions::connect_timeout
     */
    virtual bool wait_until_ready() = 0;

    /**
     * @brief Sets a callback for when the client is connected and ready.
     *
     * The registered callback is guaranteed to be executed
     * before any call to wait_until_ready() returns.
     *
     * It is recommended to call this method before start(),
     * to reliably detect client ready state.
     * Do not call any client methods within the callback,
     * as client locks are held when it is called,
     * which would cause a deadlock.
     */
    virtual void on_ready(std::function<void()> callback) = 0;

    /**
     * @brief Sets a callback for when the client disconnected.
     *
     * This callback is guaranteed to be called, even when the client fails.
     *
     * It is recommended to call this method before start(),
     * to reliably detect client ready state.
     * Do not call any client methods within the callback,
     * as client locks are held when it is called,
     * which would cause a deadlock.
     */
    virtual void on_disconnect(std::function<void()> callback) = 0;

    /**
     * @brief Sets a callback for when the client has unrecoverably failed.
     *
     * This callback is only called when an abnormal event occurs,
     * like an unacceptable server configuration that does not allow
     * the client to continue operation.
     * The callback is not called when the client is stopped with stop()
     * or when the Client instance is destructed in the destructor.
     * The client is stopped when this method is called.
     *
     * It is recommended to call this method before start(),
     * to reliably detect client failures.
     * Do not call any client methods within the callback,
     * as client locks are held when it is called,
     * which would cause a deadlock.
     *
     * @param callback The function to call when the client failed.
     */
    virtual void on_failed(std::function<void()> callback) = 0;

    /**
     * @brief Registers content with this client and returns a handle for it.
     *
     * The content is registered for the lifetime of the websocket connection
     * or until it is unregistered with unregister_content().
     * It is necessary to re-register the content again after a disconnect.
     *
     * Calls to any of the methods of the source are synchronized.
     * The source's data() method is only called once per request
     * and while no other requests are being handled
     * that are using a previous return value of it.
     * If the source reads from a file e.g.,
     * the data() method can safely seek to the beginning of the file
     * without having to worry about corrupting other ongoing requests.
     *
     * If the client is idling, the client is put out of idle.
     *
     * @param source The source for the content.
     * @param info Information about how to provide the content.
     * @param timeout How long to wait until the connection is ready,
     * if it isn't yet.
     *
     * @returns A handle to the content.
     * For use with unregister_content() to unregister this content again.
     *
     * @throws TimeoutException
     * if an operation timed out.
     * @throws ClientNotConnectedException
     * if the client is not started, connected or
     * if the client disconnected while waiting for the connection to be ready.
     * @throws PathAlreadyRegisteredException
     * if content has already been registered under the path
     * that was specified in the info parameter.
     * @throws MalformedContentException
     * if the content has a size of zero bytes.
     * @throws UnacceptableContentException
     * if the server's constraints do not allow this content.
     */
    virtual std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<ContentSource> source, ContentInfo const& info,
        std::chrono::milliseconds timeout) = 0;

    /**
     * @brief Registers content with this client and returns a handle for it.
     *
     * Uses the connect timeout or a sane default value for the timeout.
     *
     * @see IClient::register_content(source, info, timeout)
     * @see WebsocketOptions::connect_timeout
     */
    virtual std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<ContentSource> source, ContentInfo const& info) = 0;

    /**
     * @brief Unregisters registered content from this client.
     *
     * Does nothing if the content is already not registered anymore
     * or the client has disconnected from the server or failed.
     *
     * @param handle The handle for which the content should be unregistered.
     *
     * @throws MalformedContentException
     * if the content is null or has the wrong type.
     */
    virtual void unregister_content(std::shared_ptr<ContentHandle> handle) = 0;

    /**
     * @brief Returns all handles of content that is currently registered.
     *
     * Represents a snapshot of registered content.
     * Might get invalidated if any of the handles are unregistered
     * or if the client disconnects, fails or is stopped in the meantime.
     *
     * @returns A list of content handles.
     */
    virtual std::vector<std::shared_ptr<ContentHandle>> content() = 0;

    // TODO: maybe call this "content_state" which returns one of many states,
    //   like "invalid content", "client not connected", "not registered", ...

    /**
     * @brief Checks whether the content handle is still registered and served.
     *
     * @param handle The content handle to check.
     *
     * @returns Whether the content handle is still registered with this client.
     *
     * @throws MalformedContentException
     * if the content is null or has the wrong type.
     */
    virtual bool is_registered(std::shared_ptr<ContentHandle> handle) = 0;
};

/**
 * @brief Websocket options for connecting to a loon server.
 */
struct WebsocketOptions
{
    /**
     * @brief A set of HTTP headers to use during connects.
     */
    std::map<std::string, std::string> headers{};

    /**
     * @brief Credentials for HTTP Basic authorization.
     *
     * Must be a RFC 7617 compliant value for Basic authorization credentials.
     * Must not be base64-encoded, encoding is done by the implementation.
     * The value must be a user-id and a password, separated by a single colon
     * or a different value that are valid credentials.
     *
     * If this value is set, the Authorization header will be overwritten.
     * No exception will be thrown if both an Authorization header
     * and this field are set.
     */
    std::optional<std::string> basic_authorization{};

    /**
     * @brief A path to a CA certificate to authenticate the server.
     *
     * In case the QT backend is used for websocket connections,
     * this may be a QT resource path, as defined in the QT documentation,
     * the path is then passed as an argument to the QFile constructor:
     * https://doc.qt.io/qt-6/resources.html
     * https://doc.qt.io/qt-6/qfile.html
     *
     * If this value is set, ca_certificate must be empty.
     */
    std::optional<std::string> ca_certificate_path{};

    /**
     * @brief An in-memory CA certificate to authenticate the server.
     *
     * If this value is set, ca_certificate_path must be empty.
     */
    std::optional<std::vector<uint8_t>> ca_certificate{};

    /**
     * @brief The time after which a connection attempt times out.
     *
     * If not set, a sane default value is used.
     */
    std::optional<std::chrono::milliseconds> connect_timeout{};

    /**
     * @brief The interval in which ping messages are sent.
     *
     * The client closes the connection and attempts to reconnect,
     * if a ping message does not receive a pong response in time.
     * The ping interval is used as the pong timeout.
     *
     * If not set, a sane default value is used.
     */
    std::optional<std::chrono::milliseconds> ping_interval{};

    /**
     * @brief The delay between reconnects.
     *
     * If not set, the client will not attempt to reconnect after a close.
     * Otherwise reconnect attempts are made in the given interval.
     */
    std::optional<std::chrono::milliseconds> reconnect_delay{};

    /**
     * @brief The maximum delay between reconnects.
     *
     * If set, the reconnect delay will be continuously increased
     * (exactly how is left to the implementation)
     * until the maximum reconnect delay.
     * If not set, the reconnect delay remains constant.
     *
     * reconnect_delay must be set. The value of max_reconnect_delay
     * must be greater than reconnect_delay.
     */
    std::optional<std::chrono::milliseconds> max_reconnect_delay{};
};

/**
 * @brief Options for creating a loon client.
 */
struct ClientOptions
{
    /**
     * @brief The underlying websocket options to use.
     */
    WebsocketOptions websocket{};

    /**
     * @brief The duration for which responses must be cached by the server.
     *
     * Within this time period, at most one request may be received
     * for an individual piece of registered content,
     * otherwise the connection is closed and the client reconnects.
     *
     * The value must be greater than zero, if set.
     * The client fails and stops if the server does not support caching.
     */
    std::optional<std::chrono::seconds> min_cache_duration{};

    /**
     * @brief Maximum number of requests per time frame for missing content.
     *
     * Designates how many requests (first pair value)
     * are allowed within a given time frame (second pair value)
     * for request paths that lead to content that is not registered anymore.
     * If this limit is exceeded, the client simply disconnects and reconnects,
     * leading to a new client ID and thereby
     * invalidating all previously generated URLs.
     *
     * The purpose of this option is to make sure the client
     * is not flooded with requests that lead to no content.
     * To simplify the implementation, requests that lead to no content
     * are not tracked individually, but grouped together.
     * Therefore a general limit like this is employed
     * as a means to limit incoming requests for no content.
     *
     * It is recommended to set this limit as high as possible,
     * to not disrupt the normal flow of your application.
     * The exact value should be chosen depending on:
     * - how much content is registered,
     * - how many invalid generated URLs might exist during program execution,
     * - which third parties receive generated URLs,
     * - when third parties might discard these generated URLs,
     *   i.e. how many URLs might be in circulation at a time,
     * - how often these URLs are being requested,
     * - and how long the server caches responses.
     *
     * Both pair values must be greater than zero.
     * It is recommended to set this value manually.
     * An explicit value must be set.
     *
     * If no limit is required, set this value to nullopt.
     */
    std::optional<std::pair<size_t, std::chrono::milliseconds>>
        no_content_request_limit{ std::make_pair(
            -1, std::chrono::seconds{ 0 }) };

    /**
     * @brief Automatically disconnect after the given idle duration.
     *
     * Idling is defined as being connected without having content registered
     * or having entered idle state explicitly with the IClient::idle() method.
     * If no content is registered within the given duration,
     * the client will disconnect from the server.
     *
     * Should content be registered while being disconnected due to idling,
     * then the client will reconnect to the server
     * and then register the content, if the client is in started state.
     *
     * @see automatic_idling
     * @see IClient::idle()
     */
    std::optional<std::chrono::milliseconds> disconnect_after_idle{};

    /**
     * @brief Enables automatic idling whenever no content is registered.
     *
     * The client is put into idle automatically when:
     * - The client connects and is ready, but no content is registered yet
     * - Content was unregistered and there is no registered content anymore
     *
     * This value is ignored when disconnect_after_idle is not set.
     *
     * @see disconnect_after_idle
     * @see IClient::idle()
     */
    bool automatic_idling{ true };

    // TODO: not yet implemented
    /**
     * @brief Maximum total upload speed for the client in bytes per second.
     *
     * Limits the upload speed for all requests that are served by the client.
     * If multiple requests are served simultaneously,
     * this limit is evenly split across every response
     * on a best-effort basis.
     */
    std::optional<size_t> max_upload_speed{};
};

/**
 * @brief The loon client.
 */
class Client : public IClient
{
public:
    /**
     * Creates a new loon client with authentication credentials.
     *
     * @param address The websocket address to connect to.
     * @param options Client options.
     * @throws std::exception if client creation failed.
     */
    Client(std::string const& address, ClientOptions options = {});

    inline void start() override { return m_impl->start(); }

    inline void stop() override { return m_impl->stop(); }

    inline void terminate() override { return m_impl->terminate(); }

    inline bool started() override { return m_impl->started(); }

    inline void idle() override { return m_impl->idle(); }

    inline bool wait_until_ready() override
    {
        return m_impl->wait_until_ready();
    }

    inline bool wait_until_ready(std::chrono::milliseconds timeout) override
    {
        return m_impl->wait_until_ready(timeout);
    }

    inline void on_ready(std::function<void()> callback) override
    {
        return m_impl->on_ready(callback);
    }

    inline void on_disconnect(std::function<void()> callback) override
    {
        return m_impl->on_disconnect(callback);
    }

    inline void on_failed(std::function<void()> callback) override
    {
        return m_impl->on_failed(callback);
    }

    inline std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info,
        std::chrono::milliseconds timeout) override
    {
        return m_impl->register_content(source, info, timeout);
    }

    inline std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info) override
    {
        return m_impl->register_content(source, info);
    }

    inline void unregister_content(
        std::shared_ptr<ContentHandle> handle) override
    {
        return m_impl->unregister_content(handle);
    }

    inline std::vector<std::shared_ptr<ContentHandle>> content() override
    {
        return m_impl->content();
    }

    inline bool is_registered(std::shared_ptr<ContentHandle> handle) override
    {
        return m_impl->is_registered(handle);
    }

private:
    std::unique_ptr<IClient> m_impl;
};

/**
 * @brief Severity levels for log messages.
 *
 * Severity increases with the integer value of the log level.
 */
enum class LogLevel
{
    Debug,   /// Messages to aid with debugging.
    Info,    /// Informational messages, like served requests.
    Warning, /// Events and errors that can be handled gracefully.
    Error,   /// Errors which cause the client to restart the connection.
    Fatal,   /// Errors which cause the client to fail and stop.
    Silent,  /// Log nothing.
};

using log_handler_t =
    std::function<void(LogLevel level, std::string const& message)>;

/**
 * @brief Set the minimum level for loon client log messages.
 *
 * Log messages with a level less than this level will not be logged.
 *
 * @param level The log level.
 */
void client_log_level(LogLevel level);

/**
 * @brief Set the minimum level for websocket log messages.
 *
 * Websocket log messages with a level less than this level will not be logged.
 *
 * @param level The log level.
 */
void websocket_log_level(LogLevel level);

/**
 * @brief A custom handler function for handling log messages.
 *
 * If not set, log messages will be written to stderr by default.
 * If a nullptr is passed, the default log handler will be used.
 *
 * @param handler The log handler.
 */
void log_handler(log_handler_t handler);

/**
 * @brief The client is not started.
 */
class ClientNotStartedException : public std::runtime_error
{
public:
    using runtime_error::runtime_error;
};

/**
 * @brief The client is not connected to the server.
 */
class ClientNotConnectedException : public std::runtime_error
{
public:
    using runtime_error::runtime_error;
};

/**
 * @brief Content is already registered under this path.
 */
class PathAlreadyRegisteredException : public std::runtime_error
{
public:
    using runtime_error::runtime_error;
};

/**
 * @brief No content is registered under this handle with this client.
 */
class ContentNotRegisteredException : public std::runtime_error
{
public:
    using runtime_error::runtime_error;
};

/**
 * @brief The content is malformed.
 *
 * This could either be because the pointer is a null pointer
 * or it points to a content instance that has an invalid type.
 */
class MalformedContentException : public std::runtime_error
{
public:
    using runtime_error::runtime_error;
};

/**
 * @brief The content does not conform with the server's constraints.
 */
class UnacceptableContentException : public std::runtime_error
{
public:
    using runtime_error::runtime_error;
};

/**
 * @brief The operation timed out.
 *
 */
class TimeoutException : public std::runtime_error
{
public:
    using runtime_error::runtime_error;
};
} // namespace loon
