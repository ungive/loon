#include "loon/shared_client.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

#ifdef LOON_TEST
#include <thread>
#endif

#include "shared_client.h"

using namespace loon;
using namespace std::chrono_literals;

loon::SharedClient::SharedClient(std::shared_ptr<IClient> client)
    : m_impl{ std::make_unique<SharedClientImpl>(client) }
{
}

loon::SharedClient::SharedClient(SharedClient&& other)
    : m_impl{ std::move(other.m_impl) }
{
}

static SharedClientState g_state;

loon::SharedClientImpl::SharedClientImpl(std::shared_ptr<IClient> client)
    : m_client{ client }
{
    // Ensure the client is not a shared client instance.
    auto casted = std::dynamic_pointer_cast<ISharedClient>(m_client);
    if (casted != nullptr) {
        throw std::invalid_argument(
            "the wrapped client may not be a shared client");
    }

    // Add a global reference for this client and set the index and path prefix.
    m_index = g_state.add(m_client);
    m_path_prefix = internal_path_prefix();

    // Default callback which sets the ready flag.
    g_state.on_ready.get(m_client)->set(m_index, [this] {
        if (m_started.load()) {
            m_on_ready_called.store(true);
        }
    });

    // Default callback which resets the ready flag.
    g_state.on_disconnect.get(m_client)->set(m_index, [this] {
        m_on_ready_called.store(false);
    });
}

loon::SharedClientImpl::~SharedClientImpl()
{
    // Ensure the underlying client is stopped, once all shared clients
    // are destructed. This also unregisters all content of this shared client.
    stop();

    // Remove one client reference from the global shared client state.
    g_state.remove(m_client);
}

size_t loon::SharedClientImpl::index() const
{
    assert(m_index != size_t(-1));
    return m_index;
}

std::string const& loon::SharedClientImpl::path_prefix() const
{
    assert(!m_path_prefix.empty());
    return m_path_prefix;
}

inline std::string loon::SharedClientImpl::internal_path_prefix() const
{
    assert(m_index != size_t(-1));
    return std::to_string(m_index) + "/";
}

void loon::SharedClientImpl::start()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    internal_reset_idling();
    if (m_started.load()) {
        if (!m_client->started()) {
            // If the client is not started, it needs to be started again.
            // Everything else is already set the way it needs to be.
            m_client->start(); // delegate
        }
        return;
    }

    // Remember whether the client has already been started before.
    bool was_started = m_client->started();

    // Start the client.
    // Note: Executing this is okay, if the client is already started.
    g_state.started.add(m_client);
    m_client->start(); // delegate
    m_started.store(true);
    m_on_ready_called.store(false);

#ifdef LOON_TEST
    std::this_thread::sleep_for(m_before_manual_start_callback_sleep_duration);
#endif

    // If the client was already started before having called start() now,
    // then ensure that the ready callback is called when the client is ready.
    // Note: m_started must have been set to true before calling the callback,
    // otherwise the callback will not properly execute, see on_ready() below.
    if (was_started) {
        try {
            // Call the callback for this shared client, if there is any and
            // if the client is already ready (no waiting done).
            if (!m_client->wait_until_ready(0s)) {
                g_state.on_ready.call(m_client, m_index);
            }
        }
        catch (loon::TimeoutException const&) {
        }
        catch (loon::ClientNotConnectedException const&) {
        }
        catch (loon::ClientNotStartedException const&) {
            assert(false);
        }
    }
}

void loon::SharedClientImpl::stop()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    internal_reset_idling();
    internal_unregister_content();
    if (!m_started.load()) {
        return;
    }
    // Note: Executing this is okay, if the client is not actually started.
    auto previous_count = g_state.started.remove(m_client);
    // Stop the client when this was the last started shared client.
    if (previous_count == 1) {
        m_client->stop(); // delegate
    } else {
        // The client is not actually stopped, but the disconnect callback
        // that is registered for this shared client still needs to be called.
        g_state.on_disconnect.call(m_client, m_index);
    }
    m_started.store(false);
}

bool loon::SharedClientImpl::started()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return internal_started();
}

bool loon::SharedClientImpl::internal_started()
{
    return m_started.load() && m_client->started(); // delegate
}

void loon::SharedClientImpl::internal_reset_idling()
{
    if (m_idling.load()) {
        auto previous_count = g_state.idling.remove(m_client);
        assert(previous_count > 0);
        m_idling.store(false);
    }
}

