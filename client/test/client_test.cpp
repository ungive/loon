#include "common/test.hpp"

TEST(Client, ServerServesContentWhenRegisteredWithClient)
{
    std::string path = "index.html";
    uint32_t cache_duration = 23;
    std::string filename = "page.html";
    std::string content = "<h1>It works!";
    std::string content_type = "text/html";

    auto client = create_client();
    ContentInfo info;
    info.path = path;
    info.max_cache_duration = cache_duration;
    info.attachment_filename = filename;
    auto handle = client->register_content(
        std::make_shared<loon::BufferContentSource>(
            std::vector<char>(content.begin(), content.end()), content_type),
        info);
    ASSERT_THAT(handle->url(), EndsWith(path));

    auto response = http_get(handle->url());
    EXPECT_EQ(content, response.body);
    EXPECT_EQ(content_type, response.headers["Content-Type"]);
    EXPECT_EQ("no-store", response.headers["Cache-Control"]);
    EXPECT_EQ("attachment; filename=\"page.html\"",
        response.headers["Content-Disposition"]);
    EXPECT_EQ(200, response.status);
}

TEST(Client, UnregisteredCallbackIsCalledWhenUnregistering)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled callback;
    handle->on_unregistered(callback.get());
    client->unregister_content(handle);
}

TEST(Client, IsUnregisteredReturnsFalseAfterUnregistering)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    EXPECT_TRUE(client->is_registered(handle));
    client->unregister_content(handle);
    EXPECT_FALSE(client->is_registered(handle));
}

TEST(Client, UrlIsInvalidWhenContentIsUnregistered)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    client->unregister_content(handle);
    auto response = http_get(handle->url());
    EXPECT_NE(200, response.status);
}

TEST(Client, UnregisteredCallbackIsCalledWhenSetAfterUnregistering)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    client->unregister_content(handle);
    ExpectCalled callback;
    handle->on_unregistered(callback.get());
}

TEST(Client, UnregisteredCallbackIsCalledWhenServerClosesConnection)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled callback;
    handle->on_unregistered(callback.get());
    // Trigger connection close with an invalid message.
    loon::ClientMessage message;
    auto empty_response = message.mutable_empty_response();
    empty_response->set_request_id(1000);
    client->send(message);
}

TEST(Client, IsUnregisteredReturnsFalseWhenServerClosesConnection)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    // Trigger connection close with an invalid message.
    loon::ClientMessage message;
    auto empty_response = message.mutable_empty_response();
    empty_response->set_request_id(1000);
    EXPECT_TRUE(client->is_registered(handle));
    client->send(message);
    std::this_thread::sleep_for(25ms);
    EXPECT_FALSE(client->is_registered(handle));
}

TEST(Client, UnregisteredCallbackIsCalledWhenClientClosesConnection)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled callback;
    handle->on_unregistered(callback.get());
    client->stop();
    EXPECT_EQ(1, callback.count());
}

TEST(Client, ServedCallbackIsCalledWhenContentHandleUrlIsRequested)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled callback;
    handle->on_served(callback.get());
    http_get(handle->url());
}

TEST(Client, NoActiveRequestsWhenHandleUrlRequestIsCanceled)
{
    const auto chunk_sleep = 20ms;
    const auto chunk_count = 3;
    const auto total_sleep = chunk_sleep * chunk_count;
    const auto cancel_delta = chunk_sleep;
    auto client = create_client();
    auto hello = client->wait_for_hello();
    auto chunk_size = hello.constraints().chunk_size();
    auto content = example_content_n(3 * chunk_size);
    client->chunk_sleep(chunk_sleep);
    auto handle = client->register_content(content.source, content.info);
    CurlOptions options{};
    // Force a send failure by timing out before everything is sent.
    options.timeout = total_sleep - chunk_sleep / 2;
    options.callback = [&client](std::string const& chunk) {
        EXPECT_EQ(1, client->active_requests());
    };
    EXPECT_EQ(0, client->active_requests());
    EXPECT_THROW(http_get(handle->url(), options), std::exception);
    std::this_thread::sleep_for(total_sleep + chunk_sleep);
    EXPECT_EQ(0, client->active_requests());
}

