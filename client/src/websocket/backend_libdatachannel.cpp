#include <cassert>
#include <chrono>
#include <functional>
#include <optional>

#include <rtc/global.hpp>
#include <rtc/websocket.hpp>

#include "base64.hpp"
#include "client.hpp"
#include "logging.hpp"

#define BACKEND_NAME "libdatachannel"
#define LOG_PREFIX BACKEND_NAME ": "

#define var loon::log_var
#define log(level) \
    loon_log_macro(level, g_log_level.load(), ::log_message, loon::Logger)

static constexpr rtc::LogLevel loon_to_rtc_level(loon::LogLevel level);
static constexpr loon::LogLevel rtc_to_loon_level(rtc::LogLevel level);
static void log_message(loon::LogLevel level, std::string const& message);
static void rtc_log_handler(rtc::LogLevel level, std::string message);

using namespace loon::websocket;

static std::mutex g_log_mutex{};
static std::atomic<loon::LogLevel> g_log_level{
    loon::websocket::default_log_level
};
static loon::log_handler_t g_log_handler{ loon::default_log_handler };

using WebsocketOptions = loon::WebsocketOptions;

std::chrono::milliseconds loon::websocket::default_connect_timeout =
    std::chrono::milliseconds{ 10000 };

std::chrono::milliseconds loon::websocket::default_ping_interval =
    std::chrono::milliseconds{ 20000 };

// TODO Consider reducing the libdatachannel thread pool size? how large is it?
// https://github.com/paullouisageneau/libdatachannel/pull/1486

class ClientImpl : public BaseClient
{
public:
    ClientImpl(std::string const& address, WebsocketOptions const& options);
    ~ClientImpl();

    int64_t send_binary(const char* data, size_t length) override;
    int64_t send_text(const char* data, size_t length) override;

protected:
    void on_connected();
    void on_disconnected();
    void on_binary_message_received(rtc::binary message);
    void on_text_message_received(rtc::string message);

    void internal_start() override;
    void internal_stop() override;

private:
    std::unique_ptr<rtc::WebSocket> create_conn();

    // FIXME There's a lot of levels of indirection here. Use optional?
    std::unique_ptr<rtc::WebSocket> m_conn{};
};

Client::Client(std::string const& address, WebsocketOptions const& options)
    : m_impl{ std::make_unique<ClientImpl>(address, options) }
{
}

Client::~Client() {}

ClientImpl::ClientImpl(
    std::string const& address, WebsocketOptions const& options)
    : BaseClient(address, options), m_conn{ create_conn() }
{
}

ClientImpl::~ClientImpl() { stop(); }

std::unique_ptr<rtc::WebSocket> ClientImpl::create_conn()
{
    rtc::WebSocket::Configuration configuration;
    configuration.disableTlsVerification = options().disable_tls_verification;
    // For now we stick with 1 max outstanding ping, since the documentation for
    // "ping_interval" says "the ping interval is used as the pong timeout".
    configuration.maxOutstandingPings = 1;
    configuration.connectionTimeout =
        options().connect_timeout.value_or(default_connect_timeout);
    configuration.pingInterval =
        options().ping_interval.value_or(default_ping_interval);
    if (options().ca_certificate.has_value()) {
        // This configuration option also accepts in-memory certificate strings:
        // https://github.com/paullouisageneau/libdatachannel/blob/v0.23/src/impl/websocket.cpp#L48
        configuration.caCertificatePemFile = *options().ca_certificate;
    } else if (options().ca_certificate_path.has_value()) {
        configuration.caCertificatePemFile = *options().ca_certificate_path;
    }
    auto conn = std::make_unique<rtc::WebSocket>(std::move(configuration));
    conn->onOpen(std::bind(&ClientImpl::on_connected, this));
    conn->onMessage(std::bind(&ClientImpl::on_binary_message_received, this,
                        std::placeholders::_1),
        std::bind(&ClientImpl::on_text_message_received, this,
            std::placeholders::_1));
    conn->onClosed(std::bind(&ClientImpl::on_disconnected, this));
    conn->onError([](rtc::string message) {
        log(Error) << message;
    });
    return conn;
}

void ClientImpl::internal_start()
{
    if (m_conn == nullptr) {
        m_conn = create_conn();
    }
    rtc::WebSocket::Headers headers;
    headers["User-Agent"] = LOON_USER_AGENT;
    for (auto const& [key, value] : options().headers) {
        headers[key] = value;
    }
    if (options().basic_authorization.has_value()) {
        auto credentials = options().basic_authorization.value();
        headers["Authorization"] = "Basic " + base64::encode(credentials);
    }
    try {
        m_conn->open(address(), headers);
    }
    catch (std::exception const& e) {
        log(Error) << "failed to open connection: " << e.what()
                   << var("address", address());
    }
}

