#include <iostream>
#include <thread>

using namespace std::chrono_literals;

#include <google/protobuf/text_format.h>
#include <hv/WebSocketClient.h>
#include <hv/base64.h>

#include "loon.h"

int main()
{
    std::string address = "ws://127.0.0.1:80/ws";
    std::string auth = "loon-client:qadjB4GeRyUSEjbj6ZFWwOiDtjLq";
    loon::Client client(address, auth);
    // loon::Client client("ws://127.0.0.1:8080/ws", std::nullopt);
    client.start();

    // Wait for the Hello message to arrive.
    std::this_thread::sleep_for(250ms);

    std::string content = "<h1>It works!";
    // for (size_t i = 0; i < 32000; i++) {
    //     content += std::string("\n\n") + "okay";
    // }
    std::string content_type = "text/html";
    std::vector<char> content_data(content.begin(), content.end());
    auto content_source =
        std::make_shared<loon::BufferContentSource>(content_data, content_type);
    auto content_info = loon::ContentInfo("index.html", 10);
    auto handle = client.register_content(content_source, content_info);

    std::cout << "URL: " << handle->url() << "\n";

    std::this_thread::sleep_for(120s);
    client.stop();
}
