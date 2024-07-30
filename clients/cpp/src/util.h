#pragma once

#include <string>

#ifdef USE_QT
#include <QByteArray>
#endif

namespace util
{
std::string hmac_sha256(std::string_view msg, std::string_view key);
std::string base64_raw_url_encode(std::string const& text);
std::string base64_encode(std::string const& text);
#ifdef USE_QT
QByteArray base64_raw_url_encode(QByteArray const& text);
QByteArray base64_encode(QByteArray const& text);
#endif
} // namespace util