TEST(Client, FailsWhenMinCacheDurationIsSetAndServerDoesNotCacheResponses)
{
    ClientOptions options;
    options.min_cache_duration = std::chrono::seconds{ 10 };
    auto client = create_client(options, false);
    client->inject_hello_modifier([](Hello& hello) {
        hello.mutable_constraints()->set_cache_duration(0);
    });
    ExpectCalled callback;
    std::mutex mutex;
    std::condition_variable cv;
    bool done{ false };
    client->on_failed([&] {
        std::lock_guard lock(mutex);
        callback();
        cv.notify_one();
        done = true;
    });
    client->start();
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 250ms, [&] {
            return done;
        });
        ASSERT_TRUE(done);
    }
    // Wait for the Hello message to have been handled.
    // It's expected that the client is not connected anymore,
    // since the client should be in a failed state.
    EXPECT_THROW(client->wait_for_hello(), ClientNotStartedException);
}

TEST(Client, ReadyWhenClientIsStarted)
{
    auto client = create_client(false);
    ExpectCalled callback;
    client->on_ready(callback.get());
    client->start();
    EXPECT_NO_THROW(client->wait_until_ready());
}

TEST(Client, OnDisconnectWhenClientIsStopped)
{
    auto client = create_client(false);
    ExpectCalled callback;
    std::mutex mutex;
    std::condition_variable cv;
    bool done{ false };
    client->on_disconnect([&] {
        std::lock_guard lock(mutex);
        callback();
        cv.notify_one();
        done = true;
    });
    client->start();
    EXPECT_NO_THROW(client->wait_until_ready());
    client->stop();
    std::unique_lock lock(mutex);
    cv.wait_for(lock, 2s, [&] {
        return done;
    });
    EXPECT_TRUE(done);
}

TEST(Client, FailsWhenMinCacheDurationIsSetButResponseIsNotCached)
{
    uint32_t cache_duration = 10;
    ClientOptions options;
    options.min_cache_duration = std::chrono::seconds{ cache_duration / 2 };
    auto client = create_client(options, false);
    client->inject_hello_modifier([](Hello& hello) {
        if (hello.constraints().cache_duration() > 0) {
            FAIL() << "the test server is expected to not cache responses";
        }
        // The real test server does not cache responses,
        // but for the sake of the test, we pretend it does.
        // This would resemble a server that claims to cache, but doesn't.
        hello.mutable_constraints()->set_cache_duration(30);
    });
    client->start_and_wait_until_connected();
    auto content = example_content(cache_duration);
    auto handle = client->register_content(content.source, content.info);
    // The served callback should only be called once,
    // since it is expected to be cached on the server.
    ExpectCalled served(1);
    handle->on_served(served.get());
    auto result1 = http_get(handle->url());
    EXPECT_EQ(200, result1.status);
    auto result2 = http_get(handle->url());
    EXPECT_NE(200, result2.status);
}

TEST(Client, RestartsWhenReceivingTooManyNoContentRequests)
{
    ClientOptions options;
    options.no_content_request_limit = std::make_pair(1, 1s);
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    auto c1 = example_content("1.txt");
    auto c2 = example_content("2.txt");
    auto handle = client->register_content(c1.source, c1.info);
    client->register_content(c2.source, c2.info);
    client->unregister_content(handle);
    auto response1 = http_get(handle->url());
    EXPECT_EQ(404, response1.status);
    auto response2 = http_get(handle->url());
    EXPECT_NE(200, response2.status);
    EXPECT_NO_THROW(client->wait_until_ready());
    // Check that the client restarted, i.e. it has no content.
    EXPECT_EQ(0, client->content().size());
}