void loon::SharedClientImpl::internal_unregister_content()
{
    // It's okay to unregister content handles that are not registered anymore.
    // No exceptions will be thrown in that case.
    for (auto it = m_registered.begin(); it != m_registered.end();) {
        m_client->unregister_content(*it);
        it = m_registered.erase(it);
    }
    assert(m_registered.empty());
}

void loon::SharedClientImpl::idle()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!internal_started()) {
        return;
    }
    if (m_idling.load()) {
        return;
    }
    g_state.idling.add(m_client); // this shared client is idling
    auto started = g_state.started.count(m_client, true);
    auto idling = g_state.idling.count(m_client, true);
    assert(idling <= started);
    bool all_idling = started == idling;
    // Put the client into idle when all shared clients are idling.
    if (all_idling) {
        m_client->idle(); // delegate
    }
    m_idling.store(true);
}

bool loon::SharedClientImpl::wait_until_ready()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started.load()) {
        throw loon::ClientNotStartedException(
            "the shared client must be started");
    }
    return m_client->wait_until_ready(); // delegate
}

bool loon::SharedClientImpl::wait_until_ready(std::chrono::milliseconds timeout)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started.load()) {
        throw loon::ClientNotStartedException(
            "the shared client must be started");
    }
    return m_client->wait_until_ready(timeout); // delegate
}

void loon::SharedClientImpl::on_ready(std::function<void()> callback)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    g_state.on_ready.get(m_client)->set(m_index, [this, callback] {
        // Note: Do not lock the shared client's mutex within the callback.
        if (m_started.load()) {
            // Only call the callback when the client is actually started.
            if (!m_on_ready_called.exchange(true)) {
                // And only call it when it wasn't already called before.
                callback();
            }
        }
    });
}

void loon::SharedClientImpl::on_disconnect(std::function<void()> callback)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    g_state.on_disconnect.get(m_client)->set(m_index, [this, callback] {
        // Note: Do not lock the shared client's mutex within the callback.
        if (m_on_ready_called.exchange(false)) {
            // Only call the callback when the client was ready before.
            // The client is also not ready anymore.
            callback();
        }
    });
}

void loon::SharedClientImpl::on_failed(std::function<void()> callback)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    g_state.on_failed.get(m_client)->set(m_index, callback);
}

loon::ContentInfo loon::SharedClientImpl::internal_modified_content_info(
    loon::ContentInfo const& info) const
{
    auto copy = info;
    // Prepend the path prefix before the desired path for disambiguation.
    copy.path = path_prefix() + copy.path;
    return copy;
}

std::shared_ptr<ContentHandle> loon::SharedClientImpl::register_content(
    std::shared_ptr<loon::ContentSource> source, loon::ContentInfo const& info,
    std::chrono::milliseconds timeout)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto handle = m_client->register_content(
        source, internal_modified_content_info(info), timeout); // delegate
    m_registered.insert(handle);
    return handle;
}

std::shared_ptr<ContentHandle> loon::SharedClientImpl::register_content(
    std::shared_ptr<loon::ContentSource> source, loon::ContentInfo const& info)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto handle = m_client->register_content(
        source, internal_modified_content_info(info)); // delegate
    m_registered.insert(handle);
    return handle;
}

void loon::SharedClientImpl::unregister_content(
    std::shared_ptr<ContentHandle> handle)
{
    if (handle == nullptr) {
        throw MalformedContentException("the content handle cannot be null");
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_registered.find(handle) == m_registered.end()) {
        throw loon::MalformedContentException(
            "the content handle is not registered with this shared client");
    }
    m_client->unregister_content(handle); // delegate
    m_registered.erase(handle);
}

std::vector<std::shared_ptr<ContentHandle>> loon::SharedClientImpl::content()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::shared_ptr<ContentHandle>> result;
    auto content = m_client->content();
    result.reserve(content.size());
    size_t still_registered{ 0 };
    for (auto const& handle : content) {
        if (m_registered.find(handle) != m_registered.end()) {
            result.push_back(handle);
            still_registered++;
        }
    }
    if (still_registered < m_registered.size()) {
        // Erase stray content handles that are not registered anymore.
        for (auto it = m_registered.begin(); it != m_registered.end();) {
            if (!m_client->is_registered(*it)) {
                it = m_registered.erase(it);
                continue;
            }
            it++;
        }
    }
    return result;
}

