#include "loon/client.h"

#include <iostream>
#include <thread>

using namespace std::chrono_literals;

int main()
{
    const uint32_t cache_duration = 30;

    loon::ClientOptions options;
    options.min_cache_duration = (cache_duration * 4) / 5;
    options.max_requests_per_second = 1.0f;
    std::string address = "ws://127.0.0.1:80/ws";
    std::string auth = "loon-client:qadjB4GeRyUSEjbj6ZFWwOiDtjLq";
    loon::Client client(address, auth, options);
    client.start();

    std::string content = "<h1>It works!";
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

    std::cout << "URL: " << handle->url() << "\n";

    std::this_thread::sleep_for(2s);

    std::cout << "Unregistering content" << std::endl;
    client.unregister_content(handle);
    std::this_thread::sleep_for(1s);

    std::cout << "Stopping client" << std::endl;
    client.stop();
    std::this_thread::sleep_for(1s);

    // for (size_t i = 0; i < 32000; i++) {
    //     content += std::string("\n\n") + "okay";
    // }
}
