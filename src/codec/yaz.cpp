#include "codec/yaz.hpp"

#include <algorithm>
#include <array>

namespace xl::yaz {

namespace {

constexpr u32 MaxShortMatch = 0x11;
constexpr u32 MaxLongMatch = 0xFF + 0x12;
constexpr u32 MinMatch = 3;
constexpr u32 Window = 0x1000;
constexpr std::size_t ChunksPerGroup = 8;
constexpr std::size_t MaxGroupBytes = 1 + ChunksPerGroup * 3;

// Longest decompressed size a stream of `compressed_size` bytes can describe:
// every group of eight 3-byte matches takes 25 bytes and emits 8 * 273.
u64 max_decompressed_size(std::size_t compressed_size) {
  if (compressed_size <= HeaderSize)
    return 0;
  const u64 body = compressed_size - HeaderSize;
  return (body + MaxGroupBytes - 1) / MaxGroupBytes * ChunksPerGroup * MaxLongMatch;
}

// Hash chains over 3-byte prefixes, limited to the 4 KiB window.
class MatchFinder {
public:
  explicit MatchFinder(Bytes data) : m_data(data), m_prev(data.size(), -1) { m_head.fill(-1); }

  void insert(std::size_t pos) {
    if (pos + MinMatch > m_data.size())
      return;
    const u32 h = hash(pos);
    m_prev[pos] = m_head[h];
    m_head[h] = static_cast<s32>(pos);
  }

  // Longest earlier match for `pos`, preferring the nearest on ties.
  void find(std::size_t pos, u32& best_len, u32& best_dist) const {
    best_len = 0;
    best_dist = 0;
    if (pos + MinMatch > m_data.size())
      return;
    const u32 limit = static_cast<u32>(std::min<std::size_t>(MaxLongMatch, m_data.size() - pos));
    int chain = 256;
    for (s32 cand = m_head[hash(pos)]; cand >= 0 && chain-- > 0; cand = m_prev[cand]) {
      const std::size_t dist = pos - static_cast<std::size_t>(cand);
      if (dist > Window)
        break;
      u32 len = 0;
      while (len < limit && m_data[cand + len] == m_data[pos + len])
        ++len;
      if (len > best_len) {
        best_len = len;
        best_dist = static_cast<u32>(dist);
        if (len == limit)
          break;
      }
    }
    if (best_len < MinMatch)
      best_len = 0;
  }

private:
  u32 hash(std::size_t pos) const {
    return (u32(m_data[pos]) << 16 ^ u32(m_data[pos + 1]) << 8 ^ m_data[pos + 2]) * 2654435761u >> 17;
  }

  Bytes m_data;
  std::array<s32, 1 << 15> m_head;
  std::vector<s32> m_prev;
};

}  // namespace

bool probe(Bytes data) {
  return Reader(data, Endian::Big).magic_at(0, "Yaz0");
}

Header read_header(Bytes data) {
  const Reader r(data, Endian::Big);
  r.require(0, HeaderSize, "Yaz header");
  return {r.u32_at(0x4), r.u32_at(0x8)};
}

Buffer decompress(Bytes data) {
  const Header h = read_header(data);
  if (h.decompressed_size > max_decompressed_size(data.size()))
    throw Error("Yaz decompressed size " + hex(h.decompressed_size) + " is impossible for " + hex(data.size()) +
                " bytes of input");
  Buffer out(h.decompressed_size);
  const std::size_t size = data.size();
  std::size_t src = HeaderSize, dst = 0;
  u8 group = 0;
  std::size_t remaining = 0;
  while (dst < out.size()) {
    if (remaining == 0) {
      if (src >= size)
        throw Error("Yaz stream truncated at " + hex(src));
      group = data[src++];
      remaining = ChunksPerGroup;
    }
    if (group & 0x80) {
      if (src >= size)
        throw Error("Yaz stream truncated at " + hex(src));
      out[dst++] = data[src++];
    } else {
      if (size - src < 2)
        throw Error("Yaz stream truncated at " + hex(src));
      const u8 b0 = data[src], b1 = data[src + 1];
      src += 2;
      const u32 distance = (u32(b0 & 0x0F) << 8 | b1) + 1;
      u32 length = b0 >> 4;
      if (length == 0) {
        if (src >= size)
          throw Error("Yaz stream truncated at " + hex(src));
        length = u32(data[src++]) + MaxShortMatch + 1;
      } else {
        length += 2;
      }
      if (distance > dst)
        throw Error("Yaz back-reference reaches before the start of the output");
      if (length > out.size() - dst)
        throw Error("Yaz back-reference overruns the decompressed size");
      // Byte-wise: overlapping matches replicate bytes produced by this copy.
      for (std::size_t i = 0; i < length; ++i, ++dst)
        out[dst] = out[dst - distance];
    }
    group = static_cast<u8>(group << 1);
    --remaining;
  }
  return out;
}

Buffer compress(Bytes data, u32 data_alignment) {
  Writer w(Endian::Big);
  w.bytes(std::string_view("Yaz0"));
  w.w32(static_cast<u32>(data.size()));
  w.w32(data_alignment);
  w.w32(0);

  MatchFinder finder(data);
  std::size_t pos = 0;
  while (pos < data.size()) {
    const std::size_t group_at = w.pos();
    w.w8(0);
    u8 group = 0;
    for (std::size_t chunk = 0; chunk < ChunksPerGroup && pos < data.size(); ++chunk) {
      u32 len, dist;
      finder.find(pos, len, dist);
      finder.insert(pos);
      // One-step lazy matching: a longer match one byte later is worth a literal.
      if (len >= MinMatch && len < MaxLongMatch && pos + 1 < data.size()) {
        u32 next_len, next_dist;
        finder.find(pos + 1, next_len, next_dist);
        if (next_len > len + 1)
          len = 0;
      }
      if (len >= MinMatch) {
        const u32 d = dist - 1;
        if (len <= MaxShortMatch) {
          w.w8(static_cast<u8>((len - 2) << 4 | d >> 8));
          w.w8(static_cast<u8>(d));
        } else {
          w.w8(static_cast<u8>(d >> 8));
          w.w8(static_cast<u8>(d));
          w.w8(static_cast<u8>(len - (MaxShortMatch + 1)));
        }
        for (std::size_t i = 1; i < len; ++i)
          finder.insert(pos + i);
        pos += len;
      } else {
        group |= static_cast<u8>(0x80 >> chunk);
        w.w8(data[pos]);
        ++pos;
      }
    }
    w.buffer()[group_at] = group;
  }
  return std::move(w.buffer());
}

}  // namespace xl::yaz
