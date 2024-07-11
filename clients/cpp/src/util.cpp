#include "util.h"

#include <algorithm>
#include <array>
#include <string>

#include <openssl/hmac.h>
#include <openssl/sha.h>

#if defined(USE_LIBHV)
#include <hv/base64.h>
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

std::string util::base64_raw_url_encode(std::string const& text)
{
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
}

std::string util::base64_encode(std::string const& text)
{
#if defined(USE_LIBHV)
    auto str = reinterpret_cast<const unsigned char*>(text.c_str());
    auto len = static_cast<unsigned int>(std::min(text.size(),
        static_cast<size_t>(std::numeric_limits<unsigned int>::max())));
    return hv::Base64Encode(str, len);
#else
#error Missing base64 implementation
#endif
}
