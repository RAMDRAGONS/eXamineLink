#pragma once

#include <string>
#include <unordered_map>

#include "core/bytes.hpp"

namespace xl {

// A NUL-separated string table. Seeding it with a file's original table keeps
// every string that is still used at its original offset, so an unedited
// database writes back unchanged; new strings are appended.
class StringPool {
public:
  StringPool() = default;
  explicit StringPool(std::string seed);

  u32 offset(const std::string& s);
  bool contains(const std::string& s) const;
  const std::string& bytes() const { return m_bytes; }
  std::size_t size() const { return m_bytes.size(); }

private:
  std::string m_bytes;
  std::size_t m_seed_size = 0;
  std::unordered_map<std::string, u32> m_offsets;
};

// Reads a NUL-terminated string at `base + offset`, naming the table in errors.
std::string read_pool_string(const Reader& r, u64 base, u64 offset, const char* table);

}  // namespace xl
