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
     * The URL under which the content is accessible.
     *
     * @returns A valid HTTP or HTTPS url.
     */
    virtual std::string const& url() const = 0;
};

class IClient
{
public:
    /**
     * Connects to the server and maintains a websocket connection.
     * Attempts to reconnect on connection failure or disconnect,
     * until the stop method is called.
     * Returns immediately and does nothing, if already connecting or connected.
     */
    virtual void start() = 0;

    /**
     * Disconnects from the server.
     * Returns immediately and does nothing, if already disconnected.
     * Notifies all handles that registered content is not available anymore.
     */
    virtual void stop() = 0;

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
    virtual std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<ContentSource> source, ContentInfo const& info) = 0;

    /**
     * Unregisters content from this client that is under the given handle.
     *
     * @param handle The handle for which the content should be unregistered.
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
