#include "common/test.hpp"

TEST(SharedClient, ConstructorThrowsWithSharedClientAsArgument)
{
    auto client = create_client(false);
    auto s = std::make_shared<SharedClient>(client);
    EXPECT_ANY_THROW(SharedClient(std::dynamic_pointer_cast<IClient>(s)));
}

TEST(SharedClient, StartedNotDelegatedWhenNotStarted)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s = std::make_shared<SharedClient>(check);
    EXPECT_FALSE(s->started());
    EXPECT_EQ(0, check->n_started());
}

TEST(SharedClient, MustBeStartedToBeStopped)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s = std::make_shared<SharedClient>(check);
    s->stop();
    // The stop call wasn't actually delegated.
    EXPECT_EQ(0, check->n_stop());
    EXPECT_EQ(0, check->n_start());
    s->start();
    EXPECT_EQ(1, check->n_start()); // delegated(start)
    auto prev_n_started = check->n_started();
    EXPECT_TRUE(s->started());
    EXPECT_EQ(prev_n_started + 1, check->n_started()); // delegated(started)
    s->wait_until_ready();
    s->stop();
    // After starting it is delegated.
    EXPECT_EQ(1, check->n_stop()); // delegated(stop)
}

TEST(SharedClient, WaitUntilReadyThrowsClientNotStartedExceptionWhenNotStarted)
{
    auto c1 = create_client(false);
    auto c2 = create_client(false);
    auto check1 = std::make_shared<ClientCallCheck>(c1);
    auto check2 = std::make_shared<ClientCallCheck>(c2);
    auto s1 = std::make_shared<SharedClient>(check1);
    auto s2 = std::make_shared<SharedClient>(check2);
    EXPECT_THROW(s1->wait_until_ready(), loon::ClientNotStartedException);
    EXPECT_THROW(s2->wait_until_ready(100ms), loon::ClientNotStartedException);
    EXPECT_EQ(0, check1->n_wait_until_ready());
    EXPECT_EQ(0, check2->n_wait_until_ready_timeout());
    s1->start();
    s2->start();
    EXPECT_NO_THROW(s1->wait_until_ready());
    EXPECT_NO_THROW(s2->wait_until_ready(250ms));
    EXPECT_EQ(1, check1->n_wait_until_ready());
    EXPECT_EQ(1, check2->n_wait_until_ready_timeout());
}

TEST(SharedClient, SharedClientIsNotStartedWhenWrappedClientIsAlreadyStarted)
{
    auto client = create_client(true);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s = std::make_shared<SharedClient>(check);
    EXPECT_FALSE(s->started());
    EXPECT_EQ(0, check->n_started());
}

TEST(SharedClient, LastStopCallOfTwoSharedClientsDelegatesStop)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    EXPECT_EQ(0, check->n_stop());
    s1->stop();
    EXPECT_EQ(0, check->n_stop());
    s2->stop();
    EXPECT_EQ(1, check->n_stop());
    // The underlying client is not started anymore.
    EXPECT_FALSE(client->started());
}

TEST(SharedClient, WhenLastStartedClientIsDestructedTheClientIsStopped)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    {
        auto s1 = std::make_shared<SharedClient>(check);
        s1->start();
        s1->wait_until_ready();
        EXPECT_TRUE(client->started());
        {
            auto s2 = std::make_shared<SharedClient>(check);
            s2->start();
            s2->wait_until_ready();
        }
        EXPECT_TRUE(client->started());
    }
    // Client is stopped after all shared clients are destructed.
    EXPECT_FALSE(client->started());
}

TEST(SharedClient, IdleIsDelegatedOnlyWhenAllSharedClientsIdle)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    EXPECT_EQ(0, check->n_idle_true());
    s1->idle();
    EXPECT_EQ(0, check->n_idle_true());
    s2->idle();
    EXPECT_EQ(1, check->n_idle_true());
}

TEST(SharedClient, DoesNotDelegateIdleWhenBeingStartedAgain)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    EXPECT_EQ(0, check->n_idle_true());
    s1->idle();
    s1->start();
    EXPECT_EQ(0, check->n_idle_true());
    s2->idle();
    EXPECT_EQ(0, check->n_idle_true());
}

