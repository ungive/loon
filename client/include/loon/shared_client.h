#pragma once

#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

#include "loon/client.h"

namespace loon
{
namespace internal
{
template <typename T>
struct fail : std::false_type
{
};
} // namespace internal

/**
 * @brief A client that uses a shared connection by wrapping a real client.
 *
 * A shared client is useful when the proxy is to be used in different contexts
 * within the same application process, where it's undesirable to create
 * multiple clients and therefore multiple connections to the same server.
 * This class wraps a real client and orchestrates method calls in such a way
 * that multiple shared clients can act independently and concurrently using
 * the same underlying connection.
 */
class ISharedClient : public IClient
{
public:
    /**
     * @brief The instance index of this shared client.
     *
     * The index is unique for all shared clients that wrap a particular client.
     * This value is also used as a path prefix for registered content.
     *
     * @returns The index of this shared client.
     */
    virtual size_t index() const = 0;

    /**
     * @brief The path prefix for content registered with this shared client.
     *
     * This is unique for all shared clients that wrap a particular client.
     * The prefix is directly prepended to any path for registered content.
     *
     * @returns The path prefix for registered content.
     */
    virtual std::string const& path_prefix() const = 0;

    /**
     * @brief Shared clients cannot be terminated.
     *
     * Triggers an assertion error when called, does nothing otherwise.
     */
    inline void terminate() override
    {
        assert(false && "terminate cannot be called on a shared client");
    }
};

class SharedClient : public ISharedClient
{
public:
    /**
     * @brief Creates a shared client from an existing client.
     *
     * The existing client may not be used directly and may only be used
     * via the methods provided by any shared client instance.
     *
     * @param client The underlying client to use.
     * @returns A shared client instance that makes use of the given client.
     */
    SharedClient(std::shared_ptr<IClient> client);

    // Shared client methods

    inline size_t index() const override { return m_impl->index(); }

    inline std::string const& path_prefix() const override
    {
        return m_impl->path_prefix();
    }

    // Client implementation methods with significantly altered logic

    /**
     * @brief Stops the shared client.
     *
     * This only stops the underyling client,
     * once all shared clients are stopped.
     *
     * @see IClient::stop
     */
    inline void stop() override { return m_impl->stop(); }

    /**
     * @brief Puts the shared client into idle.
     *
     * This only puts the underlying client into idle,
     * once all shared clients are idling.
     *
     * @see IClient::idle
     */
    inline void idle() override { return m_impl->idle(); }

    /**
     * @brief Registers content with this client and returns a handle for it.
     *
     * The content is not registered directly under the given path.
     * The path is prefixed with the return value of path_prefix()
     * since multiple shared clients can register under identical paths.
     *
     * @see IClient::register_content
     */
    inline std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info,
        std::chrono::milliseconds timeout) override
    {
        return m_impl->register_content(source, info, timeout);
    }

    /**
     * @brief Registers content with this client and returns a handle for it.
     *
     * The content is not registered directly under the given path.
     * The path is prefixed with the return value of path_prefix()
     * since multiple shared clients can register under identical paths.
     *
     * @see IClient::register_content
     */
    inline std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info) override
    {
        return m_impl->register_content(source, info);
    }

    template <typename T = bool>
    inline void terminate()
    {
        // Ensure that this call does not compile.
        static_assert(internal::fail<T>::value,
            "terminate cannot be called on a shared client");
    }

    // Client implementation methods

    inline void start() override { return m_impl->start(); }

    inline bool started() override { return m_impl->started(); }

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
    std::unique_ptr<ISharedClient> m_impl;
};
} // namespace loon
