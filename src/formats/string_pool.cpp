#include "formats/string_pool.hpp"

namespace xl {

StringPool::StringPool(std::string seed): m_bytes(std::move(seed)), m_seed_size(m_bytes.size())
{
   std::size_t start = 0;
   for (std::size_t i = 0; i < m_bytes.size(); ++i) {
      if (m_bytes[i] != 0)
         continue;
      m_offsets.emplace(m_bytes.substr(start, i - start), static_cast<u32>(start));
      start = i + 1;
   }
}

bool
StringPool::contains(const std::string &s) const
{
   return m_offsets.contains(s);
}

u32
StringPool::offset(const std::string &s)
{
   if (const auto it = m_offsets.find(s); it != m_offsets.end())
      return it->second;
   // Shipped tables share tails ("Name" inside "LongName"); a string found
   // that way in the original table keeps its place. Appended strings are
   // always whole, so a written table reads back to the same offsets.
   const std::string needle = s + '\0';
   const std::size_t found = std::string_view(m_bytes).substr(0, m_seed_size).find(needle);
   if (found != std::string::npos) {
      m_offsets.emplace(s, static_cast<u32>(found));
      return static_cast<u32>(found);
   }
   const u32 at = static_cast<u32>(m_bytes.size());
   m_bytes += needle;
   m_offsets.emplace(s, at);
   return at;
}

std::string
read_pool_string(const Reader &r, u64 base, u64 offset, const char *table)
{
   return r.cstr_at(base + offset, table);
}

} // namespace xl
