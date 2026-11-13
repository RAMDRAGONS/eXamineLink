#pragma once

#include "core/bytes.hpp"

// Yaz: the LZ77 variant sead uses for .szs archives.
namespace xl::yaz {

    constexpr std::size_t HeaderSize = 0x10;

    bool probe(Bytes data);

    struct Header {
        u32 decompressed_size;
        // Buffer alignment the decompressed data needs; 0 when the encoder left it unset.
        u32 data_alignment;
    };

    Header read_header(Bytes data);
    Buffer decompress(Bytes data);
    Buffer compress(Bytes data, u32 data_alignment);

} // namespace xl::yaz
