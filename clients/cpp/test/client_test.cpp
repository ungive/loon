#include <atomic>
#include <chrono>
#include <map>
#include <memory>
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

    inline Hello current_hello() { return ClientImpl::current_hello(); }

    inline void inject_hello_modifier(std::function<void(Hello&)> modifier)
    {
        ClientImpl::inject_hello_modifier(modifier);
    }

    inline void chunk_sleep(
        std::chrono::milliseconds duration = std::chrono::milliseconds::zero())
    {
        ClientImpl::chunk_sleep(duration);
    }
};

static std::shared_ptr<TestClient> create_client(
    ClientOptions options = {}, bool started = true)
{
    auto client =
        std::make_shared<TestClient>(TEST_ADDRESS, TEST_AUTH, options);
    if (started) {
        client->start();
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
    std::string const& content_type,
    std::string const& content,
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

static Content example_content_large(size_t n_bytes,
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

    auto operator()() { wrap_callback(); }

    std::function<void()> get()
    {
        return [this] {
            wrap_callback();
        };
    }

    inline bool was_called() const { return m_called.load(); }

private:
    void wrap_callback()
    {
        m_called.exchange(true);
        callback();
    }

    MOCK_METHOD(void, callback, ());

    std::atomic<bool> m_called{ false };
};

TEST(Client, _server_serves_content_when_registered_with_client)
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
    EXPECT_EQ("max-age=" + std::to_string(cache_duration),
        response.headers["Cache-Control"]);
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
    handle->unregistered(callback.get());
    client->unregister_content(handle);
}

TEST(Client, UnregisteredCallbackIsCalledWhenSetAfterUnregistering)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    client->unregister_content(handle);
    ExpectCalled callback;
    handle->unregistered(callback.get());
}

TEST(Client, UnregisteredCallbackIsCalledWhenServerClosesConnection)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled callback;
    handle->unregistered(callback.get());
    // Trigger connection close with an invalid message.
    loon::ClientMessage message;
    auto empty_response = message.mutable_empty_response();
    empty_response->set_request_id(1000);
    client->send(message);
    std::this_thread::sleep_for(250ms);
    EXPECT_TRUE(callback.was_called());
}

TEST(Client, UnregisteredCallbackIsCalledWhenClientClosesConnection)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled callback;
    handle->unregistered(callback.get());
    client->stop();
    EXPECT_TRUE(callback.was_called());
}

TEST(Client, ServedCallbackIsCalledWhenContentHandleUrlIsRequested)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled callback;
    handle->served(callback.get());
    http_get(handle->url());
    EXPECT_TRUE(callback.was_called());
}

TEST(Client, NoActiveRequestsWhenHandleUrlRequestIsCanceled)
{
    auto client = create_client();
    auto hello = client->current_hello();
    auto chunk_size = hello.constraints().chunk_size();
    auto content = example_content_large(3 * chunk_size);
    client->chunk_sleep(25ms); // 3 chunks => 75ms to send everything
    auto handle = client->register_content(content.source, content.info);
    CurlOptions options{};
    // Force a send failure by timing out before everything is sent.
    options.timeout = 60ms;
    options.callback = [&client](std::string const& chunk) {
        EXPECT_EQ(1, client->active_requests());
    };
    EXPECT_EQ(0, client->active_requests());
    EXPECT_THROW(http_get(handle->url(), options), std::exception);
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(0, client->active_requests());
}

TEST(Client, FailsWhenMinCacheDurationIsSetAndServerDoesNotCacheResponses)
{
    ClientOptions options;
    options.min_cache_duration = 10;
    auto client = create_client(options, false);
    client->inject_hello_modifier([](Hello& hello) {
        hello.mutable_constraints()->set_response_caching(false);
    });
    ExpectCalled callback;
    client->failed(callback.get());
    client->start();
    // Wait for the Hello message to have been handled.
    // It's expected that the client is not connected anymore,
    // since the client should be in a failed state.
    EXPECT_THROW(client->current_hello(), ClientNotConnectedException);
    EXPECT_TRUE(callback.was_called());
}

TEST(Client, FailsWhenMinCacheDurationIsSetButResponseIsNotCached)
{
    uint32_t cache_duration = 10;
    ClientOptions options;
    options.min_cache_duration = cache_duration / 2;
    auto client = create_client(options, false);
    client->inject_hello_modifier([](Hello& hello) {
        if (hello.constraints().response_caching()) {
            FAIL() << "the test server is expected to not cache responses";
        }
        // The real test server does not cache responses,
        // but for the sake of the test, we pretend it does.
        // This would resemble a server that claims to cache, but doesn't.
        hello.mutable_constraints()->set_response_caching(true);
    });
    ExpectCalled failed;
    client->failed(failed.get());
    client->start();
    auto content1 = example_content(cache_duration);
    auto handle = client->register_content(content1.source, content1.info);
    // The served callback should only be called once,
    // since it is expected to be cached on the server.
    ExpectCalled served(1);
    handle->served(served.get());
    http_get(handle->url());
    http_get(handle->url());
}
