#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <string_view>

namespace xl {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;

using Bytes = std::span<const u8>;
using Buffer = std::vector<u8>;

enum class Endian { Little, Big };

class Error : public std::runtime_error {
public:
   using std::runtime_error::runtime_error;
};

std::string hex(u64 v, int width = 0);

inline u32
f32_bits(f32 f)
{
   return std::bit_cast<u32>(f);
}
inline f32
bits_f32(u32 v)
{
   return std::bit_cast<f32>(v);
}

// Bounds-checked reads in a fixed byte order.
class Reader {
public:
   Reader(Bytes data, Endian e): m_data(data), m_endian(e)
   {
   }

   Bytes data() const
   {
      return m_data;
   }
   std::size_t size() const
   {
      return m_data.size();
   }
   Endian endian() const
   {
      return m_endian;
   }
   void set_endian(Endian e)
   {
      m_endian = e;
   }

   void require(u64 at, u64 len, std::string_view what) const
   {
      if (at > m_data.size() || len > m_data.size() - at)
         throw Error(std::string(what) + " at " + hex(at) + " (" + hex(len) +
                     " bytes) runs past the end of the data (" + hex(m_data.size()) + ")");
   }
   Bytes slice(u64 at, u64 len, std::string_view what) const
   {
      require(at, len, what);
      return m_data.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(len));
   }

   u8 u8_at(u64 at) const
   {
      require(at, 1, "byte");
      return m_data[at];
   }
   u16 u16_at(u64 at) const
   {
      return static_cast<u16>(read(at, 2));
   }
   u32 u32_at(u64 at) const
   {
      return static_cast<u32>(read(at, 4));
   }
   u64 u64_at(u64 at) const
   {
      return read(at, 8);
   }
   s16 s16_at(u64 at) const
   {
      return static_cast<s16>(u16_at(at));
   }
   s32 s32_at(u64 at) const
   {
      return static_cast<s32>(u32_at(at));
   }
   f32 f32_at(u64 at) const
   {
      return bits_f32(u32_at(at));
   }

   bool magic_at(u64 at, std::string_view m) const
   {
      return at <= m_data.size() && m.size() <= m_data.size() - at &&
             std::memcmp(m_data.data() + at, m.data(), m.size()) == 0;
   }

   // NUL-terminated string as raw bytes.
   std::string cstr_at(u64 at, std::string_view what = "string") const
   {
      require(at, 0, what);
      const u8 *begin = m_data.data() + at;
      const void *nul = std::memchr(begin, 0, m_data.size() - at);
      if (!nul)
         throw Error(std::string(what) + " at " + hex(at) + " is not terminated");
      return std::string(reinterpret_cast<const char *>(begin), static_cast<const u8 *>(nul) - begin);
   }

private:
   u64 read(u64 at, int n) const
   {
      require(at, n, "value");
      u64 v = 0;
      if (m_endian == Endian::Big)
         for (int i = 0; i < n; ++i)
            v = v << 8 | m_data[at + i];
      else
         for (int i = n - 1; i >= 0; --i)
            v = v << 8 | m_data[at + i];
      return v;
   }

   Bytes m_data;
   Endian m_endian;
};

// Appending writer with patchable positions.
class Writer {
public:
   explicit Writer(Endian e): m_endian(e)
   {
   }

   Buffer &buffer()
   {
      return m_buf;
   }
   const Buffer &buffer() const
   {
      return m_buf;
   }
   std::size_t pos() const
   {
      return m_buf.size();
   }
   Endian endian() const
   {
      return m_endian;
   }

   void w8(xl::u8 v)
   {
      m_buf.push_back(v);
   }
   void w16(xl::u16 v)
   {
      put(v, 2);
   }
   void w32(xl::u32 v)
   {
      put(v, 4);
   }
   void w64(xl::u64 v)
   {
      put(v, 8);
   }
   void ws16(xl::s16 v)
   {
      w16(static_cast<xl::u16>(v));
   }
   void ws32(xl::s32 v)
   {
      w32(static_cast<xl::u32>(v));
   }
   void wf32(xl::f32 v)
   {
      w32(f32_bits(v));
   }
   void bytes(Bytes b)
   {
      m_buf.insert(m_buf.end(), b.begin(), b.end());
   }
   void bytes(std::string_view s)
   {
      m_buf.insert(m_buf.end(), s.begin(), s.end());
   }
   void zeros(std::size_t n)
   {
      m_buf.insert(m_buf.end(), n, 0);
   }
   void align(std::size_t a)
   {
      while (m_buf.size() % a)
         m_buf.push_back(0);
   }

   void patch_u16(std::size_t at, xl::u16 v)
   {
      patch(at, v, 2);
   }
   void patch_u32(std::size_t at, xl::u32 v)
   {
      patch(at, v, 4);
   }
   void patch_u64(std::size_t at, xl::u64 v)
   {
      patch(at, v, 8);
   }

private:
   void put(xl::u64 v, int n)
   {
      const std::size_t at = m_buf.size();
      m_buf.resize(at + n);
      patch(at, v, n);
   }
   void patch(std::size_t at, xl::u64 v, int n)
   {
      for (int i = 0; i < n; ++i) {
         const int shift = m_endian == Endian::Big ? (n - 1 - i) * 8 : i * 8;
         m_buf[at + i] = static_cast<xl::u8>(v >> shift);
      }
   }

   Buffer m_buf;
   Endian m_endian;
};

inline std::string
hex(u64 v, int width)
{
   static const char digits[] = "0123456789ABCDEF";
   std::string s;
   do {
      s.insert(s.begin(), digits[v & 0xF]);
      v >>= 4;
   } while (v);
   while (static_cast<int>(s.size()) < width)
      s.insert(s.begin(), '0');
   return "0x" + s;
}

} // namespace xl
