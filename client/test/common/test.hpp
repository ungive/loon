#pragma once

#include <atomic>
#include <chrono>
#include <ctime>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include <curl/curl.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "client.hpp"
#include "loon/client.hpp"
#include "shared_client.hpp"

using namespace loon;
using namespace testing;
using namespace std::chrono_literals;

#define TEST_SERVER_HOST "127.0.0.1:8072"
#define TEST_SERVER_WS "ws://" TEST_SERVER_HOST "/proxy/ws"
#define TEST_SERVER_DROP_ALL "http://" TEST_SERVER_HOST "/proxy/drop_all"
#define TEST_SERVER_DROP_ACTIVE "http://" TEST_SERVER_HOST "/proxy/drop_active"
#define TEST_SERVER_DROP_ACTIVE_IN \
    "http://" TEST_SERVER_HOST "/proxy/drop_active_in"
#define TEST_SERVER_DROP_ACTIVE_OUT \
    "http://" TEST_SERVER_HOST "/proxy/drop_active_out"
#define TEST_SERVER_DROP_NONE "http://" TEST_SERVER_HOST "/proxy/drop_none"
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

    inline void inject_hello_modifier(
        std::function<void(std::optional<Hello>&)> modifier)
    {
        ClientImpl::inject_hello_modifier(modifier);
    }

    inline void inject_send_error(bool trigger_error)
    {
        ClientImpl::inject_send_error(trigger_error);
    }

    inline void send_sleep(
        std::chrono::milliseconds duration = std::chrono::milliseconds::zero())
    {
        ClientImpl::send_sleep(duration);
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

    inline void initial_reconnect_sleep(
        std::chrono::milliseconds duration = std::chrono::milliseconds::zero())
    {
        ClientImpl::initial_reconnect_sleep(duration);
    }

    inline void before_reconnect_callback(std::function<void()> callback)
    {
        ClientImpl::before_reconnect_callback(std::move(callback));
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
        options.websocket.ping_interval = 25ms;
    }
    auto client = std::make_shared<TestClient>(TEST_SERVER_WS, options);
    if (started) {
        client->start();
        client->wait_until_ready();
        EXPECT_TRUE(client->connected());
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

static CurlResponse http_get(std::string const& url, CurlOptions options = {})
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

    inline size_t n_idle_true() const { return m_n_idle_true; }

    inline size_t n_idle_false() const { return m_n_idle_false; }

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

    inline bool started() override
    {
        m_n_started++;
        return m_client->started();
    }

    inline void idle(bool state) override
    {
        if (state) {
            m_n_idle_true++;
        } else {
            m_n_idle_false++;
        }
        return m_client->idle(state);
    }

    inline void idle() override
    {
        m_n_idle_true++;
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
    size_t m_n_idle_true{ 0 };
    size_t m_n_idle_false{ 0 };
    size_t m_n_wait_until_ready{ 0 };
    size_t m_n_wait_until_ready_timeout{ 0 };
};

enum DropPackets
{
    None,
    Active,
    ActiveIn,
    ActiveOut,
    All
};

// Turns dropping of websocket server packets on/off. Packets are dropped until
// the next websocket/client connection is established.
inline void drop_server_packets(DropPackets choice)
{
    std::optional<CurlResponse> response;
    switch (choice) {
    case None:
        response = http_get(TEST_SERVER_DROP_NONE);
        break;
    case Active:
        response = http_get(TEST_SERVER_DROP_ACTIVE);
        break;
    case ActiveIn:
        response = http_get(TEST_SERVER_DROP_ACTIVE_IN);
        break;
    case ActiveOut:
        response = http_get(TEST_SERVER_DROP_ACTIVE_OUT);
        break;
    case All:
        response = http_get(TEST_SERVER_DROP_ALL);
        break;
    default:
        throw std::invalid_argument("Unrecognized packets choice");
    }
    EXPECT_EQ(200, response->status);
}

inline void drop_server_packets(bool state)
{
    if (state) {
        drop_server_packets(Active);
    } else {
        drop_server_packets(None);
    }
}
