#include "request_handler.h"

#include <cassert>

using namespace loon;

// Macros for locking with low and high priority threads.
// Reference: https://stackoverflow.com/a/11673600/6748004

// Locks the request handle for the lifetime of the current scope
// with low priority, such that high priority threads run next.
// order: lock L, lock N, lock M, unlock N, ..., unlock M, unlock L
#define mutex_lock_low_priority()                            \
    const std::lock_guard<std::mutex> lock_low(m_mutex_low); \
    m_mutex_next.lock();                                     \
    const std::lock_guard<std::mutex> lock_data(m_mutex);    \
    m_mutex_next.unlock();

// Locks the request handle for the lifetime of the current scope
// with high priority, such that this thread runs next,
// before other threads that also acquired the data lock.
// order: lock N, lock M, unlock N, ..., unlock M
#define mutex_lock_high_priority()                        \
    m_mutex_next.lock();                                  \
    const std::lock_guard<std::mutex> lock_data(m_mutex); \
    m_mutex_next.unlock();

RequestHandler::RequestHandler(ContentInfo const& info,
    std::shared_ptr<ContentSource> source,
    Hello const& hello,
    Options options,
    std::function<bool(ClientMessage const&)> send_func)
    : m_hello{ hello }, m_info{ info }, m_source{ source },
      m_options{ options }, m_send_message{ send_func }
{
    // No need to validate the options again here,
    // since the values are already validated by the client implementation.
}

void RequestHandler::serve()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    std::shared_ptr<void> defer(nullptr, std::bind([this] {
        // Notify that the serve loop has exited once the function returns.
        // Note that "defer" needs to be constructed after the lock,
        // so that it is destructed before the lock
        // and therefore "done" is changed while the lock is still being held.
        m_done = true;
        m_cv_done.notify_all();
    }));

    // Unlocks the lock, sends the message with low priority,
    // so that cancellation while the message is being sent can go through,
    // locks the lock and returns whether the message was successfully sent.
    // If false is returned, the serve function should immediately terminate.
    auto send = [this](decltype(lock)& lock, ClientMessage const& message) {
        lock.unlock();
        auto result = send_response_message(message);
        lock.lock();
        return result;
    };

    // The loop condition needs to be true,
    // since the request cancellation is always done after an iteration.
    while (true) {
        if (m_cancel_handling_request && m_handling_request_id.has_value()) {
            // Close the response if the currently handled request
            // was canceled during the previous loop iteration.
            ClientMessage message;
            auto close_response = message.mutable_close_response();
            close_response->set_request_id(m_handling_request_id.value());
            if (!m_send_message(message)) {
                return;
            }
        }

        // Reset any request handling state.
        m_handling_request_id = std::nullopt;
        m_cancel_handling_request = false;

        // Wait for incoming or pending requests
        // or until the request handler is stopped.
        m_cv_incoming_request.wait(lock, [this] {
            return m_stop || m_pending_requests.size() > 0;
        });
        if (m_stop) {
            return;
        }

        // Read the next request from the queue.
        auto serve_request = m_pending_requests.front();
        m_pending_requests.pop_front();
        m_handling_request_id = serve_request.request.id();

        // Send the response header.
        ClientMessage header_message;
        auto header = header_message.mutable_content_header();
        header->set_request_id(serve_request.request.id());
        header->set_content_type(m_source->content_type());
        header->set_content_size(m_source->size());
        if (m_info.max_cache_duration.has_value()) {
            header->set_max_cache_duration(m_info.max_cache_duration.value());
        }
        if (m_info.attachment_filename.has_value()) {
            header->set_filename(m_info.attachment_filename.value());
        }
        if (!send(lock, header_message))
            return;
        if (m_cancel_handling_request)
            continue;
        if (m_stop)
            return;

        // Send the chunks.
        uint64_t sequence = 0;
        auto& stream = m_source->data();
        auto const chunk_size = m_hello.constraints().chunk_size();
        std::vector<char> buffer(chunk_size);
        while (stream.good()) {
            if (m_options.chunk_sleep > std::chrono::milliseconds::zero()) {
                std::this_thread::sleep_for(m_options.chunk_sleep);
            }
            stream.read(buffer.data(), buffer.size());
            std::streamsize n = stream.gcount();
            if (n <= 0) {
                break;
            }
            ClientMessage chunk_message;
            auto chunk = chunk_message.mutable_content_chunk();
            chunk->set_request_id(serve_request.request.id());
            chunk->set_sequence(sequence++);
            chunk->set_data(buffer.data(), n);
            if (!send(lock, chunk_message))
                return;
            if (m_cancel_handling_request)
                break;
            if (m_stop)
                return;
        }
        if (!m_cancel_handling_request) {
            // The request was fully sent, without being cancelled.
            lock.unlock();
            serve_request.callback();
            lock.lock();
        }
        if (!stream.good()) {
            // The response is complete, we do not need to cancel anymore.
            m_cancel_handling_request = false;
        }
    }
}

