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
     * the registered content has been request and successfully served.
     *
     * The callback will not be called for past served requests,
     * if it is set after the URL for this content has been requested.
     * The callback should therefore be set first,
     * before calling url() and request or sharing the returned URL.
     *
     * @param callback The callback function to set.
     */
    virtual void served(std::function<void()> callback) = 0;

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
     * Do not call any client methods within the callback.
     *
     * @param callback The callback function to set.
     */
    virtual void unregistered(std::function<void()> callback) = 0;
};

/**
 * @brief The interface for a loon client.
 */
class IClient
{
public:
    virtual ~IClient() {};

    /**
     * @brief Connects to the server and maintains a connection.
     *
     * Attempts to reconnect on connection failure or disconnect,
     * until stop() is called.
     * Returns immediately and does nothing, if already starting or started.
     */
    virtual void start() = 0;

    /**
     * @brief Stops the client and disconnects from the server.
     *
     * Returns immediately and does nothing, if already disconnected.
     * Notifies all handles that registered content is not available anymore.
     */
    virtual void stop() = 0;

    /**
     * @brief Whether the client is connected and ready for content.
     *
     * @returns If the client is ready for registering content.
     */
    virtual bool connected() = 0;

    /**
     * @brief Waits until the client is connected.
     *
     * Times out after the connect timeout that was configured
     * within the WebsocketOptions of the client
     * or the default timeout if not explicitly configured.
     *
     * Must be called while the client is started.
     *
     * @param timeout
     * @throws ClientNotStartedException if the client is not started.
     */
    virtual bool wait_until_connected() = 0;

    /**
     * @brief Waits until the client is connected.
     *
     * Times out after the given timeout duration,
     * if the client has not successfully connected within that period.
     * The timeout duration must be greater or equal to zero.
     *
     * Can be used to check whether the client is connected
     * at a specific moment, by using a timeout duration of zero.
     *
     * @param timeout The timeout period.
     * @throws ClientNotStartedException if the client is not started.
     */
    virtual bool wait_until_connected(std::chrono::milliseconds timeout) = 0;

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
     * Do not call any client methods within the callback.
     *
     * @param callback The function to call when the client failed.
     */
    virtual void on_failed(std::function<void()> callback) = 0;

    // TODO: probably wanna rename these to register() and unregister().

    /**
     * @brief Registers content with this client and returns a handle for it.
     *
     * The content is registered for the lifetime of the websocket connection
     * or until it is unregistered with unregister_content().
     * If it is necessary to re-register the content again after a disconnect.
     *
     * Calls to any of the methods of the source are synchronized.
     * The source's data() method is only called once per request
     * and while no other requests are being handled
     * that are using a previous return value of it.
     * If the source reads from a file e.g.,
     * the data() method can safely seek to the beginning of the file
     * without having to worry about corrupting other ongoing requests.
     *
     * @param source The source for the content.
     * @param info Information about how to provide the content.
     *
     * @returns A handle to the content.
     * For use with unregister_content() to unregister this content again.
     * @throws ClientNotConnectedException
     * if the client is not started, connected or
     * if the client disconnected while waiting for the connection to be ready.
     * @throws ClientFailedException
     * if the client failed while attempting to register the content.
     * @throws PathAlreadyRegisteredException
     * if content has already been registered under the path
     * that was specified in the info parameter.
     * @throws UnacceptableContentException
     * if the server's constraints do not allow this content.
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
     * @throws MalformedContentException if the content is invalid.
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
     * @throws MalformedContentException if the content is invalid.
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

    inline bool connected() override { return m_impl->connected(); }

    inline bool wait_until_connected() override
    {
        return m_impl->wait_until_connected();
    }

    inline bool wait_until_connected(std::chrono::milliseconds timeout) override
    {
        return m_impl->wait_until_connected(timeout);
    }

    inline void on_failed(std::function<void()> callback) override
    {
        return m_impl->on_failed(callback);
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
 * @brief The client is in a failed state and was stopped.
 */
class ClientFailedException : public std::runtime_error
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
} // namespace loon
