#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <curl/curl.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "client.h"

using namespace loon;
using namespace testing;
using namespace std::chrono_literals;

#define TEST_ADDRESS "ws://127.0.0.1:8071/ws"
#define TEST_AUTH std::nullopt

class TestClient : public loon::ClientImpl
{
public:
    using ClientImpl::ClientImpl;

    inline bool send(ClientMessage const& message)
    {
        return ClientImpl::send(message);
    }

    inline size_t active_requests() { return ClientImpl::active_requests(); }

    inline Hello wait_for_hello() { return ClientImpl::wait_for_hello(); }

    inline void inject_hello_modifier(std::function<void(Hello&)> modifier)
    {
        ClientImpl::inject_hello_modifier(modifier);
    }

    inline void inject_send_error(bool trigger_error)
    {
        ClientImpl::inject_send_error(trigger_error);
    }

    inline void incoming_sleep(
        std::chrono::milliseconds duration = std::chrono::milliseconds::zero())
    {
        ClientImpl::incoming_sleep(duration);
    }

    inline void chunk_sleep(
        std::chrono::milliseconds duration = std::chrono::milliseconds::zero())
    {
        ClientImpl::chunk_sleep(duration);
    }

    inline void start_and_wait_until_connected()
    {
        EXPECT_FALSE(connected());
        start();
        wait_until_ready();
        EXPECT_TRUE(connected());
    }

    inline void restart_and_wait() { ClientImpl::restart_and_wait(); }

    inline bool connected() { return ClientImpl::connected(); }

    inline bool wait_until_ready() { return ClientImpl::wait_until_ready(); }

    inline bool wait_until_ready(std::chrono::milliseconds timeout)
    {
        return ClientImpl::wait_until_ready(timeout);
    }

    inline bool idling() { return ClientImpl::idling(); }
};

static std::shared_ptr<TestClient> create_client(
    ClientOptions options = {}, bool started = true)
{
    loon::client_log_level(loon::LogLevel::Debug);
    loon::websocket_log_level(loon::LogLevel::Debug);
    if (options.no_content_request_limit.has_value() &&
        options.no_content_request_limit->first == -1 &&
        options.no_content_request_limit->second == 0ms) {
        options.no_content_request_limit = std::make_pair(8, 1s);
    }
    if (!options.websocket.basic_authorization.has_value()) {
        options.websocket.basic_authorization = TEST_AUTH;
    }
    if (!options.websocket.ping_interval.has_value()) {
        options.websocket.ping_interval = 20ms;
    }
    auto client = std::make_shared<TestClient>(TEST_ADDRESS, options);
    if (started) {
        client->start();
        client->wait_until_ready();
    }
    return client;
}

static std::shared_ptr<TestClient> create_client(bool started)
{
    return create_client({}, started);
}

struct Content
{
    std::string path{};
    std::string content_type{};
    std::string data{};

    std::shared_ptr<loon::BufferContentSource> source{ nullptr };
    loon::ContentInfo info{};
};

static Content create_content(std::string const& path,
    std::string const& content_type, std::string const& content,
    std::optional<uint32_t> max_cache_duration = std::nullopt)
{
    Content result;
    result.path = path;
    result.content_type = content_type;
    result.data = content;
    std::vector<char> content_data(content.begin(), content.end());
    result.source =
        std::make_shared<loon::BufferContentSource>(content_data, content_type);
    result.info.path = path;
    result.info.max_cache_duration = max_cache_duration;
    return result;
}

static Content example_content(
    std::optional<uint32_t> max_cache_duration = std::nullopt,
    std::string const& path = "example.txt")
{
    return create_content(path, "text/plain", "test", max_cache_duration);
}

static Content example_content(std::string const& path,
    std::optional<uint32_t> max_cache_duration = std::nullopt)
{
    return create_content(path, "text/plain", "test", max_cache_duration);
}

static Content example_content_n(size_t n_bytes,
    std::string const& path = "example.txt",
    std::optional<uint32_t> max_cache_duration = std::nullopt)
{
    std::vector<char> data(n_bytes, 0);
    const std::string alphabet = "abcdefghijklmnopqrstuvwxyz";
    for (size_t i = 0; i < n_bytes; i++) {
        data[i] = alphabet[i % alphabet.size()];
    }
    std::string content(data.begin(), data.end());
    return create_content(path, "text/plain", content, max_cache_duration);
}

