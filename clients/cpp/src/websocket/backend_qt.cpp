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

Client::Client(std::string const& address, WebsocketOptions const& options)
    : m_impl{ std::make_unique<ClientImpl>(address, options) }
{
    if (options.ping_interval) {
        log_message(LogLevel::Warning,
            "ignoring ping interval: "
            "this is handled internally by the QT websocket backend");
    }
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
      m_conn(), m_reconnect_timer()
{
    m_reconnect_timer.setSingleShot(true);
    this->moveToThread(&m_thread);
    m_conn.moveToThread(&m_thread);
    m_reconnect_timer.moveToThread(&m_thread);
    connect_conn(&m_conn);
    connect_reconnect_timer(&m_reconnect_timer);
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
}

void ClientImpl::connect_reconnect_timer(QTimer* timer)
{
    QObject::connect(
        timer, &QTimer::timeout, this, &ClientImpl::open_connection);
}

inline Qt::ConnectionType ClientImpl::connection_type()
{
    return QThread::currentThread() != m_thread.thread()
        ? Qt::BlockingQueuedConnection
        : Qt::AutoConnection;
}

std::chrono::milliseconds ClientImpl::next_reconnect_delay()
{
    using namespace std::chrono_literals;

    if (m_reconnect_count > 0 && options().max_reconnect_delay.has_value()) {
        // Exponentially increase reconnect delay.
        auto next = std::min(
            options().max_reconnect_delay.value(), 2 * m_reconnect_delay);
        // In case of an overflow with the above multiplication,
        // note that reconnect_delay is set if max_reconnect_delay is set:
        next = std::max(options().reconnect_delay.value(), next);
        m_reconnect_delay = next;
    }
    m_reconnect_count += 1;
    return m_reconnect_delay;
}

inline void ClientImpl::reset_reconnect_delay()
{
    using namespace std::chrono_literals;
    m_reconnect_delay = options().reconnect_delay.value_or(0ms);
    m_reconnect_count = 0;
}

void ClientImpl::open_connection()
{
    // This method is only called by the internal timer's signal,
    // which lives on the internal thread. Therefore the lock must be acquired,
    // since this can run concurrently with a call to the
    // internal_start() or internal_stop() methods.

    auto lock = acquire_lock();

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
    // This method is only called from the user
    // via the public start() method, which is synchronized with lock().

    internal_open();
}

void ClientImpl::internal_stop()
{
    // This method is only called from the destructor or from the user
    // via the public stop() method, which are both synchronized with lock().

    QMetaObject::invokeMethod(&m_reconnect_timer, "stop", connection_type());
    QMetaObject::invokeMethod(&m_conn, "close", connection_type());
}

void ClientImpl::internal_open()
{
    // Abort any existing connection first, before opening a new one.
    QMetaObject::invokeMethod(&m_conn, "abort", connection_type());
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
    QMetaObject::invokeMethod(&m_conn, "open", connection_type(), request);
}

int64_t ClientImpl::send_binary(const char* data, size_t length)
{
    qint64 n = 0;
    QMetaObject::invokeMethod(&m_conn, "sendBinaryMessage", connection_type(),
        QByteArray(data, length), &n);
    return n;
}

int64_t ClientImpl::send_text(const char* data, size_t length)
{
    qint64 n = 0;
    QMetaObject::invokeMethod(&m_conn, "sendTextMessage", connection_type(),
        QByteArray(data, length), &n);
    return n;
}

void ClientImpl::on_connected()
{
    on_websocket_open();
    reset_reconnect_delay();
    log(Info) << "connected";
}

void ClientImpl::on_disconnected()
{
    on_websocket_close();
    log(Info) << "disconnected";
    if (options().reconnect_delay.has_value()) {
        // Only start the timer if the websocket is active.
        if (active()) {
            // Note that on_disconnected() is called during
            // QWebSocket::stop's execution, which could cause a deadlock.
            // In this case it is safe though because active() always
            // returns false (which would lead to this code not being executed)
            // if the lock is held while the connection is stopped:
            // - QWebSocket::stop is only called in internal_stop()
            // - internal_stop() is called in BaseClient::stop()
            // - internal_stop() is called in BaseClient::fail()
            // => Both of these last two methods always set active to false
            // before calling internal_stop(), so if they are holding a lock
            // then the lock will not be reacquired here.
            auto lock = acquire_lock();
            auto delay = next_reconnect_delay();
            QMetaObject::invokeMethod(&m_reconnect_timer, "start",
                connection_type(), static_cast<int>(delay.count()));
        }
    }
}

void ClientImpl::on_text_message_received(QString const& message)
{
    on_websocket_message(message.toStdString());
}

void ClientImpl::on_binary_message_received(QByteArray const& message)
{
    on_websocket_message(message.toStdString());
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

static inline void log_message(loon::LogLevel level, std::string const& message)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    if (_handler) {
        _handler(level, LOG_PREFIX + message);
    }
}
