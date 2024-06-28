#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

#include "loon/loon.h"
#include "loon/messages.pb.h"

namespace loon
{
class RequestHandle
{
public:
    /**
     * Creates a request handle for a given content source.
     *
     * @param source The source of the content.
     * @param info The content information.
     * @param send_function A function that should be called
     * to send response client messages to the websocket peer.
     */
    RequestHandle(loon::ContentInfo const& info,
        std::shared_ptr<loon::ContentSource> source, Hello const& hello,
        std::function<bool(ClientMessage const&)> send_func);

    /**
     * Forwards the given request to the serve thread
     * and sends a response to the websocket peer in the background.
     * Concurrent requests are synchronized.
     *
     * @param request The request to serve.
     */
    void serve_request(Request const& request);

    /**
     * Cancels a request with the given ID that is currently being served.
     * Removes the request from the internal list of pending requests,
     * if there is no request being actively served with that ID
     * or if the currently served request has a different ID.
     *
     * @param request_id The ID of the request that should be canceled.
     */
    void cancel_request(uint64_t request_id);

    /**
     * Spawns a new serve thread for this request handle.
     * @throws std::runtime_error if the thread is already spawned
     * or if the request handle has already been destroyed.
     */
    void spawn_serve_thread();

    /**
     * Closes the request handle and its associated serve thread.
     */
    void destroy();

private:
    void serve();

    using request_id_t = uint64_t;

    /**
     * Sends a serving response message with low priority.
     * If a request is being canceled while this message is sent,
     * the thread that called cancel_request() and holds the data lock
     * will be guaranteed execute after the message has been sent.
     * It is therefore guaranteed that any cancellation is applied
     * directly after this message is sent,
     * such that ongoing requests can be cancelled as early as possible.
     *
     * @param message The response client message to send.
     * @returns The return value of the send function.
     * If this value is false, the serve function should terminate.
     */
    bool send_response_message(ClientMessage const& message);

    Hello hello;
    loon::ContentInfo info;
    std::shared_ptr<loon::ContentSource> source;
    std::function<bool(ClientMessage const&)> send_message;

    // The remaining fields are all default-initialized.

    // Using three mutex variables for a thread with priority.
    // Reference: https://stackoverflow.com/a/11673600/6748004
    // When a thread calls cancel_request(), it should always have priority
    // in changing the state of the request handle,
    // so that after sending a message, we always immediately know
    // whether it has been cancelled from the outside or not.

    std::mutex mutex_data{}; // Mutex for data fields.
    std::mutex mutex_next{}; // Mutex for next-to-access threads.
    std::mutex mutex_low{};  // Mutex for low-priority access threads.

    std::condition_variable cv_incoming_request{};
    std::deque<Request> pending_requests{};
    std::optional<request_id_t> handling_request_id{};
    bool cancel_handling_request{ false };
    bool dirty{ false };
    bool stop{ false };
};
} // namespace loon