TEST(SharedClient, DoesNotIdleAnymoreWhenBeingStartedAgain)
{
    ClientOptions options;
    options.automatic_idling = false;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    s1->start();
    s1->wait_until_ready();
    EXPECT_EQ(0, check->n_idle_true());
    s1->idle();
    EXPECT_EQ(1, check->n_idle_true());
    EXPECT_EQ(1, check->n_start());
    s1->start();
    EXPECT_EQ(2, check->n_start());
    // Check that the client really is still connected.
    std::this_thread::sleep_for(options.disconnect_after_idle.value() - 25ms);
    EXPECT_TRUE(client->connected());
    std::this_thread::sleep_for(2 * 25ms);
    EXPECT_TRUE(client->connected()); // still connected
}

TEST(SharedClient, CallingIdleWithFalseDisablesIdlingWhenClientAutomaticIdles)
{
    ClientOptions options;
    options.automatic_idling = true;
    options.disconnect_after_idle = 250ms;
    auto client = create_client(options, false);
    auto s1 = std::make_shared<SharedClient>(client);
    s1->start();
    s1->wait_until_ready();
    s1->idle(false);
    std::this_thread::sleep_for(options.disconnect_after_idle.value() - 25ms);
    EXPECT_TRUE(client->connected());
    std::this_thread::sleep_for(2 * 25ms);
    EXPECT_TRUE(client->connected()); // still connected
}

TEST(SharedClient, DoesNotIdleAnymoreWhenCallingIdleWithFalse)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    EXPECT_EQ(0, check->n_idle_true());
    s1->idle();
    s1->idle(false);
    // At this point the first client should not be idling anymore.
    EXPECT_EQ(0, check->n_idle_true());
    s2->idle();
    // Therefore idle is never delegated.
    EXPECT_EQ(0, check->n_idle_true());
}

TEST(SharedClient, StopsIdlingOnceOneSharedClientIsNotIdlingAnymore)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    s1->idle();
    s2->idle();
    EXPECT_EQ(1, check->n_idle_true());
    EXPECT_EQ(0, check->n_idle_false());
    s1->idle(false);
    EXPECT_EQ(1, check->n_idle_false());
    s2->idle(false);
    EXPECT_EQ(1, check->n_idle_false());
}

TEST(SharedClient, IdlesAgainWhenAllIdlingClientsArePutOutOfAndBackIntoIdle)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    EXPECT_EQ(0, check->n_idle_true());
    s1->idle();
    EXPECT_EQ(0, check->n_idle_true());
    s2->idle();
    EXPECT_EQ(1, check->n_idle_true());
    s1->idle(false);
    s2->idle(false);
    s1->idle();
    EXPECT_EQ(1, check->n_idle_true());
    s2->idle();
    EXPECT_EQ(2, check->n_idle_true());
}

TEST(SharedClient, ExternallyStoppedClientCanBeStartedAgain)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s = std::make_shared<SharedClient>(check);
    s->start();
    EXPECT_EQ(1, check->n_start());
    s->wait_until_ready();
    client->stop(); // externally stopped
    s->start();
    EXPECT_EQ(2, check->n_start());
}

TEST(SharedClient, ExternallyStoppedClientCanBeStopped)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s = std::make_shared<SharedClient>(check);
    s->start();
    s->wait_until_ready();
    client->stop(); // externally stopped
    s->stop();
    EXPECT_EQ(1, check->n_stop());
}

TEST(SharedClient, RegisteredContentPathIsPrefixed)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s = std::make_shared<SharedClient>(check);
    s->start();
    s->wait_until_ready();
    auto c = example_content("file.txt");
    auto h = s->register_content(c.source, c.info);
    EXPECT_THAT(h->url(), EndsWith(s->path_prefix() + c.path));
}

TEST(SharedClient, RegisteredContentIsSeparateBetweenSharedClients)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    auto c1 = example_content("1.txt");
    auto c2 = example_content("2.txt");
    auto c3 = example_content("3.txt");
    auto h1 = s1->register_content(c1.source, c1.info);
    auto h2 = s2->register_content(c2.source, c2.info);
    auto h3 = s2->register_content(c3.source, c3.info);
    EXPECT_THAT(s1->content(), UnorderedElementsAre(h1));
    EXPECT_THAT(s2->content(), UnorderedElementsAre(h2, h3));
    EXPECT_TRUE(s1->is_registered(h1));
    EXPECT_FALSE(s1->is_registered(h2));
    EXPECT_FALSE(s1->is_registered(h3));
    EXPECT_FALSE(s2->is_registered(h1));
    EXPECT_TRUE(s2->is_registered(h2));
    EXPECT_TRUE(s2->is_registered(h3));
}

