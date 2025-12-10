#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "loon/shared_client.hpp"

namespace loon
{
class SharedClientImpl : public ISharedClient
{
public:
    SharedClientImpl(std::shared_ptr<IClient> client);

    SharedClientImpl(SharedClientImpl const& other) = delete;

    SharedClientImpl(SharedClientImpl&& other) = delete;

    ~SharedClientImpl();

    size_t index() const override;

    std::string const& path_prefix() const override;

    void start() override;

    void stop() override;

    bool started() override;

    void idle(bool state) override;

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

#ifdef LOON_TEST
protected:
    /**
     * @brief How long to sleep before calling the ready callback in start().
     */
    inline void before_manual_start_callback_sleep(
        std::chrono::milliseconds duration = std::chrono::milliseconds::zero())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_before_manual_start_callback_sleep_duration = duration;
    }

private:
    std::chrono::milliseconds m_before_manual_start_callback_sleep_duration{
        std::chrono::milliseconds::zero()
    };
#endif // LOON_TEST

private:
    bool internal_started();
    void internal_idle(bool state);
    void internal_reset_idling();
    void internal_unregister_content();
    std::string internal_path_prefix() const;
    loon::ContentInfo internal_modified_content_info(
        loon::ContentInfo const& info) const;

    std::mutex m_mutex;

    std::shared_ptr<IClient> m_client{};
    size_t m_index{ size_t(-1) };
    std::string m_path_prefix{};

    std::atomic<bool> m_started{ false };
    std::atomic<bool> m_idling{ false };
    std::atomic<bool> m_on_ready_called{ false };
    // This does not necessarily contain handles that are still registered.
    // These might have been unregistered by the wrapped client without notice.
    std::unordered_set<std::shared_ptr<loon::ContentHandle>> m_registered{};
};

template <typename R>
class IReferenceErasable
{
public:
    /**
     * @brief Erases all references for the given reference value.
     *
     * After this method returns it is guaranted that the class
     * holds no references to the passed reference anymore.
     * Returns whether any references were removed.
     *
     * @param ref The reference to erase.
     * @returns Whether any references were removed.
     */
    virtual bool erase_references(R const& ref) = 0;
};

class SharedReferenceCounter
    : public IReferenceErasable<std::shared_ptr<IClient>>
{
public:
    /**
     * @brief Adds a reference count for the given client.
     *
     * Returns a unique number/index for this reference count
     * that is unique from all other reference counts.
     * Counting starts at 0 and is reset when all clients were removed.
     *
     * This method is atomic and thread-safe.
     *
     * @param client The client to add a reference count for.
     * @returns Returns a unique reference count index.
     */
    size_t add(std::shared_ptr<IClient> client);

    /**
     * @brief Removes a reference count for the given client.
     *
     * Returns the number of references before removing this reference.
     * The remaining reference count after calling this method is the
     * returned value minus one.
     *
     * The required parameter causes an assertion error when there
     * are zero references for the given client.
     * This method is atomic and thread-safe.
     *
     * @param client The client to remove a reference count for.
     * @param required Whether there needs to be one or more references.
     * @returns The number of referenced clients after removing this reference.
     */
    size_t remove(std::shared_ptr<IClient> client, bool required = true);

    /**
     * @brief Returns the number of reference counts for the given client.
     *
     * Returns 0 if there are no reference counts for this client.
     *
     * The required parameter causes an assertion error when there
     * are zero references for the given client.
     * This method is atomic and thread-safe.
     *
     * @param client The client for which to return the reference counts.
     * @param required Whether there needs to be one or more references.
     * @returns The number of reference counts for this client.
     */
    size_t count(std::shared_ptr<IClient> client, bool required = false);

    bool erase_references(std::shared_ptr<IClient> const& ref) override;

private:
    struct ReferenceCounter
    {
        size_t references{ 0 };
        size_t next_index{ 0 };
    };

    std::mutex m_mutex;

    // It's okay to keep a shared_ptr reference to the client here, even though
    // it increases the pointer's internal reference count, since it will be
    // removed here when no shared client instances exist anymore.
    std::unordered_map<std::shared_ptr<IClient>, ReferenceCounter> m_refs;
};

