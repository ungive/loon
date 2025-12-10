#include <cassert>
#include <chrono>
#include <functional>
#include <optional>

#include <rtc/websocket.hpp>

#include "base64.hpp"
#include "client.hpp"
#include "logging.hpp"

#define BACKEND_NAME "libdatachannel"
#define LOG_PREFIX BACKEND_NAME ": "

#define var loon::log_var
#define log(level) \
    loon_log_macro(level, g_log_level.load(), ::log_message, loon::Logger)

static void log_message(loon::LogLevel level, std::string const& message);

using namespace loon::websocket;

static std::mutex g_log_mutex{};
static std::atomic<loon::LogLevel> g_log_level{ default_log_level };
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

    // FIXME What value was this with Qt? What's a good default value here?
    // FIXME This should be configurable
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

    // FIXME Remove. Moved to loon client implementation

    // // Reconnect
    // if (options().reconnect_delay.has_value()) {
    //     // FIXME
    //     // assert(false && "reconnects not yet implemented");
    //     log(Warning) << "Reconnects are not yet implemented";
    // }

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

inline void ClientImpl::on_text_message_received(rtc::string message)
{
    on_websocket_message(message);
}

inline void ClientImpl::on_binary_message_received(rtc::binary message)
{
    on_websocket_message(std::string(message.begin(), message.end()));
}

void loon::websocket::log_level(LogLevel level)
{
    const std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_level.exchange(level);
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
