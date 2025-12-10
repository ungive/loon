/**
 * Copyright (c) 2020-2022 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// Borrowed and adapted from libdatachannel:
// https://github.com/paullouisageneau/libdatachannel

#include "base64.hpp"

#include <assert.h>
#include <cstddef>
#include <cstdint>
#include <string>

enum class Base64Variant
{
    Standard,
    Url,
    UrlUnpadded,
};

std::string base64::detail::base64_encode(
    std::uint8_t const* data, std::size_t length, Base64Variant variant)
{
#define BASE64_ALPHABET_VARIANT(c63_64) \
    ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789" c63_64)

    static const char standard_tab[] = BASE64_ALPHABET_VARIANT("+/");
    static const char url_tab[] = BASE64_ALPHABET_VARIANT("-_");

    const char* tab = nullptr;
    bool unpadded = false;

    switch (variant) {
    default:
        assert(false);
    case Base64Variant::Standard:
        tab = standard_tab;
        break;
    case Base64Variant::UrlUnpadded:
        unpadded = true;
    case Base64Variant::Url:
        tab = url_tab;
        break;
    }

    std::string out;
    out.reserve(3 * ((length + 3) / 4));
    int i = 0;
    while (length - i >= 3) {
        auto d0 = data[i];
        auto d1 = data[i + 1];
        auto d2 = data[i + 2];
        out += tab[d0 >> 2];
        out += tab[((d0 & 3) << 4) | (d1 >> 4)];
        out += tab[((d1 & 0x0F) << 2) | (d2 >> 6)];
        out += tab[d2 & 0x3F];
        i += 3;
    }

    int left = int(length - i);
    if (left) {
        auto d0 = data[i];
        out += tab[d0 >> 2];
        if (left == 1) {
            out += tab[(d0 & 3) << 4];
            if (!unpadded) {
                out += '=';
            }
        } else { // left == 2
            auto d1 = data[i + 1];
            out += tab[((d0 & 3) << 4) | (d1 >> 4)];
            out += tab[(d1 & 0x0F) << 2];
        }
        if (!unpadded) {
            out += '=';
        }
    }

    return out;
}
