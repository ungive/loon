#include <atomic>
#include <sstream>

#include <hv/WebSocketClient.h>

#include "client.h"
#include "logging.h"
#include "util.h"

#define RECONNECT_DELAY_POLICY_FIXED 0
#define RECONNECT_DELAY_POLICY_LINEAR 1
#define RECONNECT_DELAY_POLICY_EXPONENTIAL 2

#define DEFAULT_RECONNECT_INCREASING_DELAY_POLICY \
    RECONNECT_DELAY_POLICY_EXPONENTIAL

using namespace loon::websocket;

using WebsocketOptions = loon::WebsocketOptions;

std::chrono::milliseconds loon::websocket::default_connect_timeout =
    std::chrono::milliseconds{ HIO_DEFAULT_CONNECT_TIMEOUT };

class ClientImpl : public BaseClient
{
public:
    ClientImpl(std::string const& address, WebsocketOptions const& options);

    int64_t send_binary(const char* data, size_t length) override;
    int64_t send_text(const char* data, size_t length) override;

protected:
    void internal_start() override;
    void internal_stop() override;

    std::unique_ptr<hv::WebSocketClient> create_conn();

private:
    std::unique_ptr<hv::WebSocketClient> m_conn{};
};

static constexpr int loon_to_libhv_level(loon::LogLevel level);
static constexpr loon::LogLevel libhv_to_loon_level(int level);
void libhv_log_handler(int level, const char* buf, int len);

static std::mutex _mutex{};
static std::atomic<int> _level{ loon_to_libhv_level(default_log_level) };
static loon::log_handler_t _handler{ loon::default_log_handler };

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
    : BaseClient(address, options), m_conn{ create_conn() }
{

    // It should be okay to set a global log handler,
    // as the libhv library is being statically linked.
    hlog_set_format("%s");
    hlog_set_handler(libhv_log_handler);
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        hlog_set_level(_level);
    }
}

std::unique_ptr<hv::WebSocketClient> ClientImpl::create_conn()
{
    auto conn = std::make_unique<hv::WebSocketClient>();
    conn->onopen = std::bind(&ClientImpl::on_websocket_open, this);
    conn->onmessage = std::bind(
        &ClientImpl::on_websocket_message, this, std::placeholders::_1);
    conn->onclose = std::bind(&ClientImpl::on_websocket_close, this);
    if (m_options.connect_timeout.has_value()) {
        conn->setConnectTimeout(m_options.connect_timeout.value().count());
    } else {
        conn->setConnectTimeout(default_connect_timeout.count());
    }
    if (m_options.ping_interval.has_value()) {
        conn->setPingInterval(m_options.ping_interval.value().count());
    }
    return conn;
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
        m_conn->setReconnect(&reconnect);
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
        m_conn->withTLS(&param);
    }
    // Open the connection
    m_conn->open(m_address.c_str(), headers);
}

void ClientImpl::internal_stop()
{
    m_conn->stop();
    m_conn.reset();
    m_conn = create_conn();
}

int64_t ClientImpl::send_binary(const char* data, size_t length)
{
    return m_conn->send(data, length, WS_OPCODE_BINARY);
}

int64_t ClientImpl::send_text(const char* data, size_t length)
{
    return m_conn->send(data, length, WS_OPCODE_TEXT);
}

static void libhv_log_handler(int level, const char* buf, int len)
{
    // Logger
    decltype(_handler) handler;
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!_handler) {
            return;
        }
        handler = _handler;
    }
    // Message
    std::ostringstream oss;
    oss << "libhv: ";
    if (len > 0 && buf[len - 1] == '\n') {
        // Strip a possible linefeed at the end.
        len -= 1;
    }
    oss.write(buf, len);
    auto converted_message = oss.str();
    // Log
    loon::LogLevel converted_level = libhv_to_loon_level(level);
    handler(converted_level, converted_message);
}

void loon::websocket::log_level(LogLevel level)
{
    auto converted_level = loon_to_libhv_level(level);
    hlog_set_level(converted_level);
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _level = converted_level;
    }
}

void loon::websocket::log_handler(log_handler_t handler)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    _handler = handler;
}

static constexpr int loon_to_libhv_level(loon::LogLevel level)
{
    switch (level) {
    case loon::LogLevel::Debug:
        return LOG_LEVEL_DEBUG;
    case loon::LogLevel::Info:
        return LOG_LEVEL_INFO;
    case loon::LogLevel::Warning:
        return LOG_LEVEL_WARN;
    case loon::LogLevel::Error:
        return LOG_LEVEL_ERROR;
    case loon::LogLevel::Fatal:
        return LOG_LEVEL_FATAL;
    default:
        assert(false);
        return LOG_LEVEL_INFO;
    }
}

static constexpr loon::LogLevel libhv_to_loon_level(int level)
{
    switch (level) {
    case LOG_LEVEL_VERBOSE:
    case LOG_LEVEL_DEBUG:
        return loon::LogLevel::Debug;
        break;
    case LOG_LEVEL_SILENT:
        assert(false); // fallthrough, treat silent as info
    case LOG_LEVEL_INFO:
        return loon::LogLevel::Info;
        break;
    case LOG_LEVEL_WARN:
        return loon::LogLevel::Warning;
        break;
    case LOG_LEVEL_ERROR:
        return loon::LogLevel::Error;
        break;
    case LOG_LEVEL_FATAL:
        // If there is a fatal websocket error, it's also a fatal loon error,
        // since the client cannot possibly be connected to the server.
        return loon::LogLevel::Fatal;
        break;
    default:
        assert(false);
        return loon::LogLevel::Warning;
    }
}