TEST(Client, DoesNotRestartWhenReceivingNoContentRequestsWithinLimit)
{
    ClientOptions options;
    options.no_content_request_limit = std::make_pair(2, 1s);
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    auto c1 = example_content("1.txt");
    auto c2 = example_content("2.txt");
    auto handle = client->register_content(c1.source, c1.info);
    client->register_content(c2.source, c2.info);
    client->unregister_content(handle);
    auto response1 = http_get(handle->url());
    EXPECT_EQ(404, response1.status);
    auto response2 = http_get(handle->url());
    EXPECT_EQ(404, response2.status);
    client->wait_until_ready();
    // Check that the client did not restart.
    EXPECT_EQ(1, client->content().size());
}

TEST(Client, DoesNotRestartWhenNoContentRequestLimitIsEmpty)
{
    ClientOptions options;
    options.no_content_request_limit = std::nullopt;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    auto c1 = example_content("1.txt");
    auto c2 = example_content("2.txt");
    auto handle = client->register_content(c1.source, c1.info);
    client->register_content(c2.source, c2.info);
    client->unregister_content(handle);
    auto response1 = http_get(handle->url());
    EXPECT_EQ(404, response1.status);
    auto response2 = http_get(handle->url());
    EXPECT_EQ(404, response2.status);
    auto response3 = http_get(handle->url());
    EXPECT_EQ(404, response3.status);
    // Check that the client did not restart.
    EXPECT_EQ(1, client->content().size());
}

TEST(Client, ClientCreationThrowsExceptionWhenNoContentRequestLimitIsUnset)
{
    ClientOptions options;
    // Not setting no_content_request_limit to anything explicitly.
    // options.no_content_request_limit = std::nullopt;
    EXPECT_THROW(TestClient(TEST_SERVER_WS, options), std::exception);
}

TEST(Client, CreationFailsWhenMaxUploadSpeedIsSet)
{
    ClientOptions options;
    options.max_upload_speed = 100;
    EXPECT_THROW(create_client(options, false), std::exception);
}

TEST(Client, ContentReturnsAllRegisteredContent)
{
    auto client = create_client();
    auto c1 = example_content("1.txt");
    auto c2 = example_content("2.txt");
    auto h1 = client->register_content(c1.source, c1.info);
    auto h2 = client->register_content(c2.source, c2.info);
    auto h = client->content();
    EXPECT_TRUE(std::find(h.begin(), h.end(), h1) != h.end());
    EXPECT_TRUE(std::find(h.begin(), h.end(), h2) != h.end());
}

TEST(Client, ClientRestartsWhenSendErrorOccurs)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled served(0);
    handle->on_served(served.get());
    client->inject_send_error(true);
    auto response = http_get(handle->url());
    EXPECT_NE(200, response.status);
    EXPECT_NO_THROW(client->wait_until_ready());
    EXPECT_EQ(0, client->content().size()); // should be restarted
}

TEST(Client, ServesReregisteredContentAfterRestart)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    client->inject_send_error(true);
    auto failed_response = http_get(handle->url());
    client->inject_send_error(false);
    EXPECT_NE(200, failed_response.status);
    EXPECT_NO_THROW(client->wait_until_ready());
    EXPECT_EQ(0, client->content().size()); // should be restarted
    handle = client->register_content(content.source, content.info);
    ExpectCalled served(1);
    handle->on_served(served.get());
    auto response = http_get(handle->url());
    EXPECT_EQ(200, response.status);
    EXPECT_EQ(content.data, response.body);
}

#define EXPECT_CONNECTION_STATE_SWAP_AFTER(state, duration, epsilon) \
    std::this_thread::sleep_for(duration - epsilon);                 \
    EXPECT_EQ(!state, client->connected());                          \
    std::this_thread::sleep_for(2 * epsilon);                        \
    EXPECT_EQ(state, client->connected());

#define EXPECT_CONNECTION_STATE_AFTER(state, duration, epsilon) \
    std::this_thread::sleep_for(duration + epsilon);            \
    EXPECT_EQ(state, client->connected());

TEST(Client, DisconnectsAfterDurationWhenDisconnectAfterIdleIsSet)
{
    ClientOptions options;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    EXPECT_CONNECTION_STATE_SWAP_AFTER(
        false, options.disconnect_after_idle.value(), 25ms);
}

