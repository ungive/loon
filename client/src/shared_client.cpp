#include "loon/shared_client.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <mutex>
#include <stdexcept>

#include "shared_client.h"

using namespace loon;

loon::SharedClient::SharedClient(std::shared_ptr<IClient> client)
    : m_impl{ std::make_unique<SharedClientImpl>(client) }
{
}

static std::mutex g_mutex;
static SharedReferenceCounter g_started;
static SharedReferenceCounter g_idling;

loon::SharedClientImpl::SharedClientImpl(std::shared_ptr<IClient> client)
    : m_client{ client }
{
    auto casted = std::dynamic_pointer_cast<ISharedClient>(m_client);
    if (casted != nullptr) {
        throw std::invalid_argument(
            "the wrapped client may not be a shared client");
    }
}

loon::SharedClientImpl::~SharedClientImpl()
{
    // Ensure the underlying client is stopped,
    // once all shared clients are destructed.
    stop();
}

size_t loon::SharedClientImpl::index() const { return m_index; }

std::string const& loon::SharedClientImpl::path_prefix() const
{
    static std::string prefix;
    if (prefix.empty()) {
        prefix = std::to_string(index()) + "/";
        assert(!prefix.empty());
    }
    return prefix;
}

void loon::SharedClientImpl::start()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    internal_reset_idling();
    if (m_started) {
        if (!m_client->started()) {
            // If the client is not started, it needs to be started again.
            // Everything else is already set the way it needs to be.
            m_client->start(); // delegate
        }
        return;
    }
    // Note: Executing this is okay, if the client is already started.
    {
        const std::lock_guard<std::mutex> global_lock(g_mutex);
        g_started.add(m_client);
    }
    m_client->start(); // delegate
    m_started = true;
}

void loon::SharedClientImpl::stop()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    internal_reset_idling();
    if (!m_started) {
        return;
    }
    // Note: Executing this is okay, if the client is not actually started.
    bool last_started = false;
    {
        const std::lock_guard<std::mutex> global_lock(g_mutex);
        last_started = g_started.single(m_client);
        g_started.remove(m_client);
    }
    // Stop the client when this was the last started shared client.
    if (last_started) {
        m_client->stop(); // delegate
    }
    m_started = false;
}

bool loon::SharedClientImpl::started()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return internal_started();
}

bool loon::SharedClientImpl::internal_started()
{
    return m_started && m_client->started(); // delegate
}

void loon::SharedClientImpl::internal_reset_idling()
{
    if (m_idling) {
        const std::lock_guard<std::mutex> global_lock(g_mutex);
        assert(g_idling.count(m_client) > 0);
        g_idling.remove(m_client);
        m_idling = false;
    }
}

void loon::SharedClientImpl::idle()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!internal_started()) {
        return;
    }
    if (m_idling) {
        return;
    }
    bool all_idling = false;
    {
        const std::lock_guard<std::mutex> global_lock(g_mutex);
        g_idling.add(m_client); // this shared client is idling
        auto started = g_started.count(m_client, true);
        auto idling = g_idling.count(m_client, true);
        assert(idling <= started);
        all_idling = started == idling;
    }
    // Put the client into idle when all shared clients are idling.
    if (all_idling) {
        m_client->idle(); // delegate
    }
    m_idling = true;
}

bool loon::SharedClientImpl::wait_until_ready()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started) {
        throw loon::ClientNotStartedException(
            "the shared client must be started");
    }
    return m_client->wait_until_ready(); // delegate
}

bool loon::SharedClientImpl::wait_until_ready(std::chrono::milliseconds timeout)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started) {
        throw loon::ClientNotStartedException(
            "the shared client must be started");
    }
    return m_client->wait_until_ready(timeout); // delegate
}

void loon::SharedClientImpl::on_ready(std::function<void()> callback)
{
    // TODO
}

void loon::SharedClientImpl::on_disconnect(std::function<void()> callback)
{
    // TODO
}

void loon::SharedClientImpl::on_failed(std::function<void()> callback)
{
    // TODO
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
    result.reserve(m_registered.size());
    std::transform(m_registered.begin(), m_registered.end(),
        std::back_inserter(result),
        [](decltype(m_registered)::value_type const& handle) {
            return handle;
        });
#ifndef NDEBUG
    size_t contained = 0;
    for (auto const& handle : m_client->content())
        if (m_registered.find(handle) != m_registered.end())
            contained += 1;
    assert(result.size() == contained);
#endif
    return result;
}

bool loon::SharedClientImpl::is_registered(
    std::shared_ptr<ContentHandle> handle)
{
    if (handle == nullptr) {
        throw MalformedContentException("the content handle cannot be null");
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto result = m_registered.find(handle) != m_registered.end();
    if (!m_client->is_registered(handle)) {
        assert(!result);
    }
    return result;
}

// Shared reference counter implementation

size_t loon::SharedReferenceCounter::add(std::shared_ptr<IClient> client)
{
    if (client == nullptr)
        throw std::invalid_argument("client cannot be a null pointer");
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

void loon::SharedReferenceCounter::remove(std::shared_ptr<IClient> client)
{
    if (client == nullptr)
        throw std::invalid_argument("client cannot be a null pointer");
    auto it = m_refs.find(client);
    if (it == m_refs.end()) {
        assert(false);
        return;
    }
    auto& counter = it->second;
    if (counter.references == 0) {
        assert(false);
        m_refs.erase(client);
        return;
    }
    counter.references -= 1;
    if (counter.references == 0) {
        m_refs.erase(client);
        return;
    }
}

size_t loon::SharedReferenceCounter::count(
    std::shared_ptr<IClient> client, bool required)
{
    if (client == nullptr)
        throw std::invalid_argument("client cannot be a null pointer");
    auto it = m_refs.find(client);
    if (it == m_refs.end()) {
        assert(!required);
        return 0;
    }
    auto& counter = it->second;
    assert(counter.references != 0);
    return counter.references;
}