inline bool RequestHandler::send_response_message(ClientMessage const& message)
{
    mutex_lock_low_priority();
    return m_send_message(message);
}

void RequestHandler::serve_request(
    Request const& request, std::function<void()> callback)
{
    using namespace std::chrono_literals;

    // Neither high, nor low priority.
    const std::lock_guard<std::mutex> lock(m_mutex);

    // Check if the previous response should have been cached by the server.
    auto now = std::chrono::system_clock::now();
    if (m_options.min_cache_duration.has_value() &&
        m_last_request.has_value()) {
        auto then = m_last_request.value();
        auto elapsed = now - then;
        assert(now - then >= 0ms);
        std::chrono::seconds duration{ m_options.min_cache_duration.value() };
        if (elapsed <= duration) {
            throw ResponseNotCachedException(
                "the server does not seem to cache previous responses for a "
                "sufficient amount of time");
        }
    }
    m_last_request = now;

    m_pending_requests.push_back(ServeRequest(request, callback));
    m_cv_incoming_request.notify_one();
}

void RequestHandler::cancel_request(uint64_t request_id)
{
    // Response cancellation has high priority.
    mutex_lock_high_priority();

    // This request ID is currently being handled.
    // It's the responsibility of the serve function
    // to stop sending data and close the response afterwards.
    if (m_handling_request_id.has_value() &&
        request_id == m_handling_request_id.value()) {
        m_cancel_handling_request = true;
        return;
    }

    // This request ID is not being actively handled.
    // See if a response is pending for it,
    // then remove it and close the response immediately.
    auto it = std::find_if(m_pending_requests.begin(), m_pending_requests.end(),
        [request_id](ServeRequest const& request) {
            return request.request.id() == request_id;
        });
    if (it != m_pending_requests.end()) {
        m_pending_requests.erase(it);

        // We are only sending the CloseResponse message here,
        // when we are sure that the response is not already completed,
        // since it is forbidden to send a CloseResponse message
        // for completed requests.
        ClientMessage message;
        auto close_response = message.mutable_close_response();
        close_response->set_request_id(request_id);
        if (!m_send_message(message)) {
            return;
        }
    }
}

void RequestHandler::spawn_serve_thread()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_dirty) {
        throw std::runtime_error("serve thread has already been spawned");
    }
    if (m_stop) {
        throw std::runtime_error("request handle is stopped");
    }
    std::thread(&RequestHandler::serve, this).detach();
    m_dirty = true;
}

void loon::RequestHandler::exit_gracefully()
{
    {
        // Exiting gracefully is high priority.
        mutex_lock_high_priority();

        if (m_stop || m_done) {
            throw std::runtime_error("request handle is already destroyed");
        }
        if (m_handling_request_id.has_value()) {
            // Cancel the request that is being handled before before stopping.
            m_cancel_handling_request = true;
        }
        m_stop = true;
    }
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        // Wake the serve thread up, in case it is waiting for a request.
        m_cv_incoming_request.notify_all();
        // Block until the serve loop has exited.
        m_cv_done.wait(lock, [this] {
            return m_done;
        });
    }
}

void RequestHandler::destroy()
{
    // Destroying the request handle has high priority.
    mutex_lock_high_priority();

    if (m_stop || m_done) {
        throw std::runtime_error("request handle is already destroyed");
    }
    m_stop = true;
}
