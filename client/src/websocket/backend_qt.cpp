#include "backend_qt.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QFile>
#include <QMetaEnum>
#include <QObject>
#include <QWebSocket>

#include "client.h"
#include "logging.h"
#include "util.h"

#define BACKEND_NAME "qt"
#define LOG_PREFIX BACKEND_NAME ": "

#define var loon::log_var
#define log(level) \
    loon_log_macro(level, _level.load(), ::log_message, loon::Logger)

void log_message(loon::LogLevel level, std::string const& message);

using namespace loon::websocket;
using WebsocketOptions = loon::WebsocketOptions;

static std::mutex _mutex{};
static std::atomic<loon::LogLevel> _level{ default_log_level };
static loon::log_handler_t _handler{ loon::default_log_handler };

std::chrono::milliseconds loon::websocket::default_connect_timeout =
    std::chrono::milliseconds{ 10000 /* same as libhv */ };

std::chrono::milliseconds loon::websocket::default_ping_interval =
    std::chrono::milliseconds{ 20000 };

Client::Client(std::string const& address, WebsocketOptions const& options)
    : m_impl{ std::make_unique<ClientImpl>(address, options) }
{
}

Client::~Client() {}

ClientImpl::ClientImpl(
    std::string const& address, WebsocketOptions const& options)
    : BaseClient(address, options), QObject(), m_thread(this),
      m_reconnect_delay{ options.reconnect_delay.value_or(
          std::chrono::milliseconds::zero()) },
      // The following members may not be have "this" as parent,
      // as they are moved to another thread.
      // Useful reference: https://stackoverflow.com/a/25230470/6748004
      m_conn(), m_reconnect_timer(), m_heartbeat_timer()
{
    m_reconnect_timer.setSingleShot(true);
    this->moveToThread(&m_thread);
    m_conn.moveToThread(&m_thread);
    m_reconnect_timer.moveToThread(&m_thread);
    m_heartbeat_timer.moveToThread(&m_thread);
    connect_conn(&m_conn);
    connect_reconnect_timer(&m_reconnect_timer);
    connect_heartbeat_timer(&m_heartbeat_timer);
    m_thread.start();
}

ClientImpl::~ClientImpl() { stop(); }

inline void ClientImpl::connect_conn(qt::WebSocket* conn)
{
    QObject::connect(
        conn, &qt::WebSocket::connected, this, &ClientImpl::on_connected);
    QObject::connect(
        conn, &qt::WebSocket::disconnected, this, &ClientImpl::on_disconnected);
    QObject::connect(conn, &qt::WebSocket::textMessageReceived, this,
        &ClientImpl::on_text_message_received);
    QObject::connect(conn, &qt::WebSocket::binaryMessageReceived, this,
        &ClientImpl::on_binary_message_received);
    QObject::connect(
        conn, &qt::WebSocket::errorOccurred, this, &ClientImpl::on_error);
    QObject::connect(
        conn, &qt::WebSocket::stateChanged, this, &ClientImpl::on_state);
    QObject::connect(
        conn, &qt::WebSocket::sslErrors, this, &ClientImpl::on_ssl_errors);
    QObject::connect(conn, &qt::WebSocket::pong, this, &ClientImpl::on_pong);
}

inline void ClientImpl::connect_reconnect_timer(QTimer* timer)
{
    QObject::connect(timer, &QTimer::timeout, this, &ClientImpl::reconnect);
}

inline void ClientImpl::connect_heartbeat_timer(QTimer* timer)
{
    QObject::connect(timer, &QTimer::timeout, this, &ClientImpl::send_ping);
}

inline Qt::ConnectionType ClientImpl::blocking_connection_type()
{
    return QThread::currentThread() != m_thread.thread()
        ? Qt::BlockingQueuedConnection
        : Qt::DirectConnection;
}

std::chrono::milliseconds ClientImpl::next_reconnect_delay()
{
    using namespace std::chrono_literals;

    m_reconnect_count += 1;

    if (m_reconnect_count == 1) {
        // Attempt to reconnect immediately first.
        m_reconnect_delay = 0ms;
        return 0ms;
    }
    if (m_reconnect_count == 2) {
        m_reconnect_delay = options().reconnect_delay.value_or(1000ms);
    }
    if (m_reconnect_count > 2 && options().max_reconnect_delay.has_value()) {
        // Exponentially increase reconnect delay.
        auto next = std::min(
            options().max_reconnect_delay.value(), 2 * m_reconnect_delay);
        // In case of an overflow with the above multiplication,
        // note that reconnect_delay is set if max_reconnect_delay is set:
        next = std::max(options().reconnect_delay.value(), next);
        m_reconnect_delay = next;
    }
    return m_reconnect_delay;
}

