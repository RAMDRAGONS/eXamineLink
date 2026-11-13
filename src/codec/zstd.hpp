#pragma once

#include "core/bytes.hpp"

namespace xl::zstd {

bool probe(Bytes data);
// The dictionary a frame was compressed with, or 0 for none.
u32 frame_dictionary(Bytes data);
// The ID a dictionary declares, or 0 for raw content without a header.
u32 dictionary_id(Bytes dictionary);
Buffer decompress(Bytes data, Bytes dictionary = {});
Buffer compress(Bytes data, int level, Bytes dictionary = {});

}  // namespace xl::zstd