TEST(SharedClient, RegisteredContentIsUnregisteredForSharedClientOnDestruction)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    {
        auto s1 = std::make_shared<SharedClient>(check);
        s1->start();
        s1->wait_until_ready();
        auto c1 = example_content("1.txt");
        auto h1 = s1->register_content(c1.source, c1.info);
        {
            auto s2 = std::make_shared<SharedClient>(check);
            s2->start();
            s2->wait_until_ready();
            auto c2 = example_content("2.txt");
            auto h2 = s2->register_content(c2.source, c2.info);
            EXPECT_THAT(s1->content(), UnorderedElementsAre(h1));
            EXPECT_THAT(s2->content(), UnorderedElementsAre(h2));
            EXPECT_THAT(client->content(), UnorderedElementsAre(h1, h2));
        }
        EXPECT_THAT(s1->content(), UnorderedElementsAre(h1));
        EXPECT_THAT(client->content(), UnorderedElementsAre(h1));
    }
    EXPECT_THAT(client->content(), UnorderedElementsAre());
}

TEST(SharedClient, RegisteredContentIsUnregisteredForSharedClientOnStop)
{
    auto client = create_client(false);

    // BEGIN s1
    auto s1 = std::make_shared<SharedClient>(client);
    s1->start();
    s1->wait_until_ready();
    auto c1 = example_content("1.txt");
    auto h1 = s1->register_content(c1.source, c1.info);

    // BEGIN s2
    auto s2 = std::make_shared<SharedClient>(client);
    s2->start();
    s2->wait_until_ready();
    auto c2 = example_content("2.txt");
    auto h2 = s2->register_content(c2.source, c2.info);
    EXPECT_THAT(s1->content(), UnorderedElementsAre(h1));
    EXPECT_THAT(s2->content(), UnorderedElementsAre(h2));
    EXPECT_THAT(client->content(), UnorderedElementsAre(h1, h2));
    s2->stop();
    // END s2

    EXPECT_THAT(s1->content(), UnorderedElementsAre(h1));
    EXPECT_THAT(client->content(), UnorderedElementsAre(h1));
    s1->stop();
    // END s1

    EXPECT_THAT(client->content(), UnorderedElementsAre());
}

TEST(SharedClient, UnregisteringContentFromAnotherSharedClientThrows)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    auto c1 = example_content("1.txt");
    auto c2 = example_content("2.txt");
    auto h1 = s1->register_content(c1.source, c1.info);
    auto h2 = s2->register_content(c2.source, c2.info);
    EXPECT_THROW(s1->unregister_content(h2), loon::MalformedContentException);
    EXPECT_THROW(s2->unregister_content(h1), loon::MalformedContentException);
}

TEST(SharedClient, UnregisterContentWithNullPointerThrows)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s = std::make_shared<SharedClient>(check);
    EXPECT_THROW(
        s->unregister_content(nullptr), loon::MalformedContentException);
}

TEST(SharedClient, IsRegisteredWithNullPointerThrows)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s = std::make_shared<SharedClient>(check);
    EXPECT_THROW(s->is_registered(nullptr), loon::MalformedContentException);
}

TEST(SharedClient, MultipleSharedClientsReturnConsecutiveUniqueIndices)
{
    auto client = create_client(false);
    auto s1 = std::make_shared<SharedClient>(client);
    auto s2 = std::make_shared<SharedClient>(client);
    auto s3 = std::make_shared<SharedClient>(client);
    EXPECT_EQ(0, s1->index());
    EXPECT_EQ(1, s2->index());
    EXPECT_EQ(2, s3->index());
    {
        auto s4 = std::make_shared<SharedClient>(client);
        EXPECT_EQ(3, s4->index());
    }
    auto s5 = std::make_shared<SharedClient>(client);
    EXPECT_EQ(4, s5->index());
}

TEST(SharedClient, IndexResetsToZeroAfterAllSharedClientsWereDestructed)
{
    auto client = create_client(false);
    {
        auto s1 = std::make_shared<SharedClient>(client);
        auto s2 = std::make_shared<SharedClient>(client);
        auto s3 = std::make_shared<SharedClient>(client);
        EXPECT_EQ(0, s1->index());
        EXPECT_EQ(1, s2->index());
        EXPECT_EQ(2, s3->index());
    }
    {
        auto s1 = std::make_shared<SharedClient>(client);
        EXPECT_EQ(0, s1->index());
    }
}