TEST(Client, DoesNotDisconnectWhenDisconnectAfterIdleIsSetAndContentRegistered)
{
    ClientOptions options;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    EXPECT_CONNECTION_STATE_AFTER(
        true, options.disconnect_after_idle.value(), 25ms);
}

TEST(Client, RegisteringContentConnectsAgainWhenDisconnectAfterIdleIsSet)
{
    ClientOptions options;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    EXPECT_CONNECTION_STATE_SWAP_AFTER(
        false, options.disconnect_after_idle.value(), 25ms);
    auto content = example_content();
    std::shared_ptr<loon::ContentHandle> handle;
    EXPECT_NO_THROW(
        handle = client->register_content(content.source, content.info));
    ASSERT_TRUE(client->connected());
}

TEST(Client, DisconnectsWhenDisconnectAfterIdleIsSetAndAllContentUnregistered)
{
    ClientOptions options;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    EXPECT_CONNECTION_STATE_AFTER(
        true, options.disconnect_after_idle.value(), 25ms);
    client->unregister_content(handle);
    ASSERT_EQ(0, client->content().size());
    EXPECT_CONNECTION_STATE_SWAP_AFTER(
        false, options.disconnect_after_idle.value(), 25ms);
}

TEST(Client, DisconnectsWhenDisconnectAfterIdleIsSetAndContentRegistrationFails)
{
    ClientOptions options;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    // Registration fails, as this content type is not allowed.
    auto content = create_content("path", "image/png", "content");
    EXPECT_THROW(client->register_content(content.source, content.info),
        UnacceptableContentException);
    EXPECT_CONNECTION_STATE_SWAP_AFTER(
        false, options.disconnect_after_idle.value(), 25ms);
}

TEST(Client, CanBeStartedAgainWhenStoppedByFailure)
{
    ClientOptions options;
    options.min_cache_duration = std::chrono::seconds{ 10 };
    options.websocket.connect_timeout = 500ms;
    auto client = create_client(options, false);
    client->inject_hello_modifier([](Hello& hello) {
        hello.mutable_constraints()->set_cache_duration(0);
    });
    ExpectCalled callback;
    std::mutex mutex;
    std::condition_variable cv;
    bool done{ false };
    client->on_failed([&] {
        std::lock_guard lock(mutex);
        callback();
        cv.notify_one();
        done = true;
    });
    client->start();
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 2s, [&] {
            return done;
        });
        ASSERT_TRUE(done);
    }
    EXPECT_THROW(client->wait_for_hello(), ClientNotStartedException);
    client->inject_hello_modifier([](Hello& hello) {
        // Increase the cache duration, so it won't fail again.
        hello.mutable_constraints()->set_cache_duration(30);
    });
    client->on_failed([] {});
    // Wait for the client to be properly disconnected.
    std::this_thread::sleep_for(25ms);
    auto content = example_content();
    EXPECT_ANY_THROW(client->register_content(content.source, content.info));
    // Start the client again after failure.
    EXPECT_NO_THROW(client->start_and_wait_until_connected());
    EXPECT_TRUE(client->connected());
}

TEST(Client, DisconnectsWhenContentIsRegisteredAndIdleIsCalled)
{
    ClientOptions options;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    client->idle();
    EXPECT_TRUE(client->idling());
    EXPECT_CONNECTION_STATE_SWAP_AFTER(
        false, options.disconnect_after_idle.value(), 25ms);
    EXPECT_FALSE(client->idling());
}

TEST(Client, DisconnectsWhenIdleIsCalledAndContentRegisteredAfterUnregistering)
{
    ClientOptions options;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    auto content1 = example_content("1.txt");
    auto content2 = example_content("2.txt");
    auto handle1 = client->register_content(content1.source, content1.info);
    auto handle2 = client->register_content(content2.source, content2.info);
    client->idle();
    EXPECT_TRUE(client->idling());
    client->unregister_content(handle2);
    EXPECT_CONNECTION_STATE_SWAP_AFTER(
        false, options.disconnect_after_idle.value(), 25ms);
    EXPECT_FALSE(client->idling());
}

