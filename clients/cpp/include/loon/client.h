#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "loon/content.h"

namespace loon
{
/**
 * A handle for content registered with a Client.
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
     * Any set callback is called at most once.
     *
     * @param callback The callback function to set.
     */
    virtual void unregistered(std::function<void()> callback) = 0;
};

class IClient
{
public:
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
     * @brief Sets a callback for when the client has unrecoverably failed.
     *
     * This callback is only called when an abnormal event occurs,
     * like an unacceptable server configuration that does not allow
     * the client to continue operation.
     * The callback is not called when the client is stopped with stop()
     * or when the Client instance is destructed in the destructor.
     *
     * If failure should be detected, this method must be called before start().
     *
     * @param callback The function to call when the client failed.
     */
    virtual void failed(std::function<void()> callback) = 0;

    /**
     * @brief Registers content with this client and returns a handle for it.
     *
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
     * @param info Information about how to provide the content.
     *
     * @returns A handle to the content.
     * For use with unregister_content() to unregister this content again.
     * @throws ClientNotConnectedException if the client is not connected
     * when the method is called or if the client disconnected
     * while waiting for the connection to be ready.
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
     * @param handle The handle for which the content should be unregistered.
     *
     * @throws ClientNotConnectedException if the client is not connected.
     * @throws ContentNotRegisteredException
     * if the content is not registered with this client.
     */
    virtual void unregister_content(std::shared_ptr<ContentHandle> handle) = 0;

    /**
     * @brief Unregisters all registered content from this client.
     *
     * Does nothing if the client is not connected or no content is registered.
     * Does not throw any exception.
     *
     * @param with_callbacks If true, call all unregistered callbacks.
     * Otherwise content's unregistered callbacks are not called.
     */
    virtual void unregister_all_content(bool with_callbacks = true) = 0;
};

struct ClientOptions
{
    /**
     * @brief The minimum duration for which
     * responses must be cached by the server, in seconds.
     *
     * This means that when another request comes in for the same content,
     * within the time period of this duration,
     * the client will fail and stop with an error.
     *
     * The client also fails if the server does not support caching.
     */
    std::optional<uint32_t> min_cache_duration{};

    /**
     * @brief The maximum number of requests to handle per second.
     *
     * If this number is exceeded, the client restarts.
     * Any registered content will be unregistered and must be registered again.
     * Unregistered callbacks will be called on each piece of content.
     *
     * If fail_on_too_many_requests is set, the client fails instead.
     *
     * This option exists in the case that a malicious third party
     * attempts to spam this client with requests,
     * which might cause an extraneous
     */
    std::optional<double> max_requests_per_second{};

    /**
     * @brief If set to true, this client fails on too many requests.
     *
     * How many requests exactly consitute as "too many requests"
     * is determined by the value for max_requests_per_second,
     * which must be set if this flag is true.
     *
     * On failure, the client stops with an error
     * and must be started again manually to continue.
     */
    bool fail_on_too_many_requests{ false };

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

class Client : public IClient
{
public:
    /**
     * Creates a new loon client with authentication credentials.
     *
     * @param address The websocket address to connect to.
     * @param auth The HTTP Basic authentication string,
     * a username and a password, separated by a colon.
     * @param options Additional client options.
     */
    Client(std::string const& address,
        std::string const& auth,
        ClientOptions options = {});

    /**
     * Creates a new loon client without any authentication.
     *
     * @param address The websocket address to connect to.
     * @param options Additional client options.
     */
    Client(std::string const& address, ClientOptions options = {});

    void Client::start() override { return m_impl->start(); }

    void Client::stop() override { return m_impl->stop(); }

    void loon::Client::failed(std::function<void()> callback) override
    {
        return m_impl->failed(callback);
    }

    std::shared_ptr<ContentHandle> Client::register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info) override
    {
        return m_impl->register_content(source, info);
    }

    void Client::unregister_content(
        std::shared_ptr<ContentHandle> handle) override
    {
        return m_impl->unregister_content(handle);
    }

    void unregister_all_content(bool with_callbacks = true) override
    {
        return m_impl->unregister_all_content(with_callbacks);
    }

private:
    std::unique_ptr<IClient> m_impl;
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
 * @brief The content does not conform with the server's constraints.
 */
class UnacceptableContentException : public std::runtime_error
{
public:
    using runtime_error::runtime_error;
};
} // namespace loon
