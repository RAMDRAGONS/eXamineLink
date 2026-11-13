#include <algorithm>

#include "formats/codecs.hpp"
#include "formats/string_pool.hpp"
#include "model/dslink_schema.hpp"

// EffectLink and SoundLink databases, the link libraries before XLink.
//
// EffectLink 9 (Wii U): a header with the user count and the name table
// position, then per user a data offset and a name. Each user block repeats
// the version and holds its effects, their parameter words, call tables,
// action triggers and actions, followed by the user's own strings.
//
// SoundLink 69 (Wii U): one offset per user, then the shared string table,
// then the user blocks: call tables, actions, assets and action triggers,
// ending with a word that is zero when records use the shared strings.
//
// EffectLink 10 and SoundLink 73 (3DS) put effects/assets and action triggers
// into pools shared by every user; a user lists pool indices and stores a -1
// marker in place of its own copies. Users can also alias another user.
namespace xl::dslink_codec {

using namespace dslink;

namespace {

constexpr u32 PoolMarker = 0xFFFFFFFF;
constexpr u32 AliasKind = 2, EmptyKind = 3;

class LinkReader {
public:
  explicit LinkReader(Bytes data) : r(data, Endian::Big) {
    r.require(0, 0x10, "link database header");
    m_effect = r.magic_at(0, "eflk");
    const u64 vf = m_effect ? 4 : 8;
    if (r.u32_at(vf) > 0xFFFF)
      r.set_endian(Endian::Little);
    m_version = r.u32_at(vf);
  }
  Document run();

private:
  const FamilySchemas& schemas() const { return m_effect ? effect_link() : sound_link(m_version); }
  void read_record(const RecordLayout& layout, Node& n, u64 at, u64 strings);
  void read_records(const RecordLayout& layout, NodeList& list, u64 at, u64 count, u64 strings);
  std::string region(u64 from, u64 to) const {
    if (from > to)
      throw Error("table at " + hex(from) + " ends before it starts");
    const auto t = r.slice(from, to - from, "table");
    return std::string(t.begin(), t.end());
  }
  void read_eflk9(Node& file);
  void read_pooled(Node& file);
  void read_slnk69(Node& file);

