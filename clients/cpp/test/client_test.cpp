#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

// #include <cpr/cpr.h>
#include <curl/curl.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "client.h"

using namespace loon;
using namespace testing;
using namespace std::chrono_literals;

#define TEST_ADDRESS "ws://127.0.0.1:80/ws"
#define TEST_AUTH "loon-client:qadjB4GeRyUSEjbj6ZFWwOiDtjLq"

class TestClient : public loon::ClientImpl
{
public:
    using ClientImpl::ClientImpl;

    bool send(ClientMessage const& message)
    {
        return ClientImpl::send(message);
    }
};

static std::shared_ptr<TestClient> create_client(bool started = true)
{
    auto client = std::make_shared<TestClient>(TEST_ADDRESS, TEST_AUTH);
    if (started) {
        client->start();
    }
    return client;
}

struct Content
{
    std::string path;
    std::string content_type;
    std::string data;

    std::shared_ptr<loon::BufferContentSource> source;
    loon::ContentInfo info;
};

static Content create_content(std::string const& path,
    std::string const& content_type,
    std::string const& content)
{
    Content result;
    result.path = path;
    result.content_type = content_type;
    result.data = content;
    std::vector<char> content_data(content.begin(), content.end());
    result.source =
        std::make_shared<loon::BufferContentSource>(content_data, content_type);
    result.info = loon::ContentInfo(path, 0);
    return result;
}

static Content example_content()
{
    return create_content("example.txt", "text/html", "test");
}

static size_t curl_receive_body(
    void* ptr, size_t size, size_t nmemb, std::string* data)
{
    data->append((char*)ptr, size * nmemb);
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

#define HTTP_GET_TIMEOUT_MS 500

CurlResponse http_get(std::string const& url)
{
    auto curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("failed to init curl");
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, HTTP_GET_TIMEOUT_MS);
    CurlResponse response{};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_receive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
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
    ExpectCalled() { EXPECT_CALL(*this, callback()); }

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

TEST(Client, server_serves_content_when_registered_with_client)
{
    std::string path = "index.html";
    uint32_t cache_duration = 23;
    std::string filename = "page.html";
    std::string content = "<h1>It works!";
    std::string content_type = "text/html";

    auto client = create_client();
    auto handle = client->register_content(
        std::make_shared<loon::BufferContentSource>(
            std::vector<char>(content.begin(), content.end()), content_type),
        loon::ContentInfo(path, cache_duration, filename));
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

TEST(Client, unregistered_callback_is_called_when_unregistering)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled callback;
    handle->unregistered(callback.get());
    client->unregister_content(handle);
}

TEST(Client, unregistered_callback_is_called_when_set_after_unregistering)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    client->unregister_content(handle);
    ExpectCalled callback;
    handle->unregistered(callback.get());
}

TEST(Client, unregistered_callback_is_called_when_server_closes_connection)
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

TEST(Client, unregistered_callback_is_called_when_client_closes_connection)
{
    auto client = create_client();
    auto content = example_content();
    auto handle = client->register_content(content.source, content.info);
    ExpectCalled callback;
    handle->unregistered(callback.get());
    client->stop();
    EXPECT_TRUE(callback.was_called());
}
