#pragma once

#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "loon/shared_client.h"

namespace loon
{
class SharedClientImpl : public ISharedClient
{
public:
    SharedClientImpl(std::shared_ptr<IClient> client);

    ~SharedClientImpl();

    size_t index() const;

    std::string const& path_prefix() const;

    void start() override;

    void stop() override;

    bool started() override;

    void idle() override;

    bool wait_until_ready() override;

    bool wait_until_ready(std::chrono::milliseconds timeout) override;

    void on_ready(std::function<void()> callback) override;

    void on_disconnect(std::function<void()> callback) override;

    void on_failed(std::function<void()> callback) override;

    std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info,
        std::chrono::milliseconds timeout) override;

    std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info) override;

    void unregister_content(std::shared_ptr<ContentHandle> handle) override;

    std::vector<std::shared_ptr<ContentHandle>> content() override;

    bool is_registered(std::shared_ptr<ContentHandle> handle) override;

private:
    bool internal_started();
    void internal_reset_idling();
    loon::ContentInfo internal_modified_content_info(
        loon::ContentInfo const& info) const;

    std::mutex m_mutex;

    std::shared_ptr<IClient> m_client{};
    size_t m_index{};

    bool m_started{ false };
    bool m_idling{ false };
    std::unordered_set<std::shared_ptr<loon::ContentHandle>> m_registered{};
};

class SharedReferenceCounter
{
public:
    /**
     * @brief Adds a reference count for the given client.
     *
     * Returns a unique number/index for this reference count
     * that is unique from all other reference counts.
     * Counting starts at 0 and is reset when all clients were removed.
     *
     * @param client The client to add a reference count for.
     * @returns Returns a unique reference count index.
     */
    size_t add(std::shared_ptr<IClient> client);

    /**
     * @brief Removes a reference count for the given client.
     *
     * Does not throw when there is no reference count for this client,
     * instead triggers an assertion error.
     *
     * @param client The client to remove a reference count for.
     */
    void remove(std::shared_ptr<IClient> client);

    /**
     * @brief Returns the number of reference counts for the given client.
     *
     * Returns 0 if there are no reference counts for this client.
     *
     * The required parameter causes an assertion error when there
     * are zero reference for the given client.
     *
     * @param client The client for which to return the reference counts.
     * @param required Whether there need to be one or more references.
     * @returns The number of reference counts for this client.
     */
    size_t count(std::shared_ptr<IClient> client, bool required = false);

    /**
     * @brief Returns whether there is only a single reference for the client.
     *
     * With the required parameter set (which is the default), the method
     * operates on the assumption that the caller expects there to be any
     * amount of references greater or equal to 1.
     *
     * The required parameter acts the same as in the count() method.
     *
     * @param client The client to check.
     * @param required Whether there need to be one or more references.
     * @returns Whether there is exactly one reference for the given client.
     */
    inline bool single(std::shared_ptr<IClient> client, bool required = true)
    {
        return count(client, required) == 1;
    }

private:
    struct ReferenceCounter
    {
        size_t references{ 0 };
        size_t next_index{ 0 };
    };

    // It's okay to keep a shared_ptr reference to the client here, even though
    // it increases the pointer's internal reference count, since it will be
    // removed here when no shared client instances exist anymore.
    std::unordered_map<std::shared_ptr<IClient>, ReferenceCounter> m_refs;
};
} // namespace loon
