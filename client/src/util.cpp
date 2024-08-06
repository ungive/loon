#include "util.h"

#include <algorithm>
#include <array>
#include <string>

#include <openssl/hmac.h>
#include <openssl/sha.h>

#ifdef USE_LIBHV
#include <hv/base64.h>
#endif

#ifdef USE_QT
#include <QString>
#endif

std::string util::hmac_sha256(std::string_view msg, std::string_view key)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash;
    unsigned int size;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
        reinterpret_cast<unsigned char const*>(msg.data()),
        static_cast<int>(msg.size()), hash.data(), &size);
    return std::string{ reinterpret_cast<char const*>(hash.data()), size };
}

#ifdef USE_QT
inline static QByteArray qbytearray(std::string const& text)
{
    return QByteArray(text.data(), text.size());
}

inline static QByteArray qt_base64_encode(
    QByteArray const& text, QByteArray::Base64Options options)
{
    return text.toBase64(options);
}

inline QByteArray util::base64_raw_url_encode(QByteArray const& text)
{
    return qt_base64_encode(
        text, QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

inline QByteArray util::base64_encode(QByteArray const& text)
{
    return qt_base64_encode(text, QByteArray::Base64Encoding);
}
#endif

std::string util::base64_raw_url_encode(std::string const& text)
{
#if defined(USE_QT)
    return base64_raw_url_encode(qbytearray(text)).toStdString();
#elif defined(USE_LIBHV)
    auto result = base64_encode(text);
    size_t new_size = result.size();
    for (size_t i = 0; i < result.size(); i++) {
        if (result[i] == '=') {
            // Strip the padding at the end.
            new_size = i;
            break;
        }
        switch (result[i]) {
        case '+':
            result[i] = '-';
            break;
        case '/':
            result[i] = '_';
            break;
        }
    }
    result.resize(new_size);
    return result;
#else
#error missing base64_raw_url_encode implementation
#endif
}

std::string util::base64_encode(std::string const& text)
{
#if defined(USE_LIBHV)
    auto str = reinterpret_cast<const unsigned char*>(text.c_str());
    auto len = static_cast<unsigned int>(std::min(text.size(),
        static_cast<size_t>(std::numeric_limits<unsigned int>::max())));
    return hv::Base64Encode(str, len);
#elif defined(USE_QT)
    return base64_encode(qbytearray(text)).toStdString();
#else
#error missing base64_encode implementation
#endif
}