void ClientImpl::internal_stop()
{
    if (m_conn == nullptr) {
        log(Warning) << "stop called on stopped client";
        assert(false);
        return;
    }
    try {
        m_conn->close();
    }
    catch (std::exception const& e) {
        log(Error) << "exception during websocket close: " << e.what();
    }
    m_conn.reset();
}

int64_t ClientImpl::send_binary(const char* data, size_t length)
{
    if (m_conn == nullptr) {
        log(Warning) << "send_binary called on closed connection";
        assert(false);
        return 0;
    }
    try {
        auto result =
            m_conn->send(reinterpret_cast<const rtc::byte*>(data), length);
        if (result) {
            return length;
        }
    }
    catch (std::exception const& e) {
        log(Error) << "exception during websocket send: " << e.what();
    }
    return 0;
}

int64_t ClientImpl::send_text(const char* data, size_t length)
{
    if (m_conn == nullptr) {
        log(Warning) << "send_text called on closed connection";
        assert(false);
        return 0;
    }
    try {
        bool result = m_conn->send(rtc::string(data, length));
        if (result) {
            return length;
        }
    }
    catch (std::exception const& e) {
        log(Error) << "exception during websocket send: " << e.what();
    }
    return 0;
}

void ClientImpl::on_connected()
{
    on_websocket_open();
    log(Info) << "connected";
}

void ClientImpl::on_disconnected()
{
    on_websocket_close();
    log(Info) << "disconnected";
}

inline void ClientImpl::on_text_message_received(rtc::string text)
{
    on_websocket_message(text);
}

inline void ClientImpl::on_binary_message_received(rtc::binary bytes)
{
    // FIXME Don't call the same method for text and binary messages.

    on_websocket_message(
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void loon::websocket::log_level(LogLevel level)
{
    auto converted_level = loon_to_rtc_level(level);
    // FIXME Updating the log level does not appear to be thread-safe.
    rtc::InitLogger(converted_level, rtc_log_handler);
    {
        const std::lock_guard<std::mutex> lock(g_log_mutex);
        g_log_level.exchange(level);
    }
}

void loon::websocket::log_handler(log_handler_t handler)
{
    const std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!handler) {
        handler = default_log_handler;
    }
    g_log_handler = handler;
}

loon::log_handler_t loon::websocket::log_handler()
{
    const std::lock_guard<std::mutex> lock(g_log_mutex);
    assert(g_log_handler != nullptr);
    return g_log_handler;
}

inline void log_message(loon::LogLevel level, std::string const& message)
{
    const std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_handler) {
        g_log_handler(level, LOG_PREFIX + message);
    }
}

static void rtc_log_handler(rtc::LogLevel level, std::string message)
{
    // Read the current logger in a thread-safe manner.
    decltype(g_log_handler) handler;
    {
        const std::lock_guard<std::mutex> lock(g_log_mutex);
        if (!g_log_handler) {
            return;
        }
        handler = g_log_handler;
    }
    std::ostringstream oss;
    oss << LOG_PREFIX;
    oss << message;
    auto log_message = oss.str();
    // Log
    loon::LogLevel converted_level = rtc_to_loon_level(level);
    handler(converted_level, log_message);
}

static constexpr rtc::LogLevel loon_to_rtc_level(loon::LogLevel level)
{
    switch (level) {
    case loon::LogLevel::Verbose:
        return rtc::LogLevel::Verbose;
    case loon::LogLevel::Debug:
        return rtc::LogLevel::Debug;
    case loon::LogLevel::Info:
        return rtc::LogLevel::Info;
    case loon::LogLevel::Warning:
        return rtc::LogLevel::Warning;
    case loon::LogLevel::Error:
        return rtc::LogLevel::Error;
    case loon::LogLevel::Fatal:
        return rtc::LogLevel::Fatal;
    case loon::LogLevel::Silent:
        return rtc::LogLevel::None;
    default:
        assert(false);
        return rtc::LogLevel::Warning;
    }
}

static constexpr loon::LogLevel rtc_to_loon_level(rtc::LogLevel level)
{
    switch (level) {
    case rtc::LogLevel::Verbose:
        return loon::LogLevel::Verbose;
    case rtc::LogLevel::Debug:
        return loon::LogLevel::Debug;
    case rtc::LogLevel::Info:
        return loon::LogLevel::Info;
    case rtc::LogLevel::Warning:
        return loon::LogLevel::Warning;
    case rtc::LogLevel::Error:
        return loon::LogLevel::Error;
    case rtc::LogLevel::Fatal:
        // If there is a fatal websocket error, it's also a fatal loon error,
        // since the client cannot possibly be connected to the server.
        return loon::LogLevel::Fatal;
    case rtc::LogLevel::None:
        return loon::LogLevel::Silent;
    default:
        assert(false);
        return loon::LogLevel::Warning;
    }
}
