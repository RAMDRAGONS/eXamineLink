#include "formats/xlink_words.hpp"

#include "formats/string_pool.hpp"
#include "model/xlink_schema.hpp"

namespace xl {

using namespace xlink;

DirectValues::DirectValues(const std::string &seed, Endian endian)
{
   const Reader r(Bytes(reinterpret_cast<const u8 *>(seed.data()), seed.size()), endian);
   for (std::size_t k = 0; k + 4 <= seed.size(); k += 4) {
      m_values.push_back(r.u32_at(k));
      m_index.emplace(m_values.back(), static_cast<u32>(m_values.size() - 1));
   }
}

u32
DirectValues::index(u32 value, s64 hint)
{
   if (hint >= 0 && static_cast<u64>(hint) < m_values.size() && m_values[static_cast<std::size_t>(hint)] == value)
      return static_cast<u32>(hint);
   // The earliest matching entry, as a fresh build would choose.
   const auto range = m_index.equal_range(value);
   u32 best = 0xFFFFFFFF;
   for (auto it = range.first; it != range.second; ++it)
      best = std::min(best, it->second);
   if (best != 0xFFFFFFFF)
      return best;
   m_values.push_back(value);
   m_index.emplace(value, static_cast<u32>(m_values.size() - 1));
   return static_cast<u32>(m_values.size() - 1);
}

void
read_param_word(Node &p, u32 word, const std::vector<u32> &direct, const std::function<std::string(u64)> &name)
{
   const u32 rt = word >> 24, v = word & 0xFFFFFF;
   p.set_i(param::RefType, rt);
   p.set_i(param::DirectHint, -1);
   if (rt == RT_String) {
      p.set_s(param::Value, name(v));
   } else if (is_direct_table_ref(rt)) {
      if (v >= direct.size())
         throw Error("parameter names direct value " + std::to_string(v) + " of " + std::to_string(direct.size()));
      p.set_i(param::Value, direct[v]);
      p.set_i(param::DirectHint, v);
   } else {
      p.set_i(param::Value, v);
   }
}

u32
param_word(const Node &p, StringPool &names, DirectValues &direct, const std::vector<u64> &arrange_pos)
{
   const s64 rt = p.i(param::RefType);
   u64 v;
   if (rt == RT_String)
      v = names.offset(p.s(param::Value));
   else if (is_direct_table_ref(rt))
      v = direct.index(static_cast<u32>(p.i(param::Value)), p.i(param::DirectHint));
   else if (rt == RT_ArrangeGroup)
      v = arrange_pos.at(static_cast<std::size_t>(p.i(param::Value)));
   else
      v = static_cast<u64>(p.i(param::Value));
   if (v > 0xFFFFFF)
      throw Error("parameter value " + hex(v) + " does not fit the 24 bits a reference holds");
   return static_cast<u32>(rt) << 24 | static_cast<u32>(v);
}

} // namespace xl