TEST(SharedClient, OnReadyIsOnlyCalledOnStartedSharedClients)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    ExpectCalled c1(1), c2(0);
    s1->on_ready(c1.get());
    s2->on_ready(c2.get());
    EXPECT_EQ(0, c1.count());
    EXPECT_EQ(0, c2.count());
    s1->start();
    s1->wait_until_ready();
    EXPECT_EQ(1, c1.count());
    EXPECT_EQ(0, c2.count()); // not started
}

TEST(SharedClient, OnReadyIsOnlyCalledForStartedClients)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    ExpectCalled c1(1), c2(0);
    s1->on_ready(c1.get());
    s2->on_ready(c2.get());
    s1->start();
    s1->wait_until_ready();
    EXPECT_EQ(0, c2.count()); // not started
}

TEST(SharedClient, OnReadyCalledOnStartWhenClientIsAlreadyConnected)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    ExpectCalled c;
    s2->on_ready(c.get());
    s1->start();
    s1->wait_until_ready();
    EXPECT_EQ(0, c.count()); // not started
    // the ready callback should be called during the start() method call.
    s2->start();
    EXPECT_EQ(1, c.count()); // already called during start()
    EXPECT_FALSE(s2->wait_until_ready());
    EXPECT_EQ(1, c.count());
}

TEST(SharedClient, OnReadyCallbackIsNotCalledTwiceByStartForAConnectedClient)
{
    // This test handles a race-condition where the shared client's
    // start method believes that it needs to call the ready callback again,
    // even though it has already been called by the client implementation.

    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s = std::make_shared<TestSharedClient>(check);
    // Ensure that the connection is only ready after at least 50 milliseconds,
    // so that there is sufficent time for time-critical operations.
    client->incoming_sleep(50ms);
    // Ensure that there is enough time for the connection to be ready
    // before the ready callback is called manually by the start() method.
    s->before_manual_start_callback_sleep(100ms);
    // Register the ready callback.
    ExpectCalled c(1);
    s->on_ready(c.get());
    // Make sure the client is already started before calling
    // the shared client's start() method, so that it attempts
    // to call the registered ready callback manually.
    client->start();
    // Now the start method should attempt to call the ready callback manually.
    s->start();
    EXPECT_FALSE(s->wait_until_ready());
    // The start method should detect that the ready callback has
    // already been called and not call it again.
    EXPECT_EQ(1, c.count());
}

TEST(SharedClient, OnReadyCalledOnBothSharedClientsAfterBothAreStarted)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    ExpectCalled c1, c2;
    s1->on_ready(c1.get());
    s2->on_ready(c2.get());
    EXPECT_EQ(0, c1.count());
    s1->start();
    EXPECT_EQ(0, c2.count());
    s2->start();
    s1->wait_until_ready();
    EXPECT_EQ(1, c1.count());
    s2->wait_until_ready();
    EXPECT_EQ(1, c2.count());
}

TEST(SharedClient, CallbacksAreCalledInOrderOfSharedClientCreation)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    auto s3 = std::make_shared<SharedClient>(check);
    auto s4 = std::make_shared<SharedClient>(check);
    auto s5 = std::make_shared<SharedClient>(check);
    size_t order = 1;
    auto callback_for = [&](std::shared_ptr<SharedClient> client) {
        return [client, &order] {
            EXPECT_EQ(1 << client->index(), order);
            order <<= 1;
        };
    };
    s1->on_ready(callback_for(s1));
    s2->on_ready(callback_for(s2));
    s3->on_ready(callback_for(s3));
    s4->on_ready(callback_for(s4));
    s5->on_ready(callback_for(s5));
}

TEST(SharedClient, OnDisconnectIsCalledWhenClientDisconnects)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s = std::make_shared<SharedClient>(check);
    ExpectCalled callback;
    std::mutex mutex;
    std::condition_variable cv;
    bool done{ false };
    s->on_disconnect([&] {
        std::lock_guard lock(mutex);
        callback();
        cv.notify_one();
        done = true;
    });
    s->start();
    s->wait_until_ready();
    s->stop();
    std::unique_lock lock(mutex);
    cv.wait_for(lock, 2s, [&] {
        return done;
    });
    EXPECT_TRUE(done);
}