  Reader r;
  bool m_effect;
  u32 m_version;
};

void LinkReader::read_record(const RecordLayout& layout, Node& n, u64 at, u64 strings) {
  for (std::size_t s = 0; s < layout.slot_list.size(); ++s) {
    const u64 p = at + 4 * s;
    const int f = layout.slot_field[s];
    switch (layout.slot_list[s].kind) {
      case SlotKind::String:
        n.set_s(f, read_pool_string(r, strings, r.u32_at(p), "string table"));
        break;
      case SlotKind::Range:
      case SlotKind::Pair:
        n.set_i(f, r.s16_at(p));
        n.set_i(f + 1, r.s16_at(p + 2));
        break;
      case SlotKind::Int:
      case SlotKind::CallRef:
        n.set_i(f, r.s32_at(p));
        break;
      case SlotKind::Packed: {
        u64 q = p;
        int g = f;
        for (const char c : layout.slot_list[s].parts) {
          n.set_i(g++, c == 'b' ? r.u8_at(q) : c == 'H' ? r.u16_at(q) : r.s16_at(q));
          q += c == 'b' ? 1 : 2;
        }
        break;
      }
      default:
        n.set_i(f, r.u32_at(p));
        break;
    }
  }
}

void LinkReader::read_records(const RecordLayout& layout, NodeList& list, u64 at, u64 count, u64 strings) {
  for (u64 k = 0; k < count; ++k)
    read_record(layout, list.append(std::make_unique<Node>(&layout.schema)), at + k * layout.size(), strings);
}

void LinkReader::read_eflk9(Node& file) {
  const FamilySchemas& fs = schemas();
  const u32 n_user = r.u32_at(0x8);
  const u64 names = r.u32_at(0xC);
  file.set_s(file::StringPool, region(names, r.size()));
  for (u32 i = 0; i < n_user; ++i) {
    const u64 at = r.u32_at(0x10 + 8 * u64{i});
    const u64 next = i + 1 < n_user ? r.u32_at(0x10 + 8 * u64{i + 1}) : names;
    Node& u = file.list(file::Users).append(std::make_unique<Node>(&fs.user));
    u.set_s(user::Name, read_pool_string(r, names, r.u32_at(0x14 + 8 * u64{i}), "user name"));
    u.set_i(user::Type, UT_User);
    u.set_i(user::Kind, r.u32_at(at));
    const u32 n_e = r.u32_at(at + 4), n_t = r.u32_at(at + 8), n_c = r.u32_at(at + 12), n_a = r.u32_at(at + 16);
    const u64 strings = at + r.u32_at(at + 20);
    const u32 n_p = r.u32_at(at + 24);
    u.set_i(user::ParamsPerAsset, n_p);
    u.set_i(user::OwnStrings, 1);
    if (strings > next || strings < at)
      throw Error("EffectLink user " + u.s(user::Name) + " places its strings outside its block");
    u.set_s(user::StringPool, region(strings, next));
    u64 p = at + 28;
    read_records(fs.asset, u.list(user::Assets), p, n_e, strings);
    p += u64{n_e} * fs.asset.size();
    for (u32 e = 0; e < n_e; ++e)
      for (u32 k = 0; k < n_p; ++k, p += 4)
        u.list(user::Assets).at(static_cast<int>(e)).list(slot_params::Params).append_new().set_i(asset_param::Value,
                                                                                              r.u32_at(p));
    read_records(fs.call, u.list(user::CallTables), p, n_c, strings);
    p += u64{n_c} * fs.call.size();
    read_records(fs.trigger, u.list(user::Triggers), p, n_t, strings);
    p += u64{n_t} * fs.trigger.size();
    read_records(fs.action, u.list(user::Actions), p, n_a, strings);
    p += u64{n_a} * fs.action.size();
    if (p != strings)
      throw Error("EffectLink user " + u.s(user::Name) + " tables do not end at its strings");
  }
}

void LinkReader::read_slnk69(Node& file) {
  const FamilySchemas& fs = schemas();
  const u32 n_user = r.u32_at(0xC);
  const u64 strings = 0x10 + 4 * u64{n_user};
  u64 first = r.size();
  for (u32 i = 0; i < n_user; ++i)
    first = std::min<u64>(first, r.u32_at(0x10 + 4 * u64{i}));
  file.set_s(file::StringPool, region(strings, first));
  for (u32 i = 0; i < n_user; ++i) {
    const u64 at = r.u32_at(0x10 + 4 * u64{i});
    Node& u = file.list(file::Users).append(std::make_unique<Node>(&fs.user));
    const u32 kind = r.u32_at(at);
    if (kind == AliasKind || kind == EmptyKind)
      throw Error("SoundLink 69 user at " + hex(at) + " has kind " + std::to_string(kind) + ", which is not understood");
    u.set_i(user::Kind, kind);
    u.set_i(user::Type, UT_User);
    u.set_s(user::Name, read_pool_string(r, strings, r.u32_at(at + 8), "user name"));
    const u32 n_c = r.u32_at(at + 12), n_a = r.u32_at(at + 16), n_s = r.u32_at(at + 20), n_t = r.u32_at(at + 24);
    const u64 trailer = at + r.u32_at(at + 28);
    for (int k = 0; k < 4; ++k)
      u.set_i(user::Header1 + k, r.u32_at(at + 32 + 4 * k));
    u64 rec_strings = strings;
    if (r.u32_at(trailer) != 0) {
      const u64 next = i + 1 < n_user ? r.u32_at(0x10 + 4 * u64{i + 1}) : r.size();
      u.set_i(user::OwnStrings, 1);
      u.set_s(user::StringPool, region(trailer, std::max(trailer, next)));
      rec_strings = trailer;
    }
    u64 p = at + 0x30;
    read_records(fs.call, u.list(user::CallTables), p, n_c, rec_strings);
    p += u64{n_c} * fs.call.size();
    read_records(fs.action, u.list(user::Actions), p, n_a, rec_strings);
    p += u64{n_a} * fs.action.size();
    read_records(fs.asset, u.list(user::Assets), p, n_s, rec_strings);
    p += u64{n_s} * fs.asset.size();
    read_records(fs.trigger, u.list(user::Triggers), p, n_t, rec_strings);
    p += u64{n_t} * fs.trigger.size();
    if (p != trailer || r.u32_at(at + 4) != (trailer - at) + 4)
      throw Error("SoundLink user " + u.s(user::Name) + " block sizes disagree with its tables");
    const u64 next = i + 1 < n_user ? r.u32_at(0x10 + 4 * u64{i + 1}) : r.size();
    if (!u.i(user::OwnStrings) && trailer + 4 < next)
      u.set_s(user::Tail, region(trailer + 4, next));
  }
}

void LinkReader::read_pooled(Node& file) {
  const FamilySchemas& fs = schemas();
  const u64 header = m_effect ? 0x14 : 0x18;
  const u64 users_field = m_effect ? 0x8 : 0xC;
  const u32 n_user = r.u32_at(users_field);
  const u32 n_assets = r.u32_at(users_field + 4), n_triggers = r.u32_at(users_field + 8);
  const u64 offsets = header + u64{n_assets} * fs.asset.size() + u64{n_triggers} * fs.trigger.size();
  const u64 strings = offsets + 4 * u64{n_user};
  u64 first = r.size();
  for (u32 i = 0; i < n_user; ++i)
    first = std::min<u64>(first, r.u32_at(offsets + 4 * u64{i}));
  file.set_s(file::StringPool, region(strings, first));
  read_records(fs.asset, file.list(file::SharedAssets), header, n_assets, strings);
  read_records(fs.trigger, file.list(file::SharedTriggers), header + u64{n_assets} * fs.asset.size(), n_triggers,
               strings);

  // Header fields per family: count positions of call tables, actions,
  // assets, triggers and (EffectLink) parameters per effect.
  const u64 user_header = m_effect ? 0x1C : 0x18;
  const u64 f_calls = m_effect ? 0x14 : 0x10, f_actions = m_effect ? 0x16 : 0x12;
  const u64 f_assets = m_effect ? 0x10 : 0x14, f_triggers = m_effect ? 0x12 : 0x16;
  for (u32 i = 0; i < n_user; ++i) {
    const u64 at = r.u32_at(offsets + 4 * u64{i});
    Node& u = file.list(file::Users).append(std::make_unique<Node>(&fs.user));
    const u32 kind = r.u32_at(at);
    u.set_i(user::Kind, kind);
    u.set_s(user::Name, read_pool_string(r, strings, r.u32_at(at + 4), "user name"));
    u.set_s(user::BaseName, read_pool_string(r, strings, r.u32_at(at + 8), "user name"));
    if (kind == AliasKind || kind == EmptyKind) {
      u.set_i(user::Type, kind == AliasKind ? UT_Alias : UT_Empty);
      u.set_i(user::Header4, r.u32_at(at + 12));
      continue;
    }
    u.set_i(user::Type, UT_User);
    const u32 n_c = r.u16_at(at + f_calls), n_a = r.u16_at(at + f_actions);
    const u32 n_s = r.u16_at(at + f_assets), n_t = r.u16_at(at + f_triggers);
    const u32 n_p = m_effect ? r.u16_at(at + 0x18) : 0;
    u.set_i(user::ParamsPerAsset, n_p);
    u.set_i(user::Header4, m_effect ? r.u16_at(at + 0x1A) : 0);
    u64 p = at + user_header;
    const u64 calls = p;
    p += u64{n_c} * fs.call.size();
    const u64 actions = p;
    p += u64{n_a} * fs.action.size();
    const u64 asset_refs = p;
    p += 4 * u64{n_s};
    const u64 trigger_refs = p;
    p += 4 * u64{n_t};
    const u64 params = p;
    p += 4 * u64{n_s} * n_p;
    const u64 assets = p;
    const bool pooled_assets = r.u32_at(p) == PoolMarker;
    p += pooled_assets ? 4 : u64{n_s} * fs.asset.size();
    const u64 triggers = p;
    const bool pooled_triggers = r.u32_at(p) == PoolMarker;
    p += pooled_triggers ? 4 : u64{n_t} * fs.trigger.size();
    const u64 trailer = p;
    if (!m_effect && r.u32_at(at + 12) != trailer - at)
      throw Error("SoundLink user " + u.s(user::Name) + " places its trailer away from its tables");
    u64 next = r.size();
    for (u32 j = 0; j < n_user; ++j)
      if (const u64 o = r.u32_at(offsets + 4 * u64{j}); o > at)
        next = std::min(next, o);
    u64 rec_strings = strings;
    if (r.u32_at(trailer) != 0) {
      u.set_i(user::OwnStrings, 1);
      u.set_s(user::StringPool, region(trailer, next));
      rec_strings = trailer;
    } else if (trailer + 4 < next) {
      u.set_s(user::Tail, region(trailer + 4, next));
    }
    u.set_i(user::PooledAssets, pooled_assets);
    u.set_i(user::PooledTriggers, pooled_triggers);
    read_records(fs.call, u.list(user::CallTables), calls, n_c, rec_strings);
    read_records(fs.action, u.list(user::Actions), actions, n_a, rec_strings);
    for (u32 k = 0; k < n_s; ++k)
      u.list(user::AssetRefs).append(std::make_unique<Node>(&fs.pool_ref_asset))
          .set_i(pool_ref::Index, r.s32_at(asset_refs + 4 * u64{k}));
    for (u32 k = 0; k < n_t; ++k)
      u.list(user::TriggerRefs).append(std::make_unique<Node>(&fs.pool_ref_trigger))
          .set_i(pool_ref::Index, r.s32_at(trigger_refs + 4 * u64{k}));
    if (!pooled_assets)
      read_records(fs.asset, u.list(user::Assets), assets, n_s, rec_strings);
    NodeList& slots = pooled_assets ? u.list(user::AssetRefs) : u.list(user::Assets);
    for (u32 e = 0; e < n_s; ++e)
      for (u32 k = 0; k < n_p; ++k)
        slots.at(static_cast<int>(e)).list(slot_params::Params).append_new().set_i(
            asset_param::Value, r.u32_at(params + 4 * (u64{e} * n_p + k)));
    if (!pooled_triggers)
      read_records(fs.trigger, u.list(user::Triggers), triggers, n_t, rec_strings);
  }
}

Document LinkReader::run() {
  Document doc;
  doc.format = m_effect ? Format::EffectLink : Format::SoundLink;
  doc.endian = r.endian();
  if (m_effect ? (m_version != 9 && m_version != 10) : (m_version != 69 && m_version != 73))
    throw Error(format_name(doc.format) + " version " + std::to_string(m_version) + " is not supported");
  doc.root = std::make_unique<Node>(&schemas().file);
  Node& file = *doc.root;
  file.set_i(file::Version, m_version);
  if (m_effect && m_version == 9)
    read_eflk9(file);
  else if (!m_effect && m_version == 69)
    read_slnk69(file);
  else
    read_pooled(file);
  doc.encoding = detect_encoding(file.s(file::StringPool), Encoding::ShiftJis);
  return doc;
}

// ---------------------------------------------------------------- writing

class LinkWriter {
public:
  explicit LinkWriter(const Document& doc)
      : d(doc), file(*doc.root), fs(*family_of(file)), w(doc.endian), m_global(file.s(file::StringPool)) {}
  Buffer run();

private:
  void write_record(const RecordLayout& layout, const Node& n, StringPool& strings);
  void write_records(const RecordLayout& layout, const NodeList& list, StringPool& strings) {
    for (const auto& n : list)
      write_record(layout, *n, strings);
  }
  bool uses_own_strings(const Node& u) const;
  void write_params(const Node& u, const NodeList& slots, u64 per_slot);
  Buffer write_eflk9();
  Buffer write_slnk69();
  Buffer write_pooled();

