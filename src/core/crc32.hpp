#pragma once

#include <array>
#include <string_view>

#include "core/bytes.hpp"

namespace xl {

    // CRC-32 with the zlib polynomial, as sead::HashCRC32 computes it.
    inline u32 crc32(std::string_view s) {
        static constexpr auto table = [] {
            std::array<u32, 256> t { };
            for (u32 i = 0; i < 256; ++i) {
                u32 c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        u32 c = 0xFFFFFFFFu;
        for (const char ch : s)
            c = table[(c ^ static_cast<u8>(ch)) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

} // namespace xl