inline void ClientImpl::reset_reconnect_delay()
{
    using namespace std::chrono_literals;
    m_reconnect_delay = 0ms;
    m_reconnect_count = 0;
}

void ClientImpl::reconnect()
{
    if (!active()) {
        // Cancel if the websocket became inactive.
        return;
    }
    if (m_conn.state() != QAbstractSocket::SocketState::UnconnectedState) {
        // Cancel if the websocket is not unconnected.
        return;
    }

    log(Info) << "automatic reconnect" << var("count", m_reconnect_count)
              << var("delay", m_reconnect_delay.count());

    internal_open();
}

void ClientImpl::internal_start()
{
    // This method can be called by the user via the public start() method.
    // Using Qt::AutoConnection to make sure that this method does not block,
    // since opening the connection can trigger websocket events which would
    // in turn could call into callbacks of the caller of this method,
    // therefore resulting in a possible deadlock.

    QMetaObject::invokeMethod(
        this, &ClientImpl::internal_open, Qt::AutoConnection);
}

void ClientImpl::internal_stop()
{
    // This method can be called by the user via the public stop() method.
    // Using Qt::AutoConnection for the same reason as in internal_start().

    QMetaObject::invokeMethod(&m_reconnect_timer, "stop", Qt::AutoConnection);
    QMetaObject::invokeMethod(&m_conn, "close", Qt::AutoConnection);
}

void ClientImpl::internal_terminate()
{
    // Blocks until the connection is fully terminated.

    QMetaObject::invokeMethod(
        &m_reconnect_timer, "stop", blocking_connection_type());
    QMetaObject::invokeMethod(&m_conn, "abort", blocking_connection_type());
}

void ClientImpl::internal_open()
{
    // Abort any existing connection first, before opening a new one.
    m_conn.abort();
    // URL
    QUrl url(QString::fromStdString(address()));
    if (!url.isValid() || (url.scheme() != "ws" && url.scheme() != "wss")) {
        log(Error) << "the websocket address is invalid"
                   << var("address", address());
        fail();
        return;
    }
    // Headers
    QNetworkRequest request(url);
    // Default user-agent.
    request.setRawHeader("User-Agent", LOON_USER_AGENT);
    for (auto const& [key, value] : options().headers) {
        request.setRawHeader(QString::fromStdString(key).toLocal8Bit(),
            QString::fromStdString(value).toLocal8Bit());
    }
    if (options().basic_authorization.has_value()) {
        auto credentials = options().basic_authorization.value();
        auto encoded = QString::fromStdString(util::base64_encode(credentials));
        request.setRawHeader("Authorization", "Basic " + encoded.toLocal8Bit());
    }
    // Server verification (CA certificate)
    bool is_wss = url.scheme() == "wss";
    if (is_wss && options().ca_certificate_path.has_value()) {
        QSslConfiguration sslConfiguration;
        auto const& path = options().ca_certificate_path.value();
        QFile certFile(QString::fromStdString(path));
        certFile.open(QIODevice::ReadOnly);
        QSslCertificate certificate(&certFile, QSsl::Pem);
        certFile.close();
        sslConfiguration.addCaCertificate(certificate);
        m_conn.setSslConfiguration(sslConfiguration);
    } else if (is_wss && options().ca_certificate.has_value()) {
        QSslConfiguration sslConfiguration;
        auto const& raw_cert = options().ca_certificate.value();
        QByteArray cert_data(
            reinterpret_cast<const char*>(raw_cert.data()), raw_cert.size());
        QSslCertificate certificate(cert_data, QSsl::Pem);
        sslConfiguration.addCaCertificate(certificate);
        m_conn.setSslConfiguration(sslConfiguration);
    } else {
        m_conn.setSslConfiguration({}); // reset
    }
    // Connect timeout
    if (options().connect_timeout.has_value()) {
        request.setTransferTimeout(options().connect_timeout.value().count());
    } else {
        request.setTransferTimeout(default_connect_timeout.count());
    }
    m_conn.open(request);
}

int64_t ClientImpl::send_binary(const char* data, size_t length)
{
    qint64 n = 0;
    // While this check is not thread-safe (the client could disconnect after),
    // calling sendBinaryMessage can freeze if the connection isn't established
    // (for whatever bizarre reason).
    // This behaviour can be confirmed by commenting out this line:
    // QMetaObject::invokeMethod(&m_conn, "close", blocking_connection_type());
    // That will freeze some of the tests.
    // Commenting "sendBinaryMessage" will then unfreeze them again.
    if (connected()) {
        QMetaObject::invokeMethod(&m_conn, "sendBinaryMessage",
            blocking_connection_type(), QByteArray(data, length), &n);
    }
    return n;
}

