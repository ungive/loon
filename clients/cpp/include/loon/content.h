#pragma once

#include <istream>
#include <optional>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

namespace loon
{
class ContentSource
{
public:
    virtual std::istream& data() = 0;
    virtual size_t size() const = 0;
    virtual std::string const& content_type() const = 0;
    virtual void close() = 0;
};

class BufferContentSource : public ContentSource
{
public:
    BufferContentSource(
        std::vector<char> const& buffer, std::string const& content_type)
        : m_data{ buffer }, m_content_type{ content_type },
          m_stream{ new_stream() }
    {
    }

    inline std::istream& data() override
    {
        reset_stream();
        return m_stream;
    }

    inline size_t size() const override { return m_data.size(); }

    inline std::string const& content_type() const override
    {
        return m_content_type;
    }

    inline void close() override {}

private:
    class MemoryStreamBuffer : public std::basic_streambuf<char>
    {
    public:
        MemoryStreamBuffer(char* p, size_t l) { setg(p, p, p + l); }
    };

    class MemoryStream : public std::istream
    {
    public:
        MemoryStream(char* p, size_t l)
            : std::istream(&m_buffer), m_buffer(p, l)
        {
            rdbuf(&m_buffer);
        }

    private:
        MemoryStreamBuffer m_buffer;
    };

    inline MemoryStream new_stream()
    {
        return MemoryStream(const_cast<char*>(m_data.data()), m_data.size());
    }

    inline void reset_stream() { m_stream = std::move(new_stream()); }

    std::vector<char> m_data;
    MemoryStream m_stream;
    std::string m_content_type;
};

struct ContentInfo
{
    ContentInfo(std::string const& path) : path{ path } {}

    ContentInfo(std::string const& path, uint32_t max_cache_duration)
        : path{ path }, max_cache_duration{ max_cache_duration }
    {
    }

    ContentInfo(std::string const& path, std::string const& attachment_filename)
        : path{ path }, attachment_filename{ attachment_filename }
    {
    }

    ContentInfo(std::string const& path, uint32_t max_cache_duration,
        std::string const& attachment_filename)
        : path{ path }, max_cache_duration{ max_cache_duration },
          attachment_filename{ attachment_filename }
    {
    }

    std::string path;
    std::optional<std::string> attachment_filename;
    std::optional<uint32_t> max_cache_duration;
};
} // namespace loon
