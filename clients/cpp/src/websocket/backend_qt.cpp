#include "backend_qt.h"

#include <iostream>

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QFile>
#include <QMetaEnum>
#include <QObject>
#include <QWebSocket>

#include "client.h"
#include "logging.h"
#include "util.h"

using namespace loon::websocket;

using WebsocketOptions = loon::WebsocketOptions;

std::chrono::milliseconds loon::websocket::default_connect_timeout =
    std::chrono::milliseconds{ 10000 /* same as libhv */ };

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
    : BaseClient(address, options), QObject(), m_thread(this), m_conn()
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
    connect(conn, &qt::WebSocket::destroyed, this,
        &ClientImpl::on_private_destroyed);
}

inline Qt::ConnectionType ClientImpl::connection_type()
{
    return QThread::currentThread() != m_conn.thread()
        ? Qt::BlockingQueuedConnection
        : Qt::AutoConnection;
}

void ClientImpl::internal_start()
{
    std::cerr << "qt:internal_start\n";
    // Abort any existing connection first, before opening a new one.
    QMetaObject::invokeMethod(&m_conn, "abort", connection_type());
    // URL
    QUrl url(QString::fromStdString(m_address));
    if (!url.isValid() || (url.scheme() != "ws" && url.scheme() != "wss")) {
        throw std::runtime_error("the websocket address is invalid");
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

void ClientImpl::on_connected()
{
    std::cerr << "qt:connected\n";
    on_websocket_open();
}

void ClientImpl::on_disconnected()
{
    std::cerr << "qt:disconnected\n";
    on_websocket_close();
}

void ClientImpl::on_text_message_received(QString const& message)
{
    std::cerr << "qt:text_message_received\n";
    on_websocket_message(message.toStdString());
}

void ClientImpl::on_binary_message_received(QByteArray const& message)
{
    std::cerr << "qt:binary_message_received\n";
    on_websocket_message(message.toStdString());
}

void ClientImpl::on_error(QAbstractSocket::SocketError error)
{
    std::cerr << "qt:error: "
              << QMetaEnum::fromType<QAbstractSocket::SocketError>().valueToKey(
                     error)
              << std::endl;
}

void ClientImpl::on_state(QAbstractSocket::SocketState state)
{
    std::cerr << "qt:state: "
              << QMetaEnum::fromType<QAbstractSocket::SocketState>().valueToKey(
                     state)
              << std::endl;
}

void ClientImpl::on_ssl_errors(const QList<QSslError>& errors)
{
    for (auto const& error : errors) {
        std::cerr << "qt:ssl_error: " << error.errorString().toStdString()
                  << std::endl;
    }
}

void loon::websocket::ClientImpl::on_private_destroyed(QObject* ptr)
{
    std::cerr << "qt:destroyed " << reinterpret_cast<size_t>(ptr) << std::endl;
}

void loon::websocket::log_level(LogLevel level)
{
    // TODO
}

void loon::websocket::log_handler(log_handler_t handler)
{
    // TODO
}