struct CurlWriteFunctionData
{
    std::string* result;
    std::function<void(std::string const&)> callback;
};

static size_t curl_receive_body(
    void* ptr, size_t size, size_t nmemb, CurlWriteFunctionData* data)
{
    data->result->append((char*)ptr, size * nmemb);
    if (data->callback) {
        data->callback(std::string((char*)ptr, size * nmemb));
    }
    return size * nmemb;
}

static size_t curl_receive_header(
    void* pData, size_t tSize, size_t tCount, void* pmUser)
{
    size_t length = tSize * tCount, index = 0;
    while (index < length) {
        unsigned char* temp = (unsigned char*)pData + index;
        if ((temp[0] == '\r') || (temp[0] == '\n'))
            break;
        index++;
    }
    std::string str((unsigned char*)pData, (unsigned char*)pData + index);
    std::map<std::string, std::string>* pmHeader =
        (std::map<std::string, std::string>*)pmUser;
    size_t pos = str.find(": ");
    if (pos != std::string::npos)
        pmHeader->insert(std::pair<std::string, std::string>(
            str.substr(0, pos), str.substr(pos + 2)));
    return (tCount);
}

struct CurlResponse
{
    long status;
    std::string body;
    std::map<std::string, std::string> headers;
};

struct CurlOptions
{
    // See https://curl.se/libcurl/c/CURLOPT_BUFFERSIZE.html
    static constexpr size_t MIN_BUFFER_SIZE{ 1024 };
    static constexpr size_t DEFAULT_BUFFER_SIZE{ MIN_BUFFER_SIZE };
    static constexpr size_t DEFAULT_TIMEOUT_MS{ 250 };

    size_t download_speed_bytes{ 0 };
    size_t download_buffer_size{ DEFAULT_BUFFER_SIZE };
    std::chrono::milliseconds timeout{ DEFAULT_TIMEOUT_MS };
    decltype(CurlWriteFunctionData::callback) callback{ nullptr };
};

CurlResponse http_get(std::string const& url, CurlOptions options = {})
{
    auto curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("failed to init curl");
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, options.timeout.count());
    curl_easy_setopt(
        curl, CURLOPT_MAX_RECV_SPEED_LARGE, options.download_speed_bytes);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, options.download_buffer_size);
    CurlResponse response{};
    CurlWriteFunctionData data{};
    data.result = &response.body;
    data.callback = options.callback;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_receive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_receive_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    auto result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        auto err = curl_easy_strerror(result);
        throw std::runtime_error(std::string("curl request failed: ") + err);
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_easy_cleanup(curl);
    return response;
}

class ExpectCalled
{
public:
    ExpectCalled(int n = 1) { EXPECT_CALL(*this, callback()).Times(n); }

    ~ExpectCalled()
    {
        // Give some time for the callback to be called.
        std::this_thread::sleep_for(25ms);
    }

    auto operator()() { wrap_callback(); }

    std::function<void()> get()
    {
        return [this] {
            wrap_callback();
        };
    }

    inline uint64_t count() const { return m_called.load(); }

private:
    void wrap_callback()
    {
        m_called += 1;
        callback();
    }

    MOCK_METHOD(void, callback, ());

    std::atomic<uint64_t> m_called{ 0 };
};

static inline std::chrono::system_clock::time_point time_now()
{
    return std::chrono::system_clock::now();
}

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
    client->on_failed(callback.get());
    client->start();
    // Wait for the Hello message to have been handled.
    // It's expected that the client is not connected anymore,
    // since the client should be in a failed state.
    EXPECT_THROW(client->wait_for_hello(), ClientNotConnectedException);
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
    bool done;
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
    client->wait_until_ready();
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
    EXPECT_THROW(TestClient(TEST_ADDRESS, options), std::exception);
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
    bool done;
    client->on_failed([&] {
        std::lock_guard lock(mutex);
        callback();
        cv.notify_one();
        done = true;
    });
    client->start();
    EXPECT_THROW(client->wait_for_hello(), ClientNotConnectedException);
    client->inject_hello_modifier([](Hello& hello) {
        // Increase the cache duration, so it won't fail again.
        hello.mutable_constraints()->set_cache_duration(30);
    });
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 2s, [&] {
            return done;
        });
    }
    client->on_failed([] {});
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
    std::this_thread::sleep_for(25ms);
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