TEST(Client, StaysConnectedWhenIdleIsCalledAndThenRegisteringNewContent)
{
    ClientOptions options;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    auto content1 = example_content("1.txt");
    auto content2 = example_content("2.txt");
    auto handle1 = client->register_content(content1.source, content1.info);
    client->idle();
    EXPECT_TRUE(client->idling());
    std::this_thread::sleep_for(25ms);
    EXPECT_TRUE(client->idling());
    auto handle2 = client->register_content(content2.source, content2.info);
    EXPECT_CONNECTION_STATE_AFTER(
        true, options.disconnect_after_idle.value(), 25ms);
    EXPECT_FALSE(client->idling());
}

TEST(Client, ReconnectsWhenIdleDisconnectedAndWaitUntilConnectedIsCalled)
{
    ClientOptions options;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    EXPECT_CONNECTION_STATE_SWAP_AFTER(
        false, options.disconnect_after_idle.value(), 25ms);
    EXPECT_TRUE(client->wait_until_ready());
    EXPECT_TRUE(client->connected());
}

TEST(Client, RegisterContentWorksWhenConnectedAndTimeoutIsZero)
{
    auto client = create_client(false);
    auto content = example_content();
    client->start();
    client->wait_until_ready();
    auto t1 = std::chrono::high_resolution_clock::now();
    std::shared_ptr<loon::ContentHandle> handle;
    EXPECT_NO_THROW(
        handle = client->register_content(content.source, content.info, 0ms));
    auto t2 = std::chrono::high_resolution_clock::now();
    auto delta = t2 - t1;
    EXPECT_LT(delta, 25ms);
    EXPECT_NE(nullptr, handle);
}

TEST(Client, ReconnectsWhenIdleDisconnectedAndStartIsCalled)
{
    ClientOptions options;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    EXPECT_CONNECTION_STATE_SWAP_AFTER(
        false, options.disconnect_after_idle.value(), 25ms);
    client->start();
    // Make sure to not call wait_until_ready by accident,
    // since that also guarantees that the client will reconnect after idle.
    // Instead sleep for a small duration.
    std::this_thread::sleep_for(50ms);
    EXPECT_TRUE(client->connected());
}

TEST(Client, RegisteringContentThrowsWhenContentSizeIsZero)
{
    auto client = create_client();
    auto content = example_content_n(0);
    EXPECT_THROW(client->register_content(content.source, content.info),
        MalformedContentException);
}

TEST(Client, RegisteringContentThrowsWhenContentHandleIsNull)
{
    auto client = create_client();
    auto content = example_content_n(0);
    EXPECT_THROW(client->register_content(nullptr, content.info),
        MalformedContentException);
}

TEST(Client, StartingTheClientAgainDisablesIdling)
{
    ClientOptions options;
    options.automatic_idling = false;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    client->idle();
    std::this_thread::sleep_for(options.disconnect_after_idle.value() - 25ms);
    client->start();
    EXPECT_TRUE(client->connected());
    std::this_thread::sleep_for(2 * 25ms);
    EXPECT_TRUE(client->connected()); // not idling anymore
}

TEST(Client, CallingIdleWithFalseDisablesIdling)
{
    ClientOptions options;
    options.automatic_idling = false;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    client->idle();
    std::this_thread::sleep_for(options.disconnect_after_idle.value() - 25ms);
    client->idle(false);
    EXPECT_TRUE(client->connected());
    std::this_thread::sleep_for(2 * 25ms);
    EXPECT_TRUE(client->connected()); // not idling anymore
}

enum CallbackOrderFlag
{
    FLAG_CALLBACK,
    FLAG_AFTER_CALL
};