int64_t ClientImpl::send_text(const char* data, size_t length)
{
    qint64 n = 0;
    // Making this check for the same reason as in send_binary().
    if (connected()) {
        QMetaObject::invokeMethod(&m_conn, "sendTextMessage",
            blocking_connection_type(), QByteArray(data, length), &n);
    }
    return n;
}

void ClientImpl::start_heartbeat()
{
    // Note that the ping and pong time are initialized to the same value here
    // which prevents send_ping() from attempting to reconnect on first call.
    m_last_ping_time = {};
    m_last_pong_time = {};
    m_heartbeat_timer.start(
        options().ping_interval.value_or(default_ping_interval));
}

inline void ClientImpl::stop_heartbeat() { m_heartbeat_timer.stop(); }

void ClientImpl::send_ping()
{
    // Reopen the connection if no pong was received in time.
    if (m_last_pong_time < m_last_ping_time) {
        log(Warning) //
            << "server did not respond, attempting to reconnect"
            << var("last_pong_time", m_last_pong_time.time_since_epoch())
            << var("last_ping_time", m_last_ping_time.time_since_epoch());
        return internal_open();
    }
    // Making this check for the same reason as in send_binary().
    if (connected()) {
        m_last_ping_time = std::chrono::steady_clock::now();
        m_conn.ping();
    }
}

void ClientImpl::on_pong(quint64 elapsed_time, const QByteArray& payload)
{
    m_last_pong_time = std::chrono::steady_clock::now();
}

void ClientImpl::on_connected()
{
    start_heartbeat();
    reset_reconnect_delay();
    try {
        on_websocket_open();
    }
    catch (std::exception const& e) {
        log(Error) << "uncaught exception in on_connected callback: "
                   << e.what();
    }
    catch (...) {
        log(Error) << "unknown uncaught exception in on_connected callback";
    }
    log(Info) << "connected";
}

void ClientImpl::on_disconnected()
{
    stop_heartbeat();
    try {
        on_websocket_close();
    }
    catch (std::exception const& e) {
        log(Error) << "uncaught exception in on_disconnected callback: "
                   << e.what();
    }
    catch (...) {
        log(Error) << "unknown uncaught exception in on_disconnected callback";
    }
    log(Info) << "disconnected";
    if (options().reconnect_delay.has_value() && active()) {
        m_reconnect_timer.start(next_reconnect_delay().count());
    }
}

void ClientImpl::on_text_message_received(QString const& message)
{
    try {
        on_websocket_message(message.toStdString());
    }
    catch (std::exception const& e) {
        log(Error)
            << "uncaught exception in on_text_message_received callback: "
            << e.what();
    }
    catch (...) {
        log(Error) << "unknown uncaught exception in on_text_message_received "
                      "callback";
    }
}

void ClientImpl::on_binary_message_received(QByteArray const& message)
{
    try {
        on_websocket_message(message.toStdString());
    }
    catch (std::exception const& e) {
        log(Error)
            << "uncaught exception in on_binary_message_received callback: "
            << e.what();
    }
    catch (...) {
        log(Error) << "unknown uncaught exception in "
                      "on_binary_message_received callback";
    }
}

static std::string flatten_qt_enum_value(
    const char* str, bool remove_last_word = false)
{
    std::ostringstream oss;
    size_t last_space_index = 0;
    size_t space_count = 0;
    for (auto ptr = str; *ptr; ptr++) {
        char c = *ptr;
        if (std::isupper(c) && ptr > str) {
            oss << ' ';
            last_space_index = ptr - str;
            space_count += 1;
        }
        oss << char(std::tolower(c));
    }
    auto result = oss.str();
    result.resize(last_space_index + space_count - 1);
    return result;
}

template <typename T>
inline const char* qt_enum_key(T value)
{
    return QMetaEnum::fromType<T>().valueToKey(value);
}

void ClientImpl::on_error(QAbstractSocket::SocketError error)
{
    log(Warning) << "socket error: "
                 << flatten_qt_enum_value(qt_enum_key(error), true)
                 << var("value", qt_enum_key(error));
}

void ClientImpl::on_state(QAbstractSocket::SocketState state)
{
    log(Debug) << "socket state: "
               << flatten_qt_enum_value(qt_enum_key(state), true)
               << var("value", qt_enum_key(state));
}

void ClientImpl::on_ssl_errors(const QList<QSslError>& errors)
{
    for (auto const& error : errors) {
        log(Error) << "ssl error: " << error.errorString().toStdString();
    }
}

void loon::websocket::log_level(LogLevel level) { _level.exchange(level); }

void loon::websocket::log_handler(log_handler_t handler)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    if (!handler) {
        handler = default_log_handler;
    }
    _handler = handler;
}

inline void log_message(loon::LogLevel level, std::string const& message)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    if (_handler) {
        _handler(level, LOG_PREFIX + message);
    }
}
