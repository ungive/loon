#pragma once

#include <istream>
#include <optional>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

namespace loon
{
/**
 * @brief Describes a source for loon content.
 *
 * This class must be derived from to provide content for a loon::Client.
 * The implemented data structure does not need to be thread safe.
 */
class ContentSource
{
public:
    /**
     * @brief Returns an input stream that supplies binary data for the content.
     *
     * Must return a reader that points to the beginning of the data.
     * This method may be called more than once to serve multiple requests.
     * The return value is guaranteed to be used only to serve one request
     * and another call to this method is only made,
     * once any previous requests have been completed.
     * The underlying stream may therefore be reused or reset
     * and does not need to be thread-safe.
     *
     * The stream must supply at least as many bytes
     * as the size() method indicates
     * and at most that many bytes will be read from the stream.
     *
     * @returns The input stream to use for reading data.
     */
    virtual std::istream& data() = 0;

    /**
     * @brief Returns the size of the content.
     *
     * The input stream returned by data()
     * must supply at least this many bytes.
     *
     * @returns The size of the content data in bytes.
     */
    virtual size_t size() const = 0;

    /**
     * @brief Returns the MIME type of the content.
     *
     * The content type must be a valid MIME type/HTTP content type,
     * as per RFC 1341: https://www.rfc-editor.org/rfc/rfc1341.
     *
     * @returns The MIME type of the content.
     */
    virtual std::string const& content_type() const = 0;

    /**
     * @brief Deallocates any resources that were allocated for this source.
     */
    virtual void close() = 0;
};

/**
 * @brief Implements a ContentSource for a simple in-memory byte buffer.
 */
class BufferContentSource : public ContentSource
{
public:
    /**
     * @brief Construct a new BufferContentSource from a byte vector.
     *
     * The default MIME type is "application/octet-stream".
     *
     * @param buffer The buffer to use for the underlying data.
     * @param content_type The content type of the data.
     */
    BufferContentSource(std::vector<char> const& buffer,
        std::string const& content_type = "application/octet-stream")
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

/**
 * @brief Information about how loon content should be provided by the server.
 */
struct ContentInfo
{
    ContentInfo() = default;

    ContentInfo(std::string const& path) : path{ path } {}

    ContentInfo(std::string const& path, uint32_t max_cache_duration)
        : path{ path }, max_cache_duration{ max_cache_duration }
    {
    }

    ContentInfo(std::string const& path, std::string const& attachment_filename)
        : path{ path }, attachment_filename{ attachment_filename }
    {
    }

    ContentInfo(std::string const& path,
        uint32_t max_cache_duration,
        std::string const& attachment_filename)
        : path{ path }, max_cache_duration{ max_cache_duration },
          attachment_filename{ attachment_filename }
    {
    }

    /**
     * @brief The path under which the content should be available.
     *
     * An empty value represents the root path.
     *
     * Note that making content available under a path
     * after other content was available under the same path before,
     * might cause the old content to still be available under that path
     * until all caches are cleared.
     * It is recommended to never register a path twice,
     * if the content under that path can change over time
     * for the lifetime of the client connection.
     *
     * Use a randomized path to guarantee uniqueness of every registered path.
     */
    std::string path{ "" };

    /**
     * @brief The maximum duration for which content may be cached.
     *
     * A zero value indicates that the content should not be stored in caches.
     */
    std::optional<uint32_t> max_cache_duration{ 0 };

    /**
     * @brief The attachment filename.
     *
     * Indicates that the content should be downloaded as an attachment
     * with the given filename.
     */
    std::optional<std::string> attachment_filename{};
};
} // namespace loon
