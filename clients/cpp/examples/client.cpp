#include "loon/client.h"

#include <iostream>
#include <sstream>
#include <thread>

using namespace std::chrono_literals;

int main()
{
    const uint32_t cache_duration = 30;

    // std::string address = "wss://echo.websocket.org";
    // options.min_cache_duration = (cache_duration * 4) / 5;
    // options.max_requests_per_second = 1.0f;

    loon::client_log_level(loon::LogLevel::Debug);
    loon::websocket_log_level(loon::LogLevel::Warning);

    std::string address = "ws://localhost:8071/ws";
    loon::ClientOptions options;
    // options.websocket.basic_authorization =
    //     "loon-client:qadjB4GeRyUSEjbj6ZFWwOiDtjLq";
    // options.websocket.ca_certificate_path =
    //     "W:\\dev\\projects\\loon\\deployments\\cert.pem";
    options.websocket.connect_timeout = 5000ms;
    options.websocket.ping_interval = 20000ms;
    options.websocket.reconnect_delay = 1000ms;
    // options.websocket.max_reconnect_delay = 30000ms;
    options.no_content_request_limit = std::make_pair(16, 1s);
    loon::Client client(address, options);
    client.start();
    if (!client.wait_until_connected(20s)) {
        std::cerr << "Failed to connect, exiting\n";
        return -1;
    }

    std::ostringstream oss;
    oss << "<h1>It works!</h1><br>";
    for (size_t i = 0; i < 1000; i++) {
        oss << "<p>This is such an interesting thing to read!</p>";
    }

    // std::string content = "<h1>It works!";
    std::string content = oss.str();
    std::string content_type = "text/html";
    std::vector<char> content_data(content.begin(), content.end());
    auto content_source =
        std::make_shared<loon::BufferContentSource>(content_data, content_type);
    loon::ContentInfo content_info;
    content_info.path = "index.html";
    content_info.max_cache_duration = cache_duration;
    auto handle = client.register_content(content_source, content_info);

    handle->unregistered([] {
        std::cout << "unregistered\n";
    });

    std::cout << "URL: " << handle->url() << std::endl;

    std::this_thread::sleep_for(30s);

    std::cout << "Unregistering content" << std::endl;
    client.unregister_content(handle);
    std::this_thread::sleep_for(1s);

    std::cout << "Stopping client" << std::endl;
    client.stop();
    std::this_thread::sleep_for(1s);
    std::cout << "Exiting" << std::endl;

    // for (size_t i = 0; i < 32000; i++) {
    //     content += std::string("\n\n") + "okay";
    // }
}
