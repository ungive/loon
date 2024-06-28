#include "request_handle.h"

#include <cassert>

using namespace loon;

RequestHandle::RequestHandle(ContentInfo const& info,
    std::shared_ptr<ContentSource> source, Hello const& hello,
    std::function<bool(ClientMessage const&)> send_func)
    : hello{ hello }, info{ info }, source{ source }, send_message{ send_func }
{
}

void RequestHandle::serve()
{
    std::unique_lock<std::mutex> lock(mutex_data);

    // Unlocks the data lock, sends the message with low priority,
    // so that cancellation while the message is being sent can go through,
    // locks the lock and returns whether the message was successfully sent.
    // If false is returned, the serve function should immediately terminate.
    auto send = [this, &lock](ClientMessage const& message) {
        lock.unlock();
        auto result = send_response_message(message);
        lock.lock();
        return result;
    };

    // Serve requests until the request handler is stopped.
    while (!stop) {
        if (cancel_handling_request && !handling_request_id.has_value()) {
            assert(false && "no handled request ID to cancel");
        }
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
        if (!send(header_message) || stop)
            return;
        if (cancel_handling_request)
            continue;

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
            if (!send(chunk_message) || stop)
                return;
            if (cancel_handling_request)
                continue;
        }
        if (!stream.good()) {
            // The response is complete, we do not need to cancel anymore.
            cancel_handling_request = false;
        }
        if (cancel_handling_request)
            continue;
    }
}

bool RequestHandle::send_response_message(ClientMessage const& message)
{
    // low-priority: lock L, lock N, lock M, unlock N, ..., unlock M, unlock L
    const std::lock_guard<std::mutex> lock_low(mutex_low);
    mutex_next.lock();
    const std::lock_guard<std::mutex> lock_data(mutex_data);
    mutex_next.unlock();
    return send_message(message);
}

void RequestHandle::serve_request(Request const& request)
{
    const std::lock_guard<std::mutex> lock(mutex_data);
    pending_requests.push_back(request);
    cv_incoming_request.notify_one();
}

void RequestHandle::cancel_request(uint64_t request_id)
{
    // high-priority: lock N, lock M, unlock N, ..., unlock M
    mutex_next.lock();
    const std::lock_guard<std::mutex> lock(mutex_data);
    mutex_next.unlock();

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
    const std::lock_guard<std::mutex> lock(mutex_data);
    if (dirty) {
        throw std::runtime_error("serve thread has already been spawned");
    }
    if (stop) {
        throw std::runtime_error("request handle is stopped");
    }
    std::thread(&RequestHandle::serve, this).detach();
    dirty = true;
}

void RequestHandle::destroy()
{
    // high-priority: lock N, lock M, unlock N, ..., unlock M
    mutex_next.lock();
    const std::lock_guard<std::mutex> lock(mutex_data);
    mutex_next.unlock();

    if (stop) {
        throw std::runtime_error("request handle is already destroyed");
    }
    stop = true;
}
