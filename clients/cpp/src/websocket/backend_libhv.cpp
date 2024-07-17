#include <hv/WebSocketClient.h>

#include "client.h"
#include "util.h"

#define RECONNECT_DELAY_POLICY_FIXED 0
#define RECONNECT_DELAY_POLICY_LINEAR 1
#define RECONNECT_DELAY_POLICY_EXPONENTIAL 2

#define DEFAULT_RECONNECT_INCREASING_DELAY_POLICY \
    RECONNECT_DELAY_POLICY_EXPONENTIAL

using WebsocketOptions = loon::WebsocketOptions;
using namespace loon::websocket;

std::chrono::milliseconds loon::websocket::default_connect_timeout =
    std::chrono::milliseconds{ HIO_DEFAULT_CONNECT_TIMEOUT };

class ClientImpl : public BaseClient
{
public:
    ClientImpl(std::string const& address, WebsocketOptions const& options);

    size_t send_binary(const char* data, size_t length) override;
    size_t send_text(const char* data, size_t length) override;

protected:
    void internal_start() override;
    void internal_stop() override;

private:
    hv::WebSocketClient m_conn{};
};

Client::Client(std::string const& address, WebsocketOptions const& options)
    : m_impl{ std::make_unique<ClientImpl>(address, options) }
{
    if (options.ca_certificate.has_value()) {
        // Open issue: https://github.com/ithewei/libhv/issues/586
        throw std::runtime_error(
            "in-memory CA certificates are not supported with libhv yet");
    }
}

ClientImpl::ClientImpl(
    std::string const& address, WebsocketOptions const& options)
    : BaseClient(address, options)
{
    m_conn.onopen = std::bind(&ClientImpl::on_websocket_open, this);
    m_conn.onmessage = std::bind(
        &ClientImpl::on_websocket_message, this, std::placeholders::_1);
    m_conn.onclose = std::bind(&ClientImpl::on_websocket_close, this);
    if (m_options.connect_timeout.has_value()) {
        m_conn.setConnectTimeout(m_options.connect_timeout.value().count());
    } else {
        m_conn.setConnectTimeout(default_connect_timeout.count());
    }
    if (m_options.ping_interval.has_value()) {
        m_conn.setPingInterval(m_options.ping_interval.value().count());
    }
    // Logging
    // TODO Allow the caller to configure this.
    hlog_set_level(LOG_LEVEL_DEBUG);
    hlog_set_handler(stderr_logger);
}

void ClientImpl::internal_start()
{
    // Reconnect
    if (m_options.reconnect_delay.has_value()) {
        reconn_setting_t reconnect;
        reconn_setting_init(&reconnect);
        reconnect.min_delay = m_options.reconnect_delay.value().count();
        if (m_options.max_reconnect_delay.has_value()) {
            reconnect.max_delay = m_options.max_reconnect_delay.value().count();
            reconnect.delay_policy = DEFAULT_RECONNECT_INCREASING_DELAY_POLICY;
        }
        m_conn.setReconnect(&reconnect);
    }
    // Headers
    http_headers headers;
    for (auto const& [key, value] : m_options.headers) {
        headers[key] = value;
    }
    if (m_options.basic_authorization.has_value()) {
        auto credentials = m_options.basic_authorization.value();
        headers["Authorization"] = "Basic " + util::base64_encode(credentials);
    }
    // Server verification (CA certificate)
    bool is_wss = m_address.rfind("wss", 0) == 0;
    if (is_wss && m_options.ca_certificate_path.has_value()) {
        hssl_ctx_opt_t param{};
        param.endpoint = HSSL_CLIENT;
        param.verify_peer = 1;
        param.ca_file = m_options.ca_certificate_path.value().c_str();
        m_conn.withTLS(&param);
    }
    // Open the connection
    m_conn.open(m_address.c_str(), headers);
}

void ClientImpl::internal_stop()
{
    m_conn.setReconnect(nullptr);
    m_conn.close();
}

size_t ClientImpl::send_binary(const char* data, size_t length)
{
    return m_conn.send(data, length, WS_OPCODE_BINARY);
}

size_t ClientImpl::send_text(const char* data, size_t length)
{
    return m_conn.send(data, length, WS_OPCODE_TEXT);
}