TEST(Client, StoppingTheClientMustNotWaitForCallbacks)
{
    using namespace std::chrono_literals;
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled callback;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<CallbackOrderFlag> flags;
    client->on_disconnect([&] {
        std::lock_guard lock(mutex);
        std::this_thread::sleep_for(100ms);
        callback();
        flags.push_back(FLAG_CALLBACK);
        cv.notify_one();
    });
    client->stop();
    flags.push_back(FLAG_AFTER_CALL);
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 2s, [&] {
            return std::find(flags.begin(), flags.end(), FLAG_CALLBACK) !=
                flags.end();
        });
    }
    client->on_disconnect([] {});
    EXPECT_EQ(1, callback.count());
    ASSERT_EQ(2, flags.size());
    EXPECT_TRUE(
        std::find(flags.begin(), flags.end(), FLAG_CALLBACK) != flags.end());
    EXPECT_TRUE(
        std::find(flags.begin(), flags.end(), FLAG_AFTER_CALL) != flags.end());
}

TEST(Client, TerminatingTheClientWaitsForCallbacks)
{
    using namespace std::chrono_literals;
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled callback;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<CallbackOrderFlag> flags;
    client->on_disconnect([&] {
        std::lock_guard lock(mutex);
        std::this_thread::sleep_for(100ms);
        callback();
        flags.push_back(FLAG_CALLBACK);
        cv.notify_one();
    });
    client->terminate();
    flags.push_back(FLAG_AFTER_CALL);
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 2s, [&] {
            return std::find(flags.begin(), flags.end(), FLAG_CALLBACK) !=
                flags.end();
        });
    }
    client->on_disconnect([] {});
    EXPECT_EQ(1, callback.count());
    ASSERT_EQ(2, flags.size());
    EXPECT_EQ(FLAG_CALLBACK, flags[0]);
    EXPECT_EQ(FLAG_AFTER_CALL, flags[1]);
}

TEST(Client, StartCanBeCalledAgainAfterTerminate)
{
    auto client = create_client();
    auto content = example_content();
    auto h1 = client->register_content(content.source, content.info);
    client->terminate();
    EXPECT_NO_THROW(client->start());
    EXPECT_NO_THROW(client->wait_until_ready());
    EXPECT_TRUE(client->started());
    auto h2 = client->register_content(content.source, content.info);
    auto response = http_get(h2->url());
    EXPECT_EQ(content.data, response.body);
}

TEST(Client, CanBeConstructedWithMoveConstructor)
{
    loon::ClientOptions options{};
    options.no_content_request_limit = std::make_pair(8, 1s);
    loon::Client client1(TEST_SERVER_WS, options);
    client1.start();
    client1.wait_until_ready();
    loon::Client client2(std::move(client1));
    EXPECT_NO_THROW(EXPECT_FALSE(client2.wait_until_ready()));
    // Test the default move-assignment operator as well.
    loon::Client client3 = std::move(client2);
    EXPECT_NO_THROW(EXPECT_FALSE(client3.wait_until_ready()));
}

TEST(Client, DisconnectsAfterDoubleThePingIntervalOnDroppedConnection)
{
    loon::ClientOptions options{};
    auto ping_interval = 250ms;
    const auto expected_ping_timeout = 2 * ping_interval;
    options.websocket.ping_interval = ping_interval;
    auto client = create_client(options, false);
    client->start_and_wait_until_connected();
    EXPECT_TRUE(client->connected());
    drop_server_packets(true);
    std::this_thread::sleep_for(expected_ping_timeout + 50ms);
    EXPECT_FALSE(client->connected());
}

// TEST(Client, AutomaticallyRestartsAfterPingTimeout)
// {
//     loon::ClientOptions options{};
//     options.websocket.ping_interval = 1s;
//     auto client = create_client(options);
//     auto content = example_content();
//     auto handle = client->register_content(content.source, content.info);
//     // ExpectCalled callback;
//     // handle->on_unregistered(callback.get());
//     // client->unregister_content(handle);
//     std::this_thread::sleep_for(100s);
// }

// TODO TEST Check that the client doesn't idle anymore when it disconnects
// from and then reconnects to the server internally. It shouldn't
// idle-disconnect shortly after reconnecting. I think it does at the moment.

// TODO TEST The client should not reconnect after being stopped explicitly
// TODO TEST The client should not reconnect after being stopped due to idle

// TODO TEST The client should not idle stop after a disconnect

// TODO TEST Do not hold public Client mutex during send(), when responding
// to a websocket message...
