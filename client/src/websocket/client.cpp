#include "client.hpp"

using namespace loon::websocket;

BaseClient::BaseClient(
    std::string const& address, WebsocketOptions const& options)
    : m_address{ address }, m_options{ options }
{
    if (options.ca_certificate.has_value() &&
        options.ca_certificate_path.has_value()) {
        throw std::runtime_error(
            "only one of ca_certificate and ca_certificate_path may be set");
    }
    if (options.connect_timeout.has_value() &&
        options.connect_timeout.value() <= std::chrono::milliseconds::zero()) {
        throw std::runtime_error(
            "the connect_timeout must be greater than zero");
    }
    if (options.ping_interval.has_value() &&
        options.ping_interval.value() <= std::chrono::milliseconds::zero()) {
        throw std::runtime_error("the ping_interval must be greater than zero");
    }
}

BaseClient::~BaseClient() {}

void BaseClient::on_open(std::function<void()> callback)
{
    if (m_active.load()) {
        throw std::runtime_error("websocket client already started");
    }
    m_open_callback = callback;
}

void BaseClient::on_close(std::function<void()> callback)
{
    if (m_active.load()) {
        throw std::runtime_error("websocket client already started");
    }
    m_close_callback = callback;
}

void BaseClient::on_message(
    std::function<void(std::string const& message)> callback)
{
    if (m_active.load()) {
        throw std::runtime_error("websocket client already started");
    }
    m_message_callback = callback;
}

void BaseClient::start()
{
    if (m_active.exchange(true)) {
        return;
    }
    internal_start();
}

void BaseClient::stop()
{
    if (!m_active.exchange(false)) {
        return;
    }
    internal_stop();
}
