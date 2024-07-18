#pragma once

#include <sstream>
#include <utility>

#include "loon/client.h"

namespace loon
{
LogLevel log_level();
void init_logging();
void log(LogLevel level, std::string const& message);

template <typename K, typename V>
inline std::pair<K&, V&> log_var(K& name, V& value)
{
    return std::make_pair(std::ref(name), std::ref(value));
}

template <typename K, typename V>
inline std::pair<K&, V> log_var(K& name, V&& value)
{
    return std::make_pair(std::ref(name), value);
}

class LogBuffer
{
public:
    LogBuffer() = default;

    LogBuffer(LogBuffer&& other)
    {
        m_has_content = other.m_has_content;
        m_was_pair = other.m_was_pair;
        m_buffer << other.m_buffer.str();
        other.clear();
    }

    template <typename K, typename V>
    LogBuffer& operator<<(std::pair<K, V> const& pair)
    {
        if (m_has_content)
            m_buffer << ' ';
        m_buffer << pair.first << '=' << pair.second;
        m_has_content = true;
        m_was_pair = true;
        return *this;
    }

    template <typename K>
    LogBuffer& operator<<(std::pair<K, const std::string&> const& pair)
    {
        if (m_has_content)
            m_buffer << ' ';
        m_buffer << pair.first << '=' << '"' << pair.second << '"';
        m_has_content = true;
        m_was_pair = true;
        return *this;
    }

    template <typename T>
    LogBuffer& operator<<(T const& value)
    {
        if (m_was_pair)
            m_buffer << ' ';
        return write(value);
    }

    template <typename T>
    LogBuffer& write(T const& value)
    {
        m_buffer << value;
        m_has_content = true;
        m_was_pair = false;
        return *this;
    }

    inline std::string str() const { return m_buffer.str(); }

    inline bool empty() const { return !m_has_content; }

    inline void clear()
    {
        m_buffer.str("");
        m_buffer.clear();
        m_has_content = false;
        m_was_pair = false;
    }

private:
    std::ostringstream m_buffer{};
    bool m_has_content{ false };
    bool m_was_pair{ false };
};

class Logger
{
public:
    Logger(LogLevel level) : m_level{ level }, m_buffer{} {}

    Logger(Logger&& other)
        : m_level{ other.m_level }, m_buffer{ std::move(other.m_buffer) },
          m_after_buffer{ std::move(other.m_after_buffer) }
    {
        other.m_moved = true;
    }

    template <typename T>
    Logger& operator<<(T const& value)
    {
        m_buffer << value;
        return *this;
    }

    inline LogBuffer& after() { return m_after_buffer; }

    ~Logger()
    {
        if (!m_moved) {
            if (!m_after_buffer.empty()) {
                m_buffer.write(' ');
                m_buffer.write(m_after_buffer.str());
            }
            loon::log(m_level, m_buffer.str());
        }
    }

private:
    LogLevel m_level;
    LogBuffer m_buffer;
    LogBuffer m_after_buffer;
    bool m_moved{ false };
};
} // namespace loon
