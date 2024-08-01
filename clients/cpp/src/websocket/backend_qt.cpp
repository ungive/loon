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

using namespace loon::websocket;

using WebsocketOptions = loon::WebsocketOptions;

std::chrono::milliseconds loon::websocket::default_connect_timeout =
    std::chrono::milliseconds{ 10000 /* same as libhv */ };

void log_message(loon::LogLevel level, std::string const& message);

Client::Client(std::string const& address, WebsocketOptions const& options)
    : m_impl{ std::make_unique<ClientImpl>(address, options) }
{
    if (options.ping_interval) {
        log(LogLevel::Warning,
            "ignoring ping interval: "
            "this is handled internally by the QT websocket backend");
    }
    if (options.reconnect_delay.has_value()) {
        // TODO implement automatic reconnecting
        throw std::runtime_error(
            "reconnects are not supported yet with the QT websocket backend");
    }
}

Client::~Client() {}

ClientImpl::ClientImpl(
    std::string const& address, WebsocketOptions const& options)
    : BaseClient(address, options), QObject(), m_thread(this),
      // The following members may not be have "this" as parent,
      // as they are moved to another thread.
      // Useful reference: https://stackoverflow.com/a/25230470/6748004
      m_conn(), m_reconnect_timer()
{
    this->moveToThread(&m_thread);
    m_conn.moveToThread(&m_thread);
    connect_conn(&m_conn);
    m_thread.start();
}

ClientImpl::~ClientImpl() { internal_stop(); }

inline void ClientImpl::connect_conn(qt::WebSocket* conn)
{
    connect(conn, &qt::WebSocket::connected, this, &ClientImpl::on_connected);
    connect(
        conn, &qt::WebSocket::disconnected, this, &ClientImpl::on_disconnected);
    connect(conn, &qt::WebSocket::textMessageReceived, this,
        &ClientImpl::on_text_message_received);
    connect(conn, &qt::WebSocket::binaryMessageReceived, this,
        &ClientImpl::on_binary_message_received);
    connect(conn, &qt::WebSocket::errorOccurred, this, &ClientImpl::on_error);
    connect(conn, &qt::WebSocket::stateChanged, this, &ClientImpl::on_state);
    connect(conn, &qt::WebSocket::sslErrors, this, &ClientImpl::on_ssl_errors);
}

inline Qt::ConnectionType ClientImpl::connection_type()
{
    return QThread::currentThread() != m_thread.thread()
        ? Qt::BlockingQueuedConnection
        : Qt::AutoConnection;
}

void ClientImpl::internal_start()
{
    // Abort any existing connection first, before opening a new one.
    QMetaObject::invokeMethod(&m_conn, "abort", connection_type());
    // URL
    QUrl url(QString::fromStdString(m_address));
    if (!url.isValid() || (url.scheme() != "ws" && url.scheme() != "wss")) {
        log_message(LogLevel::Error, "the websocket address is invalid");
        fail();
        return;
    }
    // Headers
    QNetworkRequest request(url);
    // Default user-agent.
    request.setRawHeader("User-Agent", LOON_USER_AGENT);
    for (auto const& [key, value] : m_options.headers) {
        request.setRawHeader(QString::fromStdString(key).toLocal8Bit(),
            QString::fromStdString(value).toLocal8Bit());
    }
    if (m_options.basic_authorization.has_value()) {
        auto credentials = m_options.basic_authorization.value();
        auto encoded = QString::fromStdString(util::base64_encode(credentials));
        request.setRawHeader("Authorization", "Basic " + encoded.toLocal8Bit());
    }
    // Server verification (CA certificate)
    bool is_wss = url.scheme() == "wss";
    if (is_wss && m_options.ca_certificate_path.has_value()) {
        QSslConfiguration sslConfiguration;
        auto const& path = m_options.ca_certificate_path.value();
        QFile certFile(QString::fromStdString(path));
        certFile.open(QIODevice::ReadOnly);
        QSslCertificate certificate(&certFile, QSsl::Pem);
        certFile.close();
        sslConfiguration.addCaCertificate(certificate);
        m_conn.setSslConfiguration(sslConfiguration);
    } else if (is_wss && m_options.ca_certificate.has_value()) {
        QSslConfiguration sslConfiguration;
        auto const& raw_cert = m_options.ca_certificate.value();
        QByteArray cert_data(
            reinterpret_cast<const char*>(raw_cert.data()), raw_cert.size());
        QSslCertificate certificate(cert_data, QSsl::Pem);
        sslConfiguration.addCaCertificate(certificate);
        m_conn.setSslConfiguration(sslConfiguration);
    } else {
        m_conn.setSslConfiguration({}); // reset
    }
    // Connect timeout
    if (m_options.connect_timeout.has_value()) {
        request.setTransferTimeout(m_options.connect_timeout.value().count());
    } else {
        request.setTransferTimeout(default_connect_timeout.count()); // reset
    }
    QMetaObject::invokeMethod(&m_conn, "open", connection_type(), request);
}

void ClientImpl::internal_stop()
{
    QMetaObject::invokeMethod(&m_conn, "close", connection_type());
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

void ClientImpl::on_connected() { on_websocket_open(); }

void ClientImpl::on_disconnected() { on_websocket_close(); }

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

static std::mutex _mutex{};
static std::atomic<loon::LogLevel> _level{ default_log_level };
static loon::log_handler_t _handler{ loon::default_log_handler };

void ClientImpl::on_error(QAbstractSocket::SocketError error)
{
    if (LogLevel::Error < _level.load()) {
        return;
    }
    std::ostringstream oss;
    oss << "socket error: " << flatten_qt_enum_value(qt_enum_key(error), true);
    log_message(LogLevel::Warning, oss.str());
}

void ClientImpl::on_state(QAbstractSocket::SocketState state)
{
    if (LogLevel::Info < _level.load()) {
        return;
    }
    std::ostringstream oss;
    oss << "socket state: " << flatten_qt_enum_value(qt_enum_key(state), true);
    log_message(LogLevel::Info, oss.str());
}

void ClientImpl::on_ssl_errors(const QList<QSslError>& errors)
{
    if (LogLevel::Error < _level.load()) {
        return;
    }
    for (auto const& error : errors) {
        std::ostringstream oss;
        oss << "ssl error: " << error.errorString().toStdString();
        log_message(LogLevel::Error, oss.str());
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
