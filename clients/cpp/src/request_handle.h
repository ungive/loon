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
     * @param info The content information.
     * @param source The source of the content.
     * @param hello The hello message that was received from the server.
     * @param send_func A function that should be called
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
     * @brief Exits the serve loop gracefully and blocks until it exited.
     *
     * Cancels any ongoing requests in the process,
     * such that the connection remains in a valid state.
     */
    void exit_gracefully();

    /**
     * Closes the request handle and its associated serve thread.
     * Meant to be used if the connection is closed
     * and the request handler should exit forcefully.
     * If the connection should remain in a valid state,
     * use exit_gracefully() instead.
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

    // Using three mutex variables for threads with priority.
    // More information at the top of request_handle.cpp.
    std::mutex mutex{};      // Mutex for data fields.
    std::mutex mutex_next{}; // Mutex for next-to-access threads.
    std::mutex mutex_low{};  // Mutex for low-priority access threads.

    std::condition_variable cv_incoming_request{};
    std::deque<Request> pending_requests{};
    std::optional<request_id_t> handling_request_id{};
    bool cancel_handling_request{ false };
    bool dirty{ false };
    bool stop{ false };
    bool done{ false };
    std::condition_variable cv_done{};
};
} // namespace loon
