#pragma once

#include <chrono>
#include <string>

#include <QObject>
#include <QThread>
#include <QTimer>
#include <QWebSocket>

#include "client.h"

// Declare the client class in the header so AUTOMOC works properly.

#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
#error Qt 6.5 or newer required
#endif // Qt < 6.5

namespace loon::websocket
{
namespace qt
{
// see https://stackoverflow.com/a/51455202/6748004
// and https://stackoverflow.com/a/25230470/6748004
class SafeThread : public QThread
{
    Q_OBJECT
    using QThread::run;

public:
    SafeThread(QObject* _parent = nullptr) : QThread(_parent) {}

    ~SafeThread()
    {
        quit();
#if QT_VERSION >= QT_VERSION_CHECK(5, 2, 0)
        requestInterruption();
#endif
        wait();
    }
};

class Timer : public QTimer
{
    Q_OBJECT

public:
    Timer(QObject* _parent = nullptr) : QTimer(_parent) {}

    ~Timer() noexcept {}
};

class WebSocket : public QWebSocket
{
    Q_OBJECT
public:
    WebSocket(QObject* _parent = nullptr)
        : QWebSocket(QString(), QWebSocketProtocol::VersionLatest, _parent)
    {
    }

public Q_SLOTS:

    void abort() { QWebSocket::abort(); }

    void sendBinaryMessage(const QByteArray& data, qint64* out)
    {
        *out = QWebSocket::sendBinaryMessage(data);
    }

    void sendTextMessage(const QString& message, qint64* out)
    {
        *out = QWebSocket::sendTextMessage(message);
    }
};
} // namespace qt

class ClientImpl : public QObject, public BaseClient
{
    Q_OBJECT
public:
    ClientImpl(std::string const& address, WebsocketOptions const& options);
    ~ClientImpl();

    int64_t send_binary(const char* data, size_t length) override;
    int64_t send_text(const char* data, size_t length) override;

protected:
    // This method should only be called when the websocket is not connected.
    void internal_start() override;
    void internal_stop() override;
    void internal_terminate() override;

private Q_SLOTS:
    void send_ping();
    void reconnect();
    void internal_open();
    void on_connected();
    void on_disconnected();
    void on_text_message_received(QString const& message);
    void on_binary_message_received(QByteArray const& message);
    void on_error(QAbstractSocket::SocketError error);
    void on_state(QAbstractSocket::SocketState state);
    void on_ssl_errors(const QList<QSslError>& errors);
    void on_pong(quint64 elapsed_time, const QByteArray& payload);

private:
    void connect_conn(qt::WebSocket* conn);
    void connect_reconnect_timer(QTimer* timer);
    void connect_heartbeat_timer(QTimer* timer);
    Qt::ConnectionType blocking_connection_type();
    std::chrono::milliseconds next_reconnect_delay();
    void reset_reconnect_delay();
    void start_heartbeat();
    void stop_heartbeat();
    void disconnect_handler(bool disconnected = true);
    void on_websocket_open_safe();
    void on_websocket_close_safe();

private:
    qt::WebSocket m_conn;
    qt::Timer m_reconnect_timer;
    qt::Timer m_heartbeat_timer;
    std::chrono::milliseconds m_reconnect_delay;
    size_t m_reconnect_count{ 0 };
    std::chrono::steady_clock::time_point m_last_ping_time{};
    std::chrono::steady_clock::time_point m_last_pong_time{};

    // The thread should be destroyed first, so define it last.
    // The websocket object above lives in this thread and may not be
    // destroyed before the thread itself is destroyed.
    qt::SafeThread m_thread;
};
} // namespace loon::websocket