#include "loon/shared_client.h"
#include "shared_client.h"

TEST(SharedReferenceCounter, AnyMethodThrowsWithNullPointer)
{
    loon::SharedReferenceCounter m;
    EXPECT_ANY_THROW(m.add(nullptr));
    EXPECT_ANY_THROW(m.remove(nullptr));
    EXPECT_ANY_THROW(m.count(nullptr));
}

TEST(SharedReferenceCounter, AddReturnsConsecutiveUniqueValues)
{
    loon::SharedReferenceCounter m;
    auto client = create_client(false);
    EXPECT_EQ(0, m.add(client));
    EXPECT_EQ(1, m.add(client));
    EXPECT_EQ(2, m.add(client));
    EXPECT_EQ(3, m.add(client));
    EXPECT_EQ(4, m.add(client));
}

TEST(SharedReferenceCounter, AddAfterRemoveContinuesToReturnConsecutiveValues)
{
    loon::SharedReferenceCounter m;
    auto client = create_client(false);
    EXPECT_EQ(0, m.add(client));
    EXPECT_EQ(1, m.add(client));
    EXPECT_EQ(2, m.add(client));
    m.remove(client);
    EXPECT_EQ(3, m.add(client));
    m.remove(client);
    EXPECT_EQ(4, m.add(client));
}

TEST(SharedReferenceCounter, CountReflectsTheCurrentReferenceCount)
{
    loon::SharedReferenceCounter m;
    auto client = create_client(false);
    for (size_t i = 0; i < 16; i++) {
        EXPECT_EQ(i, m.count(client));
        EXPECT_EQ(i, m.add(client));
        EXPECT_EQ(i + 1, m.count(client));
    }
}

TEST(SharedReferenceCounter, EqualAmountOfRemoveCallsResetsCountToZero)
{
    loon::SharedReferenceCounter m;
    for (size_t i = 1; i < 16; i++) {
        auto client = create_client(false);
        for (size_t j = 0; j < i; j++) {
            EXPECT_EQ(j, m.add(client));
        }
        for (size_t j = 0; j < i; j++) {
            m.remove(client);
            EXPECT_EQ(i - j - 1, m.count(client));
        }
    }
}

TEST(SharedReferenceCounter, RemovingNonExistentClientCausesDeath)
{
    {
        loon::SharedReferenceCounter m;
        auto client = create_client(false);
        EXPECT_DEATH(m.remove(client), "");
    }
    {
        loon::SharedReferenceCounter m;
        auto client = create_client(false);
        m.add(client);
        m.remove(client);
        EXPECT_DEATH(m.remove(client), "");
    }
}

TEST(SharedReferenceCounter, CountWithRequiredCausesDeathWhenThereIsNoClient)
{
    {
        loon::SharedReferenceCounter m;
        auto client = create_client(false);
        EXPECT_DEATH(m.count(client, true), "");
    }
    {
        loon::SharedReferenceCounter m;
        auto client = create_client(false);
        // This should not fail.
        m.count(client, false);
    }
}

TEST(SharedReferenceCounter, RemoveReturnsReferenceCountBeforeRemoveCall)
{
    loon::SharedReferenceCounter m;
    for (size_t i = 1; i < 16; i++) {
        auto client = create_client(false);
        for (size_t j = 0; j < i; j++) {
            EXPECT_EQ(j, m.add(client));
        }
        for (size_t j = 0; j < i; j++) {
            auto a = m.count(client);
            auto b = m.remove(client);
            auto c = m.count(client);
            EXPECT_EQ(a, b);
            EXPECT_EQ(a - 1, c);
        }
    }
}

class TestSharedClient : public loon::SharedClientImpl
{
public:
    using SharedClientImpl::SharedClientImpl;

    inline void before_manual_start_callback_sleep(
        std::chrono::milliseconds duration = std::chrono::milliseconds::zero())
    {
        SharedClientImpl::before_manual_start_callback_sleep(duration);
    }
};

class ClientCallCheck : public loon::IClient
{
public:
    ClientCallCheck(std::shared_ptr<IClient> client) : m_client{ client } {}

