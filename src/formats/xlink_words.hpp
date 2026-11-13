#pragma once

#include <functional>
#include <string>
#include <vector>
#include <unordered_map>

#include "core/bytes.hpp"
#include "model/node.hpp"

namespace xl {

class StringPool;

// An XLink direct value table. Seeding it with a file's own table keeps every
// value at its original index, so unedited parameters write back unchanged;
// a new value reuses the earliest equal entry or is appended.
class DirectValues {
public:
   DirectValues(const std::string &seed, Endian endian);

   u32 index(u32 value, s64 hint);
   const std::vector<u32> &values() const
   {
      return m_values;
   }

private:
   std::vector<u32> m_values;
   std::unordered_multimap<u32, u32> m_index;
};

// Parameters stored as one word: an 8-bit reference type and a 24-bit value.
// Direct values index the direct value table, strings are name offsets and
// arrange groups are byte offsets into the arrange region, which a read
// parameter keeps in its value until the region has been read.
void read_param_word(Node &p, u32 word, const std::vector<u32> &direct, const std::function<std::string(u64)> &name);
u32 param_word(const Node &p, StringPool &names, DirectValues &direct, const std::vector<u64> &arrange_pos);

} // namespace xl
