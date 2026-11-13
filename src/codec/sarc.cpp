#include "codec/sarc.hpp"

#include <algorithm>
#include <numeric>

namespace xl::sarc {

namespace {

constexpr std::size_t HeaderSize = 0x14;
constexpr std::size_t FatHeaderSize = 0xC;
constexpr std::size_t FatEntrySize = 0x10;
constexpr std::size_t FntHeaderSize = 0x8;
constexpr u32 NameOffsetMask = 0x00FFFFFF;

u32 hash_name(u32 multiplier, std::string_view name) {
  u32 hash = 0;
  for (const char c : name)
    hash = hash * multiplier + static_cast<u32>(static_cast<signed char>(c));
  return hash;
}

// Largest power of two (up to 0x2000) dividing every offset.
u32 common_alignment(const std::vector<u32>& offsets) {
  u32 a = 0x2000;
  for (const u32 o : offsets)
    while (a > 4 && o % a)
      a >>= 1;
  return a;
}

std::size_t align_up(std::size_t v, std::size_t a) {
  return (v + a - 1) / a * a;
}

}  // namespace

bool probe(Bytes data) {
  return Reader(data, Endian::Big).magic_at(0, "SARC");
}

Archive read(Bytes data) {
  Reader r(data, Endian::Big);
  r.require(0, HeaderSize, "SARC header");
  const u8 bom0 = r.u8_at(0x6), bom1 = r.u8_at(0x7);
  if (bom0 == 0xFF && bom1 == 0xFE)
    r.set_endian(Endian::Little);
  else if (!(bom0 == 0xFE && bom1 == 0xFF))
    throw Error("SARC byte order mark is invalid");

  Archive a;
  a.endian = r.endian();
  const u16 header_size = r.u16_at(0x4);
  const u32 data_offset = r.u32_at(0xC);
  a.version = r.u16_at(0x10);

  const std::size_t fat = header_size;
  if (!r.magic_at(fat, "SFAT"))
    throw Error("SARC SFAT block is missing");
  const u16 fat_header = r.u16_at(fat + 0x4);
  const u16 count = r.u16_at(fat + 0x6);
  a.hash_key = r.u32_at(fat + 0x8);
  const std::size_t entries = fat + fat_header;
  r.require(entries, std::size_t(count) * FatEntrySize, "SFAT entries");
  const std::size_t fnt = entries + std::size_t(count) * FatEntrySize;
  if (!r.magic_at(fnt, "SFNT"))
    throw Error("SARC SFNT block is missing");
  const std::size_t names = fnt + r.u16_at(fnt + 0x4);

  std::vector<u32> offsets{data_offset};
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t e = entries + i * FatEntrySize;
    const u32 name_field = r.u32_at(e + 0x4);
    const u32 begin = r.u32_at(e + 0x8), end = r.u32_at(e + 0xC);
    if (end < begin)
      throw Error("SARC entry " + std::to_string(i) + " ends before it begins");
    r.require(u64(data_offset) + begin, end - begin, "SARC file data");
    File f;
    if (name_field != 0)
      f.name = r.cstr_at(names + std::size_t(name_field & NameOffsetMask) * 4, "SARC file name");
    else
      f.name = hex(r.u32_at(e), 8);
    const auto* p = data.data() + data_offset + begin;
    f.data.assign(p, p + (end - begin));
    offsets.push_back(begin);
    a.files.push_back(std::move(f));
  }
  a.alignment = common_alignment(offsets);
  return a;
}

Buffer write(const Archive& a) {
  struct Item {
    u32 hash;
    const File* file;
  };
  std::vector<Item> items;
  for (const File& f : a.files)
    items.push_back({hash_name(a.hash_key, f.name), &f});
  std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.hash < y.hash; });

  Writer w(a.endian);
  w.bytes(std::string_view("SARC"));
  w.w16(HeaderSize);
  w.w16(0xFEFF);
  const std::size_t size_at = w.pos();
  w.w32(0);
  const std::size_t data_at = w.pos();
  w.w32(0);
  w.w16(a.version);
  w.w16(0);

  w.bytes(std::string_view("SFAT"));
  w.w16(FatHeaderSize);
  w.w16(static_cast<u16>(items.size()));
  w.w32(a.hash_key);

  std::vector<u32> name_offsets;
  u32 name_pos = 0;
  for (const Item& it : items) {
    name_offsets.push_back(name_pos);
    name_pos += static_cast<u32>(align_up(it.file->name.size() + 1, 4));
  }
  std::vector<u32> data_offsets;
  u32 data_pos = 0;
  for (const Item& it : items) {
    data_pos = static_cast<u32>(align_up(data_pos, a.alignment));
    data_offsets.push_back(data_pos);
    data_pos += static_cast<u32>(it.file->data.size());
  }
  u32 ordinal = 0;
  for (std::size_t i = 0; i < items.size(); ++i) {
    ordinal = (i != 0 && items[i].hash == items[i - 1].hash) ? ordinal + 1 : 1;
    w.w32(items[i].hash);
    w.w32(ordinal << 24 | (name_offsets[i] / 4 & NameOffsetMask));
    w.w32(data_offsets[i]);
    w.w32(data_offsets[i] + static_cast<u32>(items[i].file->data.size()));
  }

  w.bytes(std::string_view("SFNT"));
  w.w16(FntHeaderSize);
  w.w16(0);
  for (const Item& it : items) {
    w.bytes(std::string_view(it.file->name));
    w.w8(0);
    w.align(4);
  }

  w.align(a.alignment);
  const std::size_t data_start = w.pos();
  for (std::size_t i = 0; i < items.size(); ++i) {
    while (w.pos() < data_start + data_offsets[i])
      w.w8(0);
    w.bytes(Bytes(items[i].file->data));
  }
  w.patch_u32(size_at, static_cast<u32>(w.pos()));
  w.patch_u32(data_at, static_cast<u32>(data_start));
  return std::move(w.buffer());
}

}  // namespace xl::sarc
