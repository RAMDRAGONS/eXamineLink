#pragma once

#include <string>
#include <vector>

#include "core/bytes.hpp"

// SARC: sead's flat archive, a hashed file table with a name table.
namespace xl::sarc {

struct File {
  std::string name;
  Buffer data;
};

struct Archive {
  Endian endian = Endian::Little;
  u32 hash_key = 0x65;
  u16 version = 0x100;
  // Alignment of the data block and of every file inside it; recovered from
  // the original layout so a rebuilt archive keeps the positions games expect.
  u32 alignment = 0x100;
  std::vector<File> files;
};

bool probe(Bytes data);
Archive read(Bytes data);
Buffer write(const Archive& a);

}  // namespace xl::sarc
