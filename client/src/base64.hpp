#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace base64
{

namespace detail
{

enum class Base64Variant
{
    Standard,
    Url,
    UrlUnpadded,
};

std::string base64_encode(std::uint8_t const* data, std::size_t length,
    Base64Variant variant = Base64Variant::Standard);

} // namespace detail

inline std::string encode(std::uint8_t const* data, std::size_t length)
{
    return detail::base64_encode(data, length, detail::Base64Variant::Standard);
}

inline std::string encode_url(std::uint8_t const* data, std::size_t length)
{
    return detail::base64_encode(data, length, detail::Base64Variant::Url);
}

inline std::string encode_url_unpadded(
    std::uint8_t const* data, std::size_t length)
{
    return detail::base64_encode(
        data, length, detail::Base64Variant::UrlUnpadded);
}

inline std::string encode(std::string const& data)
{
    return encode(
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

inline std::string encode_url(std::string const& data)
{
    return encode_url(
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

inline std::string encode_url_unpadded(std::string const& data)
{
    return encode_url_unpadded(
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

} // namespace base64