template <typename F>
class CallbackMap
{
public:
    /**
     * @brief Sets the callback for a particular key.
     *
     * Unsets the callback for the key when a null pointer is passed.
     * This method is thread-safe.
     *
     * @param key The key to which this callback is assigned.
     * @param callback The callback.
     */
    void set(size_t key, std::function<F> callback)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_callbacks.erase(key);
        if (callback != nullptr) {
            m_callbacks.insert({ key, callback });
        }
    }

    /**
     * @brief Returns the callback for a particular key.
     *
     * Returns a null pointer when no callback exists for the key.
     *
     * @param key
     * @returns
     */
    std::function<F> get(size_t key)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_callbacks.find(key);
        if (it != m_callbacks.end()) {
            return it->second;
        }
        return nullptr;
    }

    /**
     * @brief Executes all registered callbacks.
     *
     * Callbacks are executed ordered by their key.
     * This method is thread-safe.
     */
    void execute()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (auto const& [_, callback] : m_callbacks) {
            assert(callback != nullptr);
            callback();
        }
    }

private:
    std::mutex m_mutex;
    std::map<size_t, std::function<F>> m_callbacks;
};

template <typename F>
class ClientCallbackMap : public IReferenceErasable<std::shared_ptr<IClient>>
{
public:
    /**
     * @brief Gets the callback map for a particular client.
     *
     * If the client does not exist, the callback map is created
     * and then returned on consecutive calls to this method.
     * This method is atomic and thread-safe.
     *
     * @param client The client for which to return the callback map.
     * @param insert When true, inserts a callback map when none exists.
     * @returns The callback map for the given client.
     */
    std::shared_ptr<CallbackMap<F>> get(
        std::shared_ptr<IClient> client, bool insert = true)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto contains = m_callbacks.find(client) != m_callbacks.end();
        if (!contains && !insert) {
            return nullptr;
        }
        if (!contains && insert) {
            m_callbacks.insert({ client, std::make_shared<CallbackMap<F>>() });
        }
        assert(m_callbacks.find(client) != m_callbacks.end());
        auto result = m_callbacks.at(client);
        assert(result != nullptr);
        return result;
    }

    /**
     * @brief Calls the registered callback for the given client and index.
     *
     * Does nothing when there is no callback for the client and index.
     *
     * @param client The client for which to retrieve the callback.
     * @param index The index for which to retrieve the callback.
     */
    void call(std::shared_ptr<IClient> client, size_t index)
    {
        auto map = get(client, false);
        if (map != nullptr) {
            auto callback = map->get(index);
            if (callback) {
                callback();
            }
        }
    }

    /**
     * @brief Erases the callback map for a particular client.
     *
     * This method is atomic and thread-safe.
     *
     * @param client The client for which to erase the callback map.
     * @returns Whether the client had a callback map and it was removed.
     */
    bool erase(std::shared_ptr<IClient> client)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_callbacks.erase(client) == 1;
    }

    inline bool erase_references(std::shared_ptr<IClient> const& ref) override
    {
        return erase(ref);
    }

private:
    std::mutex m_mutex;
    std::unordered_map<std::shared_ptr<IClient>,
        std::shared_ptr<CallbackMap<F>>>
        m_callbacks;
};

struct SharedClientState : public IReferenceErasable<std::shared_ptr<IClient>>
{
private:
    SharedReferenceCounter refs;

public:
    SharedReferenceCounter started;
    SharedReferenceCounter idling;
    ClientCallbackMap<void()> on_ready;
    ClientCallbackMap<void()> on_disconnect;
    ClientCallbackMap<void()> on_failed;

    SharedClientState();

    /**
     * @brief Adds a client reference.
     *
     * Sets callbacks for the client so that any shared client callbacks
     * that are registered for this client are properly called.
     * Returns a unique index for the client reference.
     *
     * @see SharedReferenceCounter::add
     *
     * @param client The client to add a reference for.
     * @returns A unique index for the client reference.
     */
    size_t add(std::shared_ptr<IClient> client);

    /**
     * @brief Removes a reference for the given client.
     *
     * Triggers an assertion error when there is no reference for the client.
     *
     * @param client The client for which to remove a reference.
     */
    void remove(std::shared_ptr<IClient> client);

    bool erase_references(std::shared_ptr<IClient> const& ref) override;

private:
    template <typename T>
    inline void init(T& value_ref)
    {
        static_assert(
            std::is_base_of_v<IReferenceErasable<std::shared_ptr<IClient>>, T>);
        m_erasers.push_back(&value_ref);
    }

    std::vector<IReferenceErasable<std::shared_ptr<IClient>>*> m_erasers;
};

} // namespace loon
