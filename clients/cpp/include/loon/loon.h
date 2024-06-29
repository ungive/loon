#pragma once

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
     */
    Client(std::string const& address, std::string const& auth);

    /**
     * Creates a new loon client without any authentication.
     *
     * @param address The websocket address to connect to.
     */
    Client(std::string const& address);

    void start() override;

    void stop() override;

    std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<ContentSource> source,
        ContentInfo const& info) override;

    void unregister_content(std::shared_ptr<ContentHandle> handle) override;

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