TEST(SharedClient, OnDisconnectIsCalledWhenOneOfMultipleStartedClientsIsStopped)
{

    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    // Connect with the first shared client first.
    s1->start();
    s1->wait_until_ready();
    // Then connect and disconnect the second shared client.
    ExpectCalled callback;
    std::mutex mutex;
    std::condition_variable cv;
    bool done{ false };
    s2->on_disconnect([&] {
        std::lock_guard lock(mutex);
        callback();
        cv.notify_one();
        done = true;
    });
    s2->start();
    s2->wait_until_ready();
    s2->stop();
    std::unique_lock lock(mutex);
    cv.wait_for(lock, 250s, [&] {
        return done;
    });
    EXPECT_TRUE(done);
}

TEST(SharedClient, OnFailedIsCalledWhenWrappedClientFails)
{
    // Copied from FailsWhenMinCacheDurationIsSetAndServerDoesNotCacheResponses

    ClientOptions options;
    options.min_cache_duration = std::chrono::seconds{ 10 };
    auto client = create_client(options, false);
    client->inject_hello_modifier([](Hello& hello) {
        hello.mutable_constraints()->set_cache_duration(0);
    });
    auto s1 = std::make_shared<SharedClient>(client);
    auto s2 = std::make_shared<SharedClient>(client);
    ExpectCalled c1, c2;
    std::mutex mutex;
    std::condition_variable cv;
    bool d1{ false }, d2{ false };
    s1->on_failed([&] {
        std::lock_guard lock(mutex);
        c1();
        cv.notify_one();
        d1 = true;
    });
    s2->on_failed([&] {
        std::lock_guard lock(mutex);
        c2();
        cv.notify_one();
        d2 = true;
    });
    s1->start();
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 250ms, [&] {
            return d1 && d2;
        });
        ASSERT_TRUE(d1);
        ASSERT_TRUE(d2);
    }
    // Wait for the Hello message to have been handled.
    // It's expected that the client is not connected anymore,
    // since the client should be in a failed state.
    EXPECT_THROW(client->wait_for_hello(), ClientNotStartedException);
}

TEST(SharedClient, IntegrationTest)
{
    enum Action
    {
        Ready,
        Disconnect,
        Served,
        Unregistered
    };

    auto client = create_client(false);
    auto s1 = std::make_shared<SharedClient>(client);
    auto s2 = std::make_shared<SharedClient>(client);
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::pair<size_t, Action>> history;
    auto expect_action = [&](size_t client, Action action) {
        std::unique_lock lock(mutex);
        auto result = cv.wait_for(lock, 250ms, [&] {
            return !history.empty();
        });
        ASSERT_TRUE(result);
        auto front = history.front();
        history.pop_front();
        EXPECT_EQ(action, front.second);
        EXPECT_EQ(client, front.first);
    };
    auto push_action = [&](size_t client, Action action) {
        std::lock_guard lock(mutex);
        history.push_back(std::make_pair(client, action));
        cv.notify_one();
    };
    s1->on_ready(std::bind(push_action, 1, Ready));
    s1->on_disconnect(std::bind(push_action, 1, Disconnect));
    s2->on_ready(std::bind(push_action, 2, Ready));
    s2->on_disconnect(std::bind(push_action, 2, Disconnect));
    // Some start and stop calls that trigger ready and disconnect calls.
    s1->start();
    s1->wait_until_ready();
    expect_action(1, Ready);
    s1->stop();
    expect_action(1, Disconnect);
    s2->start();
    s2->wait_until_ready();
    expect_action(2, Ready);
    s1->start();
    s1->wait_until_ready();
    expect_action(1, Ready);
    s2->stop();
    expect_action(2, Disconnect);
    s2->start();
    s2->wait_until_ready();
    expect_action(2, Ready);
    // Register and request some content
    auto c1 = create_content("1.txt", "text/plain", "hg23kj1h");
    auto c2 = create_content("2.txt", "text/plain", "jh321g4k");
    auto h1 = s1->register_content(c1.source, c1.info);
    auto h2 = s2->register_content(c2.source, c2.info);
    h1->on_served(std::bind(push_action, 1, Served));
    h1->on_unregistered(std::bind(push_action, 1, Unregistered));
    h2->on_served(std::bind(push_action, 2, Served));
    h2->on_unregistered(std::bind(push_action, 2, Unregistered));
    EXPECT_THAT(s1->content(), UnorderedElementsAre(h1));
    EXPECT_THAT(s2->content(), UnorderedElementsAre(h2));
    auto r1 = http_get(h1->url());
    expect_action(1, Served);
    EXPECT_EQ(200, r1.status);
    EXPECT_EQ(c1.data, r1.body);
    auto r2 = http_get(h2->url());
    expect_action(2, Served);
    EXPECT_EQ(200, r2.status);
    EXPECT_EQ(c2.data, r2.body);
    // Disconnect both
    EXPECT_THAT(client->content(), UnorderedElementsAre(h1, h2));
    s2->stop();
    expect_action(2, Unregistered);
    EXPECT_THAT(s2->content(), UnorderedElementsAre());
    EXPECT_THAT(client->content(), UnorderedElementsAre(h1));
    expect_action(2, Disconnect);
    s1->unregister_content(h1);
    expect_action(1, Unregistered);
    EXPECT_THAT(s1->content(), UnorderedElementsAre());
    EXPECT_THAT(client->content(), UnorderedElementsAre());
    EXPECT_FALSE(client->wait_until_ready());
    EXPECT_TRUE(client->started());
    EXPECT_TRUE(s1->started());
    EXPECT_FALSE(s2->started());
    s1->stop();
    expect_action(1, Disconnect);
    EXPECT_ANY_THROW(client->wait_until_ready());
    EXPECT_FALSE(client->started());
    EXPECT_FALSE(s1->started());
    EXPECT_FALSE(s2->started());
    // Ensure there are not more actions than expected
    EXPECT_TRUE(history.empty());
}