  const Document& d;
  const Node& file;
  const FamilySchemas& fs;
  Writer w;
  StringPool m_global;
};

void LinkWriter::write_record(const RecordLayout& layout, const Node& n, StringPool& strings) {
  for (std::size_t s = 0; s < layout.slot_list.size(); ++s) {
    const int f = layout.slot_field[s];
    switch (layout.slot_list[s].kind) {
      case SlotKind::String:
        w.w32(strings.offset(n.s(f)));
        break;
      case SlotKind::Range:
      case SlotKind::Pair:
        w.ws16(static_cast<s16>(n.i(f)));
        w.ws16(static_cast<s16>(n.i(f + 1)));
        break;
      case SlotKind::Packed: {
        int g = f;
        for (const char c : layout.slot_list[s].parts) {
          if (c == 'b')
            w.w8(static_cast<u8>(n.i(g++)));
          else
            w.w16(static_cast<u16>(n.i(g++)));
        }
        break;
      }
      default:
        w.w32(static_cast<u32>(n.i(f)));
        break;
    }
  }
}

void LinkWriter::write_params(const Node& u, const NodeList& slots, u64 per_slot) {
  for (const auto& slot : slots) {
    const NodeList& params = slot->list(slot_params::Params);
    if (static_cast<u64>(params.size()) != per_slot)
      throw Error("EffectLink user " + u.s(user::Name) + " gives each effect " + std::to_string(per_slot) +
                  " parameter words, but effect " + std::to_string(slot->row()) + " has " +
                  std::to_string(params.size()));
    for (const auto& p : params)
      w.w32(static_cast<u32>(p->i(asset_param::Value)));
  }
}

// A user's own string table only exists when it starts with a non-empty
// string: a zero first word reads as "use the shared table".
bool LinkWriter::uses_own_strings(const Node& u) const {
  const std::string& pool = u.s(user::StringPool);
  return u.i(user::OwnStrings) && !pool.empty() && pool[0] != 0;
}

Buffer LinkWriter::write_eflk9() {
  const NodeList& users = file.list(file::Users);
  w.bytes(std::string_view("eflk"));
  w.w32(9);
  w.w32(static_cast<u32>(users.size()));
  const std::size_t names_at = w.pos();
  w.w32(0);
  const std::size_t entries = w.pos();
  w.zeros(8 * u64(users.size()));
  for (int i = 0; i < users.size(); ++i) {
    const Node& u = users.at(i);
    if (u.i(user::Type) != UT_User || u.i(user::PooledAssets) || u.i(user::PooledTriggers))
      throw Error("EffectLink 9 has no aliases or shared pools; user " + u.s(user::Name) + " needs converting first");
    const u64 n_e = u.list(user::Assets).size(), n_p = static_cast<u64>(u.i(user::ParamsPerAsset));
    const std::size_t at = w.pos();
    w.patch_u32(entries + 8 * i, static_cast<u32>(at));
    w.patch_u32(entries + 8 * i + 4, m_global.offset(u.s(user::Name)));
    StringPool strings(u.i(user::OwnStrings) ? u.s(user::StringPool) : std::string());
    w.w32(9);
    w.w32(static_cast<u32>(n_e));
    w.w32(static_cast<u32>(u.list(user::Triggers).size()));
    w.w32(static_cast<u32>(u.list(user::CallTables).size()));
    w.w32(static_cast<u32>(u.list(user::Actions).size()));
    const std::size_t strings_field = w.pos();
    w.w32(0);
    w.w32(static_cast<u32>(n_p));
    write_records(fs.asset, u.list(user::Assets), strings);
    write_params(u, u.list(user::Assets), n_p);
    write_records(fs.call, u.list(user::CallTables), strings);
    write_records(fs.trigger, u.list(user::Triggers), strings);
    write_records(fs.action, u.list(user::Actions), strings);
    w.patch_u32(strings_field, static_cast<u32>(w.pos() - at));
    w.bytes(strings.bytes());
    w.align(4);
  }
  w.patch_u32(names_at, static_cast<u32>(w.pos()));
  w.bytes(m_global.bytes());
  return std::move(w.buffer());
}

Buffer LinkWriter::write_slnk69() {
  const NodeList& users = file.list(file::Users);
  w.bytes(std::string_view("SLNK"));
  w.w32(0);
  w.w32(69);
  w.w32(static_cast<u32>(users.size()));
  const std::size_t offsets = w.pos();
  w.zeros(4 * u64(users.size()));
  // Names go in first so the shared table is complete before the users.
  for (const auto& u : users)
    m_global.offset(u->s(user::Name));
  for (const auto& u : users)
    if (!uses_own_strings(*u))
      for (const int l : {user::CallTables, user::Actions, user::Assets, user::Triggers}) {
        const RecordLayout& layout = l == user::CallTables ? fs.call
                                     : l == user::Actions  ? fs.action
                                     : l == user::Assets   ? fs.asset
                                                           : fs.trigger;
        for (const auto& n : u->list(l))
          for (std::size_t s = 0; s < layout.slot_list.size(); ++s)
            if (layout.slot_list[s].kind == SlotKind::String)
              m_global.offset(n->s(layout.slot_field[s]));
      }
  w.bytes(m_global.bytes());
  w.align(4);
  for (int i = 0; i < users.size(); ++i) {
    const Node& u = users.at(i);
    if (u.i(user::Type) != UT_User)
      throw Error("SoundLink 69 has no aliases; user " + u.s(user::Name) + " needs converting first");
    const std::size_t at = w.pos();
    w.patch_u32(offsets + 4 * i, static_cast<u32>(at));
    const bool own = uses_own_strings(u);
    StringPool own_pool(own ? u.s(user::StringPool) : std::string());
    StringPool& strings = own ? own_pool : m_global;
    const u64 tables = 0x30 + fs.call.size() * u64(u.list(user::CallTables).size()) +
                       fs.action.size() * u64(u.list(user::Actions).size()) +
                       fs.asset.size() * u64(u.list(user::Assets).size()) +
                       fs.trigger.size() * u64(u.list(user::Triggers).size());
    w.w32(static_cast<u32>(u.i(user::Kind) ? u.i(user::Kind) : 1));
    w.w32(static_cast<u32>(tables + 4));
    w.w32(m_global.offset(u.s(user::Name)));
    for (const int l : {user::CallTables, user::Actions, user::Assets, user::Triggers})
      w.w32(static_cast<u32>(u.list(l).size()));
    w.w32(static_cast<u32>(tables));
    for (int k = 0; k < 4; ++k)
      w.w32(static_cast<u32>(u.i(user::Header1 + k)));
    write_records(fs.call, u.list(user::CallTables), strings);
    write_records(fs.action, u.list(user::Actions), strings);
    write_records(fs.asset, u.list(user::Assets), strings);
    write_records(fs.trigger, u.list(user::Triggers), strings);
    if (own) {
      w.bytes(own_pool.bytes());
      w.align(4);
    } else {
      w.w32(0);
      w.bytes(u.s(user::Tail));
    }
  }
  w.patch_u32(4, static_cast<u32>(w.pos()));
  return std::move(w.buffer());
}

Buffer LinkWriter::write_pooled() {
  const bool effect = fs.family == Family::EffectLink;
  const NodeList& users = file.list(file::Users);
  const NodeList& assets = file.list(file::SharedAssets);
  const NodeList& triggers = file.list(file::SharedTriggers);
  if (effect) {
    w.bytes(std::string_view("eflk"));
    w.w32(static_cast<u32>(file.i(file::Version)));
  } else {
    w.bytes(std::string_view("SLNK"));
    w.w32(0);
    w.w32(static_cast<u32>(file.i(file::Version)));
  }
  w.w32(static_cast<u32>(users.size()));
  w.w32(static_cast<u32>(assets.size()));
  w.w32(static_cast<u32>(triggers.size()));

  // Every string a shared record or a user without its own table names lives
  // in the shared table, which sits between the offsets and the users.
  Writer pools(d.endian);
  std::swap(w, pools);
  write_records(fs.asset, assets, m_global);
  write_records(fs.trigger, triggers, m_global);
  std::swap(w, pools);
  w.bytes(Bytes(pools.buffer()));
  const std::size_t offsets = w.pos();
  w.zeros(4 * u64(users.size()));

  Writer body(d.endian);
  std::vector<u64> user_at;
  std::swap(w, body);
  for (const auto& up : users) {
    const Node& u = *up;
    user_at.push_back(w.pos());
    const u32 name = m_global.offset(u.s(user::Name));
    const u32 base = m_global.offset(u.s(user::BaseName));
    if (u.i(user::Type) != UT_User) {
      w.w32(u.i(user::Type) == UT_Alias ? AliasKind : EmptyKind);
      w.w32(name);
      w.w32(base);
      w.w32(static_cast<u32>(u.i(user::Header4)));
      continue;
    }
    const std::size_t at = w.pos();
    const bool own = uses_own_strings(u);
    StringPool own_pool(own ? u.s(user::StringPool) : std::string());
    StringPool& strings = own ? own_pool : m_global;
    const bool pa = u.i(user::PooledAssets), pt = u.i(user::PooledTriggers);
    const int n_s = pa ? u.list(user::AssetRefs).size() : u.list(user::Assets).size();
    const int n_t = pt ? u.list(user::TriggerRefs).size() : u.list(user::Triggers).size();
    const u64 n_p = effect ? static_cast<u64>(u.i(user::ParamsPerAsset)) : 0;
    w.w32(static_cast<u32>(u.i(user::Kind) ? u.i(user::Kind) : (effect ? 10 : 1)));
    w.w32(name);
    w.w32(base);
    const std::size_t trailer_field = w.pos();
    w.w32(0);
    if (effect) {
      w.w16(static_cast<u16>(n_s));
      w.w16(static_cast<u16>(n_t));
      w.w16(static_cast<u16>(u.list(user::CallTables).size()));
      w.w16(static_cast<u16>(u.list(user::Actions).size()));
      w.w16(static_cast<u16>(n_p));
      w.w16(static_cast<u16>(u.i(user::Header4)));
    } else {
      w.w16(static_cast<u16>(u.list(user::CallTables).size()));
      w.w16(static_cast<u16>(u.list(user::Actions).size()));
      w.w16(static_cast<u16>(n_s));
      w.w16(static_cast<u16>(n_t));
    }
    write_records(fs.call, u.list(user::CallTables), strings);
    write_records(fs.action, u.list(user::Actions), strings);
    // Users with their own records still carry an index list; it is kept as
    // read when its length still fits.
    const auto indices = [&](const NodeList& refs, int count) {
      if (refs.size() == count)
        for (const auto& r : refs)
          w.ws32(static_cast<s32>(r->i(pool_ref::Index)));
      else
        for (int k = 0; k < count; ++k)
          w.ws32(k);
    };
    indices(u.list(user::AssetRefs), n_s);
    indices(u.list(user::TriggerRefs), n_t);
    if (effect)
      write_params(u, pa ? u.list(user::AssetRefs) : u.list(user::Assets), n_p);
    if (pa)
      w.w32(PoolMarker);
    else
      write_records(fs.asset, u.list(user::Assets), strings);
    if (pt)
      w.w32(PoolMarker);
    else
      write_records(fs.trigger, u.list(user::Triggers), strings);
    if (!effect)
      w.patch_u32(trailer_field, static_cast<u32>(w.pos() - at));
    if (own) {
      w.bytes(own_pool.bytes());
      w.align(4);
    } else {
      w.w32(0);
      w.bytes(u.s(user::Tail));
    }
  }
  std::swap(w, body);
  w.bytes(m_global.bytes());
  w.align(4);
  const std::size_t users_at = w.pos();
  w.bytes(Bytes(body.buffer()));
  for (std::size_t i = 0; i < user_at.size(); ++i)
    w.patch_u32(offsets + 4 * i, static_cast<u32>(users_at + user_at[i]));
  if (!effect)
    w.patch_u32(4, static_cast<u32>(w.pos()));
  return std::move(w.buffer());
}

Buffer LinkWriter::run() {
  const u32 v = static_cast<u32>(file.i(file::Version));
  if (fs.family == Family::EffectLink) {
    if (v == 9)
      return write_eflk9();
    if (v == 10)
      return write_pooled();
  } else if (&fs == &sound_link(69)) {
    if (v == 69)
      return write_slnk69();
  } else if (v == 73) {
    return write_pooled();
  }
  throw Error(format_name(d.format) + " version " + std::to_string(v) + " cannot be written with these records");
}

}  // namespace

Document read(Bytes data) {
  return LinkReader(data).run();
}

Buffer write(const Document& doc) {
  return LinkWriter(doc).run();
}

}  // namespace xl::dslink_codec
