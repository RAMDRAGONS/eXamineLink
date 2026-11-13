#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "codec/sarc.hpp"

// The containers a database travels in: Yaz-compressed SARC archives
// (.szs), Zstandard (.zs), or nothing. Opening peels the layers off and
// remembers them; saving puts the same layers back around the new payload.
namespace xl {

enum class LayerKind { Yaz, Zstd, Sarc };

struct Layer {
   LayerKind kind;
   u32 yaz_alignment = 0;
   int zstd_level = 19;
   // Compressing again needs the dictionary the original frame used.
   std::shared_ptr<const Buffer> zstd_dictionary;
   // The archive minus the edited file's contents, and which file that was.
   sarc::Archive archive;
   int sarc_index = -1;
};

struct Unwrapped {
   std::vector<Layer> layers;
   Buffer payload;
};

// Zstandard dictionaries declaring this ID. Games number theirs
// independently, so several may share one.
using DictionaryLookup = std::function<std::vector<std::shared_ptr<const Buffer>>(u32 id)>;

// `accept` picks which archive member to open; the first one it accepts wins.
Unwrapped unwrap(Bytes data, const std::function<bool(Bytes)> &accept, const DictionaryLookup &dictionaries = {});
Buffer wrap(const std::vector<Layer> &layers, Buffer payload);

std::string describe(const std::vector<Layer> &layers);

} // namespace xl
