#include <atomic>
#include <sstream>

#include <hv/WebSocketClient.h>

#include "client.h"
#include "logging.h"
#include "util.h"

#define BACKEND_NAME "libhv"
#define LOG_PREFIX BACKEND_NAME ": "

#define RECONNECT_DELAY_POLICY_FIXED 0
#define RECONNECT_DELAY_POLICY_LINEAR 1
#define RECONNECT_DELAY_POLICY_EXPONENTIAL 2

#define DEFAULT_RECONNECT_INCREASING_DELAY_POLICY \
    RECONNECT_DELAY_POLICY_EXPONENTIAL

using namespace loon::websocket;

using WebsocketOptions = loon::WebsocketOptions;

std::chrono::milliseconds loon::websocket::default_connect_timeout =
    std::chrono::milliseconds{ HIO_DEFAULT_CONNECT_TIMEOUT };

std::chrono::milliseconds loon::websocket::default_ping_interval =
    std::chrono::milliseconds{ 20000 };

class ClientImpl : public BaseClient
{
public:
    ClientImpl(std::string const& address, WebsocketOptions const& options);
    ~ClientImpl();

    int64_t send_binary(const char* data, size_t length) override;
    int64_t send_text(const char* data, size_t length) override;

protected:
    void internal_start() override;
    void internal_stop() override;
    void internal_terminate() override;

private:
    std::unique_ptr<hv::WebSocketClient> create_conn();

    std::unique_ptr<hv::WebSocketClient> m_conn{};
};

static constexpr int loon_to_libhv_level(loon::LogLevel level);
static constexpr loon::LogLevel libhv_to_loon_level(int level);
static void libhv_log_handler(int level, const char* buf, int len);

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

Client::~Client() {}

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

ClientImpl::~ClientImpl()
{
    // TODO: the destructor should not allocate a new object,
    //   which is what internal_stop() does.
    //   move construction/reconstruction into internal_start().
    internal_terminate();
}

std::unique_ptr<hv::WebSocketClient> ClientImpl::create_conn()
{
    auto conn = std::make_unique<hv::WebSocketClient>();
    conn->onopen = std::bind(&ClientImpl::on_websocket_open, this);
    conn->onmessage = std::bind(
        &ClientImpl::on_websocket_message, this, std::placeholders::_1);
    conn->onclose = std::bind(&ClientImpl::on_websocket_close, this);
    if (options().connect_timeout.has_value()) {
        conn->setConnectTimeout(options().connect_timeout.value().count());
    } else {
        conn->setConnectTimeout(default_connect_timeout.count());
    }
    // Note: libhv uses the ping interval as the ping timeout, see
    // https://github.com/ithewei/libhv/blob/0cfc3c16/http/server/HttpHandler.cpp#L203-L215
    conn->setPingInterval(
        options().ping_interval.value_or(default_ping_interval).count());
    return conn;
}

void ClientImpl::internal_start()
{
    // Reconnect
    if (options().reconnect_delay.has_value()) {
        reconn_setting_t reconnect;
        reconn_setting_init(&reconnect);
        reconnect.min_delay = options().reconnect_delay.value().count();
        if (options().max_reconnect_delay.has_value()) {
            reconnect.max_delay = options().max_reconnect_delay.value().count();
            reconnect.delay_policy = DEFAULT_RECONNECT_INCREASING_DELAY_POLICY;
        }
        m_conn->setReconnect(&reconnect);
    }
    // Headers
    http_headers headers;
    // Default user-agent. The map is case-insensitive,
    // so it will be overwritten, if set by the user.
    headers["User-Agent"] = LOON_USER_AGENT;
    for (auto const& [key, value] : options().headers) {
        headers[key] = value;
    }
    if (options().basic_authorization.has_value()) {
        auto credentials = options().basic_authorization.value();
        headers["Authorization"] = "Basic " + util::base64_encode(credentials);
    }
    // Server verification (CA certificate)
    bool is_wss = address().rfind("wss", 0) == 0;
    if (is_wss && options().ca_certificate_path.has_value()) {
        hssl_ctx_opt_t param{};
        param.endpoint = HSSL_CLIENT;
        param.verify_peer = 1;
        param.ca_file = options().ca_certificate_path.value().c_str();
        m_conn->withTLS(&param);
    }
    // Open the connection
    m_conn->open(address().c_str(), headers);
}

void ClientImpl::internal_stop()
{
    m_conn->stop();
    m_conn.reset();
    m_conn = create_conn();
}

void ClientImpl::internal_terminate()
{
    if (m_conn != nullptr) {
        m_conn->stop();
        m_conn = nullptr;
    }
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
    oss << LOG_PREFIX;
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
    if (!handler) {
        handler = default_log_handler;
    }
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
