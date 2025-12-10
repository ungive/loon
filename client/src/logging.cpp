#include "logging.hpp"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#include <google/protobuf/text_format.h>

#include "loon/client.hpp"
#include "websocket/client.hpp"

using namespace loon;

static std::mutex _mutex{};
static std::atomic<LogLevel> _level{ LogLevel::Warning };
static log_handler_t _handler{ default_log_handler };

static void protobuf_log_handler(google::protobuf::LogLevel level,
    const char* filename, int line, std::string const& message)
{
    // Level
    LogLevel converted_level = LogLevel::Info;
    switch (level) {
    case google::protobuf::LOGLEVEL_INFO:
        converted_level = LogLevel::Info;
        break;
    case google::protobuf::LOGLEVEL_WARNING:
        converted_level = LogLevel::Warning;
        break;
    case google::protobuf::LOGLEVEL_ERROR:
        converted_level = LogLevel::Error;
        break;
    case google::protobuf::LOGLEVEL_FATAL:
        // Fatal protobuf errors might not necessarily be fatal loon errors.
        converted_level = LogLevel::Error;
        break;
    default:
        // Unknown log level.
        converted_level = LogLevel::Warning;
    }
    // Message
    std::ostringstream oss;
    oss << "protobuf: ";
    oss << message;
    if (filename && *filename) { // Check that the filename is not empty
        oss << " [" << filename << ':' << line << ']';
    }
    auto converted_message = oss.str();
    // Logging
    decltype(_handler) handler;
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        handler = _handler;
    }
    handler(converted_level, converted_message);
}

static const char* level_str(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Fatal:
        return "FATAL";
    default:
        return "?";
    }
}

// Source: https://stackoverflow.com/a/38034148/6748004
static inline std::tm localtime(std::time_t timer)
{
    std::tm bt{};
#if defined(__unix__)
    localtime_r(&timer, &bt);
#elif defined(_MSC_VER)
    localtime_s(&bt, &timer);
#else
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    bt = *std::localtime(&timer);
#endif
    return bt;
}

static std::string time_str()
{
    using namespace std::chrono;
    auto t = system_clock::now();
    auto t_ = system_clock::to_time_t(t);
    auto ms = duration_cast<milliseconds>(t.time_since_epoch()) % 1000;
    std::ostringstream oss;
    auto lt = localtime(t_);
    oss << std::put_time(&lt, "%FT%T") << '.' << std::setfill('0')
        << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

void loon::default_log_handler(LogLevel level, const std::string& message)
{
    std::cerr << time_str() << " " << level_str(level) << ' ';
    std::cerr << "loon: " << message << std::endl;
}

void loon::init_logging()
{
    // It should be okay to set the global log handler,
    // as the protobuf library is being statically linked.
    google::protobuf::SetLogHandler(protobuf_log_handler);
}

void loon::log_message(LogLevel level, std::string const& message)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    if (_handler && level >= _level) {
        _handler(level, message);
    }
}

void loon::client_log_level(LogLevel level)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    _level = level;
}

void loon::websocket_log_level(LogLevel level)
{
    loon::websocket::log_level(level);
}

void loon::log_handler(log_handler_t handler)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    loon::websocket::log_handler(handler);
    if (!handler) {
        handler = default_log_handler;
    }
    _handler = handler;
}

LogLevel loon::log_level() { return _level; }