TEST(SharedClient, RegisteringIdenticalContentWithSeparateSharedClientsWorks)
{
    auto client = create_client(false);
    auto s1 = std::make_shared<SharedClient>(client);
    auto s2 = std::make_shared<SharedClient>(client);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    auto c1 = example_content("1.txt");
    auto h1 = s1->register_content(c1.source, c1.info);
    EXPECT_NO_THROW(s2->register_content(c1.source, c1.info));
}

TEST(SharedClient, NoContentRegisteredAfterWrappedClientRestarted)
{
    // This tests that the registered content that is tracked internally
    // with the shared client is properly detected as not registered anymore
    // when the wrapped client unregisters content and the shared client
    // is not notified of that.

    // Copied from RestartsWhenReceivingTooManyNoContentRequests

    ClientOptions options;
    options.no_content_request_limit = std::make_pair(1, 1s);
    auto client = create_client(options, false);
    auto s1 = std::make_shared<SharedClient>(client);
    auto s2 = std::make_shared<SharedClient>(client);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    auto c1 = example_content("1.txt");
    auto c2 = example_content("2.txt");
    auto h11 = s1->register_content(c1.source, c1.info);
    auto h12 = s1->register_content(c2.source, c2.info);
    auto h21 = s2->register_content(c1.source, c1.info);
    auto h22 = s2->register_content(c2.source, c2.info);
    EXPECT_EQ(2, s1->content().size());
    // Unregister from the real client instead of the shared client
    // so that the erasure of stray content in the content() method
    // is also tested for the case that content is still left.
    client->unregister_content(h11);
    EXPECT_EQ(1, s1->content().size());
    EXPECT_FALSE(s1->is_registered(h11));
    EXPECT_TRUE(s1->is_registered(h12));
    auto response1 = http_get(h11->url());
    EXPECT_EQ(404, response1.status);
    auto response2 = http_get(h11->url());
    EXPECT_NE(200, response2.status);
    s1->wait_until_ready();
    // Check that the client restarted, i.e. it has no content.
    EXPECT_EQ(0, s1->content().size());
    EXPECT_FALSE(s2->is_registered(h21));
    EXPECT_FALSE(s2->is_registered(h22));
    // Also check it the other way round, since content() and is_registered()
    // both modify the internal set of content handles
    // when there are inconsistencies.
    EXPECT_FALSE(s2->is_registered(h21));
    EXPECT_FALSE(s2->is_registered(h22));
    EXPECT_EQ(0, s1->content().size());
}

TEST(SharedClient, CanBeConstructedWithMoveConstructor)
{
    auto client = create_client(false);
    SharedClient s1(client);
    s1.start();
    s1.wait_until_ready();
    SharedClient s2(std::move(s1));
    EXPECT_NO_THROW(EXPECT_FALSE(s2.wait_until_ready()));
    // Test the default move-assignment operator as well.
    SharedClient s3 = std::move(s2);
    EXPECT_NO_THROW(EXPECT_FALSE(s3.wait_until_ready()));
}
