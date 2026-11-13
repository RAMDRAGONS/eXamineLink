#pragma once

#include <set>
#include <string>

#include "core/crc32.hpp"

namespace xl {

// Guids only need to differ within a user; deriving them from names keeps an
// import or conversion reproducible.
class GuidSource {
public:
   explicit GuidSource(std::string user): m_user(std::move(user))
   {
   }
   // Keeps a guid the user already holds from being handed out.
   void reserve(u32 guid)
   {
      m_used.insert(guid);
   }
   u32 next(const std::string &what)
   {
      u32 g = crc32(m_user + ":" + what) & 0x7FFFFFFF;
      while (g == 0 || !m_used.insert(g).second)
         g = (g * 1103515245u + 12345u) & 0x7FFFFFFF;
      return g;
   }

private:
   std::string m_user;
   std::set<u32> m_used;
};

} // namespace xl