    inline size_t n_start() const { return m_n_start; }

    inline size_t n_stop() const { return m_n_stop; }

    inline size_t n_started() const { return m_n_started; }

    inline size_t n_idle() const { return m_n_idle; }

    inline size_t n_wait_until_ready() const { return m_n_wait_until_ready; }

    inline size_t n_wait_until_ready_timeout() const
    {
        return m_n_wait_until_ready_timeout;
    }

    // Overrides

    inline void start() override
    {
        m_n_start++;
        return m_client->start();
    }

    inline void stop() override
    {
        m_n_stop++;
        return m_client->stop();
    }

    inline void terminate() override { return m_client->terminate(); }

    inline bool started() override
    {
        m_n_started++;
        return m_client->started();
    }

    inline void idle() override
    {
        m_n_idle++;
        return m_client->idle();
    }

    inline bool wait_until_ready() override
    {
        m_n_wait_until_ready++;
        return m_client->wait_until_ready();
    }

    inline bool wait_until_ready(std::chrono::milliseconds timeout) override
    {
        m_n_wait_until_ready_timeout++;
        return m_client->wait_until_ready(timeout);
    }

    inline void on_ready(std::function<void()> callback) override
    {
        return m_client->on_ready(callback);
    }

    inline void on_disconnect(std::function<void()> callback) override
    {
        return m_client->on_disconnect(callback);
    }

    inline void on_failed(std::function<void()> callback) override
    {
        return m_client->on_failed(callback);
    }

    inline void unregister_content(
        std::shared_ptr<ContentHandle> handle) override
    {
        return m_client->unregister_content(handle);
    }

    inline std::vector<std::shared_ptr<ContentHandle>> content() override
    {
        return m_client->content();
    }

    inline bool is_registered(std::shared_ptr<ContentHandle> handle) override
    {
        return m_client->is_registered(handle);
    }

    inline std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info,
        std::chrono::milliseconds timeout) override
    {
        return m_client->register_content(source, info, timeout);
    }

    inline std::shared_ptr<ContentHandle> register_content(
        std::shared_ptr<loon::ContentSource> source,
        loon::ContentInfo const& info) override
    {
        return m_client->register_content(source, info);
    }

private:
    std::shared_ptr<IClient> m_client;

    size_t m_n_start{ 0 };
    size_t m_n_stop{ 0 };
    size_t m_n_started{ 0 };
    size_t m_n_idle{ 0 };
    size_t m_n_wait_until_ready{ 0 };
    size_t m_n_wait_until_ready_timeout{ 0 };
};

TEST(SharedClient, ConstructorThrowsWithSharedClientAsArgument)
{
    auto client = create_client(false);
    auto s = std::make_shared<SharedClient>(client);
    EXPECT_ANY_THROW(SharedClient(std::dynamic_pointer_cast<IClient>(s)));
}

TEST(SharedClient, CallingTerminateIsNotPossible)
{
    auto client = create_client(false);
    auto s = std::make_shared<SharedClient>(client);
    auto is = std::dynamic_pointer_cast<ISharedClient>(s);
    EXPECT_DEATH(is->terminate(), "");
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

TEST(SharedClient, IdleIsDelegatedWhenAllSharedClientsIdle)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    EXPECT_EQ(0, check->n_idle());
    s1->idle();
    EXPECT_EQ(0, check->n_idle());
    s2->idle();
    EXPECT_EQ(1, check->n_idle());
}

TEST(SharedClient, AClientThatIsStartedAgainDoesNotIdleAnymore)
{
    auto client = create_client(false);
    auto check = std::make_shared<ClientCallCheck>(client);
    auto s1 = std::make_shared<SharedClient>(check);
    auto s2 = std::make_shared<SharedClient>(check);
    s1->start();
    s2->start();
    s1->wait_until_ready();
    s2->wait_until_ready();
    EXPECT_EQ(0, check->n_idle());
    s1->idle();
    s1->start();
    // At this point the first client should not be idling anymore.
    EXPECT_EQ(0, check->n_idle());
    s2->idle();
    // Therefore idle is never delegated.
    EXPECT_EQ(0, check->n_idle());
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
    bool done;
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

// TODO repeated ready callback calls still work?
// TODO repeated disconnect callback calls still work?
// TODO failed callback?

// TODO tests for callbacks