bool loon::SharedClientImpl::is_registered(
    std::shared_ptr<ContentHandle> handle)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto registered = m_client->is_registered(handle); // delegate
    auto it = m_registered.find(handle);
    auto owned = it != m_registered.end();
    if (!registered) {
        if (owned) {
            // Erase stray content handle that is not registered anymore.
            m_registered.erase(it);
        }
    }
    // Must both be registered with the underlying client and
    // the content handle must be owned by this shared client.
    return registered && owned;
}

// Helper class implementations

size_t loon::SharedReferenceCounter::add(std::shared_ptr<IClient> client)
{
    if (client == nullptr)
        throw std::invalid_argument("client cannot be a null pointer");
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_refs.find(client);
    if (it == m_refs.end()) {
        auto result = m_refs.insert({ client, ReferenceCounter{} });
        assert(result.second);
        it = result.first;
        assert(it != m_refs.end());
    }
    auto& counter = it->second;
    counter.references += 1;
    auto index = counter.next_index;
    assert(index != std::numeric_limits<decltype(index)>::max());
    counter.next_index += 1;
    return index;
}

size_t loon::SharedReferenceCounter::remove(
    std::shared_ptr<IClient> client, bool required)
{
    if (client == nullptr)
        throw std::invalid_argument("client cannot be a null pointer");
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_refs.find(client);
    if (it == m_refs.end()) {
        assert(!required);
        return 0;
    }
    auto& counter = it->second;
    if (counter.references == 0) {
        assert(false);
        m_refs.erase(client);
        return 0;
    }
    auto old_value = counter.references;
    counter.references -= 1;
    if (counter.references == 0) {
        assert(old_value == 1);
        m_refs.erase(client);
        return old_value;
    }
    assert(old_value > 1);
    return old_value;
}

size_t loon::SharedReferenceCounter::count(
    std::shared_ptr<IClient> client, bool required)
{
    if (client == nullptr)
        throw std::invalid_argument("client cannot be a null pointer");
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_refs.find(client);
    if (it == m_refs.end()) {
        assert(!required);
        return 0;
    }
    auto& counter = it->second;
    assert(counter.references != 0);
    return counter.references;
}

bool loon::SharedReferenceCounter::erase_references(
    std::shared_ptr<IClient> const& ref)
{
    if (ref == nullptr)
        throw std::invalid_argument("client cannot be a null pointer");
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_refs.find(ref);
    if (it == m_refs.end()) {
        return false;
    }
    auto& counter = it->second;
    if (counter.references == 0) {
        assert(false);
        m_refs.erase(ref);
        return false;
    }
    m_refs.erase(ref);
    return true;
}

loon::SharedClientState::SharedClientState()
{
    m_erasers.reserve(6);
    init(refs);
    init(started);
    init(idling);
    init(on_ready);
    init(on_disconnect);
    init(on_failed);
    assert(!m_erasers.empty());
}

size_t loon::SharedClientState::add(std::shared_ptr<IClient> client)
{
    auto index = refs.add(client);

    // Ensure the callbacks are set for the wrapped client.
    client->on_ready(
        std::bind(&CallbackMap<void()>::execute, on_ready.get(client)));
    client->on_disconnect(
        std::bind(&CallbackMap<void()>::execute, on_disconnect.get(client)));
    client->on_failed(
        std::bind(&CallbackMap<void()>::execute, on_failed.get(client)));

    return index;
}

void loon::SharedClientState::remove(std::shared_ptr<IClient> client)
{
    refs.remove(client);

    // There should be no more started or idling clients than
    // there are number of clients after removing the passed one.
    assert(started.count(client) <= refs.count(client));
    assert(idling.count(client) <= refs.count(client));

    if (refs.count(client) == 0) {
        // Unset all callbacks for the given client, when there are
        // no more shared clients that make use of the callbacks.
        client->on_ready(nullptr);
        client->on_disconnect(nullptr);
        client->on_failed(nullptr);

        // Likewise erase all callbacks for the given client.
        on_ready.erase(client);
        on_disconnect.erase(client);
        on_failed.erase(client);

        // The caller should have already have made sure that
        // there are no more started or idling shared clients anymore.
        assert(started.count(client) == 0);
        assert(idling.count(client) == 0);

        // For good measure, erase references from every state member
        // and trigger an assertion error when there are references left.
        if (erase_references(client)) {
            assert(false);
        }
    }
}

bool loon::SharedClientState::erase_references(
    std::shared_ptr<IClient> const& ref)
{
    auto removed = false;
    for (auto& eraser : m_erasers) {
        if (eraser->erase_references(ref)) {
            removed = true;
        }
    }
    return removed;
}
