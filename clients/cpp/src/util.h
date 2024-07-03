#pragma once

#include <string>

namespace util
{
std::string hmac_sha256(std::string_view msg, std::string_view key);
std::string base64_raw_url_encode(std::string const& text);
std::string base64_encode(std::string const& text);
} // namespace util
