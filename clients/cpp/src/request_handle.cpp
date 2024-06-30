#include "request_handle.h"

#include <cassert>

using namespace loon;

// Macros for locking with low and high priority threads.
// Reference: https://stackoverflow.com/a/11673600/6748004

// Locks the request handle for the lifetime of the current scope
// with low priority, such that high priority threads run next.
// order: lock L, lock N, lock M, unlock N, ..., unlock M, unlock L
#define mutex_lock_low_priority()                          \
    const std::lock_guard<std::mutex> lock_low(mutex_low); \
    mutex_next.lock();                                     \
    const std::lock_guard<std::mutex> lock_data(mutex);    \
    mutex_next.unlock();

// Locks the request handle for the lifetime of the current scope
// with high priority, such that this thread runs next,
// before other threads that also acquired the data lock.
// order: lock N, lock M, unlock N, ..., unlock M
#define mutex_lock_high_priority()                      \
    mutex_next.lock();                                  \
    const std::lock_guard<std::mutex> lock_data(mutex); \
    mutex_next.unlock();

RequestHandle::RequestHandle(ContentInfo const& info,
    std::shared_ptr<ContentSource> source, Hello const& hello,
    std::function<bool(ClientMessage const&)> send_func)
    : hello{ hello }, info{ info }, source{ source }, send_message{ send_func }
{
}

void RequestHandle::serve()
{
    std::unique_lock<std::mutex> lock(mutex);

    std::shared_ptr<void> defer(nullptr, std::bind([this] {
        // Notify that the serve loop has exited once the function returns.
        // Note that "defer" needs to be constructed after the lock,
        // so that it is destructed before the lock
        // and therefore "done" is changed while the lock is still being held.
        done = true;
        cv_done.notify_all();
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
        if (cancel_handling_request && handling_request_id.has_value()) {
            // Close the response if the currently handled request
            // was canceled during the previous loop iteration.
            ClientMessage message;
            auto close_response = message.mutable_close_response();
            close_response->set_request_id(handling_request_id.value());
            if (!send_message(message)) {
                return;
            }
        }

        // Reset any request handling state.
        handling_request_id = std::nullopt;
        cancel_handling_request = false;

        // Wait for incoming or pending requests
        // or until the request handler is stopped.
        cv_incoming_request.wait(lock, [this] {
            return stop || pending_requests.size() > 0;
        });
        if (stop) {
            return;
        }

        // Read the next request from the queue.
        auto request = pending_requests.front();
        pending_requests.pop_front();
        handling_request_id = request.id();

        // Send the response header.
        ClientMessage header_message;
        auto header = header_message.mutable_content_header();
        header->set_request_id(request.id());
        header->set_content_type(source->content_type());
        header->set_content_size(source->size());
        if (info.max_cache_duration.has_value()) {
            header->set_max_cache_duration(info.max_cache_duration.value());
        }
        if (info.attachment_filename.has_value()) {
            header->set_filename(info.attachment_filename.value());
        }
        if (!send(lock, header_message))
            return;
        if (cancel_handling_request)
            continue;
        if (stop)
            return;

        // Send the chunks.
        uint64_t sequence = 0;
        auto& stream = source->data();
        auto const chunk_size = hello.constraints().chunk_size();
        std::vector<char> buffer(chunk_size);
        while (stream.good()) {
            stream.read(buffer.data(), buffer.size());
            std::streamsize n = stream.gcount();
            if (n <= 0) {
                break;
            }
            ClientMessage chunk_message;
            auto chunk = chunk_message.mutable_content_chunk();
            chunk->set_request_id(request.id());
            chunk->set_sequence(sequence++);
            chunk->set_data(buffer.data(), n);
            if (!send(lock, chunk_message))
                return;
            if (cancel_handling_request)
                break;
            if (stop)
                return;
        }
        if (!stream.good()) {
            // The response is complete, we do not need to cancel anymore.
            cancel_handling_request = false;
        }
        if (cancel_handling_request)
            continue;
    }
}

inline bool RequestHandle::send_response_message(ClientMessage const& message)
{
    mutex_lock_low_priority();
    return send_message(message);
}

void RequestHandle::serve_request(Request const& request)
{
    // Neither high, nor low priority.
    const std::lock_guard<std::mutex> lock(mutex);

    pending_requests.push_back(request);
    cv_incoming_request.notify_one();
}

void RequestHandle::cancel_request(uint64_t request_id)
{
    // Response cancellation has high priority.
    mutex_lock_high_priority();

    // This request ID is currently being handled.
    // It's the responsibility of the serve function
    // to stop sending data and close the response afterwards.
    if (handling_request_id.has_value() &&
        request_id == handling_request_id.value()) {
        cancel_handling_request = true;
        return;
    }

    // This request ID is not being actively handled.
    // See if a response is pending for it,
    // then remove it and close the response immediately.
    auto it = std::find_if(pending_requests.begin(), pending_requests.end(),
        [request_id](Request const& request) {
            return request.id() == request_id;
        });
    if (it != pending_requests.end()) {
        pending_requests.erase(it);

        // We are only sending the CloseResponse message here,
        // when we are sure that the response is not already completed,
        // since it is forbidden to send a CloseResponse message
        // for completed requests.
        ClientMessage message;
        auto close_response = message.mutable_close_response();
        close_response->set_request_id(request_id);
        if (!send_message(message)) {
            return;
        }
    }
}

void RequestHandle::spawn_serve_thread()
{
    const std::lock_guard<std::mutex> lock(mutex);
    if (dirty) {
        throw std::runtime_error("serve thread has already been spawned");
    }
    if (stop) {
        throw std::runtime_error("request handle is stopped");
    }
    std::thread(&RequestHandle::serve, this).detach();
    dirty = true;
}

void loon::RequestHandle::exit_gracefully()
{
    {
        // Exiting gracefully is high priority.
        mutex_lock_high_priority();

        if (stop || done) {
            throw std::runtime_error("request handle is already destroyed");
        }
        if (handling_request_id.has_value()) {
            // Cancel the request that is being handled before before stopping.
            cancel_handling_request = true;
        }
        stop = true;
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        // Wake the serve thread up, in case it is waiting for a request.
        cv_incoming_request.notify_all();
        // Block until the serve loop has exited.
        cv_done.wait(lock, [this] {
            return done;
        });
    }
}

void RequestHandle::destroy()
{
    // Destroying the request handle has high priority.
    mutex_lock_high_priority();

    if (stop || done) {
        throw std::runtime_error("request handle is already destroyed");
    }
    stop = true;
}
