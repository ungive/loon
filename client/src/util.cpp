#include "util.h"

#include <array>
#include <string>

#include <openssl/hmac.h>
#include <openssl/sha.h>

std::string util::hmac_sha256(std::string_view msg, std::string_view key)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash;
    unsigned int size;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
        reinterpret_cast<unsigned char const*>(msg.data()),
        static_cast<int>(msg.size()), hash.data(), &size);
    return std::string{ reinterpret_cast<char const*>(hash.data()), size };
}
