#include <algorithm>
#include <map>

#include "formats/codecs.hpp"
#include "formats/string_pool.hpp"
#include "model/xlink_schema.hpp"

// XLink resource version 19, as Splatoon on Wii U loads it: a 0x40-byte
// header, one offset per user, the parameter definitions (names only), every
// asset's full parameter array, trigger overwrite sets, the random and curve
// tables, the arrange-group region, the users, the condition pool and the name
// table. Parameters are 8 bytes, a reference type and a full 32-bit value, so
// there is no direct value table. Conditions carry no type tag: each reader
// knows its parent container's type, and identical byte runs are shared.
namespace xl::xlink_v19 {

using namespace xlink;

namespace {

constexpr u32 None32 = 0xFFFFFFFF;
constexpr u64 HeaderSize = 0x40;
constexpr u64 UserHeaderSize = 100;
constexpr u64 CallSize = 32;

// Version 19 numbers container types in this order; later versions moved
// Random2 up to sit after Random.
constexpr s64 FromV19[] = {CT_Switch, CT_Random, CT_Blend, CT_Sequence, CT_Random2};
constexpr s64 ToV19[] = {0, 1, 4, 2, 3, 5};

s64 from_v19_type(u32 t) {
  return t < 5 ? FromV19[t] : t;
}

u32 to_v19_type(s64 t) {
  return t >= 0 && t < 6 ? static_cast<u32>(ToV19[t]) : static_cast<u32>(t);
}

class V19Reader {
public:
  explicit V19Reader(Bytes data) : r(data, Endian::Big) {
    r.require(0, HeaderSize, "XLink header");
    if (r.u32_at(0x8) > 0xFFFF)
      r.set_endian(Endian::Little);
  }
  Document run();

private:
  std::string name(u64 off) const { return read_pool_string(r, m_names, off, "name table"); }
  std::unique_ptr<Node> make_param(u64 at, int def);
  int condition(u64 off, s64 parent_type, const std::string& property);
  void infer_switch_types();

  Reader r;
  u64 m_names = 0, m_conditions = 0, m_ex = 0;
  Node* m_file = nullptr;
  std::map<std::pair<u64, s64>, int> m_condition_index;
  // Properties each switch condition is checked against.
  std::map<int, std::vector<std::string>> m_condition_props;
  std::vector<Node*> m_arrange_params;
};

std::unique_ptr<Node> V19Reader::make_param(u64 at, int def) {
  auto p = std::make_unique<Node>(&schemas().param);
  const u32 rt = r.u32_at(at), v = r.u32_at(at + 4);
  p->set_i(param::Def, def);
  p->set_i(param::RefType, rt);
  p->set_i(param::DirectHint, -1);
  if (rt == RT_String) {
    p->set_s(param::Value, name(v));
  } else {
    p->set_i(param::Value, v);
    if (rt == RT_ArrangeGroup)
      m_arrange_params.push_back(p.get());
  }
  return p;
}

int V19Reader::condition(u64 off, s64 parent_type, const std::string& property) {
  const auto key = std::make_pair(off, parent_type);
  if (const auto it = m_condition_index.find(key); it != m_condition_index.end()) {
    if (parent_type == CT_Switch)
      m_condition_props[it->second].push_back(property);
    return it->second;
  }
  const u64 at = m_conditions + off;
  Node& c = m_file->list(file::Conditions).append(std::make_unique<Node>(&schemas().cond));
  c.set_i(cond::ParentType, parent_type);
  c.set_i(cond::PoolOffset, static_cast<s64>(off));
  switch (parent_type) {
    case CT_Switch:
      // The first word is always zero here: the property's type comes from
      // the game's property definitions, so it is inferred afterwards.
      c.set_i(cond::PropertyType, PR_S32);
      c.set_i(cond::CompareType, r.u32_at(at + 4));
      c.set_i(cond::Value, r.u32_at(at + 8));
      c.set_i(cond::LocalPropertyEnumNameIdx, -1);
      break;
    case CT_Random:
    case CT_Random2:
      c.set_i(cond::Weight, r.u32_at(at));
      break;
    case CT_Blend:
      c.set_i(cond::BlendMin, r.u32_at(at));
      break;
    default:
      c.set_i(cond::ContinueOnFade, r.s32_at(at));
      break;
  }
  const int index = c.row();
  m_condition_index.emplace(key, index);
  if (parent_type == CT_Switch)
    m_condition_props[index].push_back(property);
  return index;
}

// A property compares against enum names when every value checked against it
// is the start of a non-empty name; otherwise against numbers, floats when
// any value is too large to be a plausible integer.
void V19Reader::infer_switch_types() {
  const std::string& pool = m_file->s(file::NameTablePool);
  std::vector<bool> start(pool.size() + 1, false);
  for (std::size_t i = 0; i < pool.size(); ++i)
    start[i] = (i == 0 || pool[i - 1] == 0) && pool[i] != 0;
  NodeList& conds = m_file->list(file::Conditions);
  std::map<std::string, bool> is_enum, is_float;
  for (const auto& [index, props] : m_condition_props) {
    const u64 v = static_cast<u32>(conds.at(index).i(cond::Value));
    for (const std::string& p : props) {
      auto [it, fresh] = is_enum.emplace(p, true);
      it->second = it->second && v < pool.size() && start[v];
      auto [ft, ffresh] = is_float.emplace(p, false);
      ft->second = ft->second || (v >= 0x10000 && v <= 0xFFFF0000u);
    }
  }
  for (const auto& [index, props] : m_condition_props) {
    Node& c = conds.at(index);
    bool all_enum = !props.empty(), any_float = false;
    for (const std::string& p : props) {
      all_enum = all_enum && is_enum[p];
      any_float = any_float || is_float[p];
    }
    const u32 v = static_cast<u32>(c.i(cond::Value));
    if (all_enum) {
      c.set_i(cond::PropertyType, PR_Enum);
      c.set_i(cond::Value, 0);
      c.set_s(cond::Value, name(v));
    } else {
      c.set_i(cond::PropertyType, any_float ? PR_F32 : PR_S32);
    }
  }
}

Document V19Reader::run() {
  const auto& S = schemas();
  Document doc;
  doc.format = Format::XLink;
  doc.endian = r.endian();
  doc.root = std::make_unique<Node>(&S.file);
  Node& file = *doc.root;
  m_file = &file;

  u32 h[16];
  for (int k = 0; k < 16; ++k)
    h[k] = r.u32_at(4 * k);
  const u32 data_size = h[1];
  file.set_i(file::Version, h[2]);
  const u32 n_up = h[3], n_ap = h[4], n_tp = h[6];
  file.set_i(file::NumCustomAssetParam, h[5]);
  const u32 n_res = h[7], n_tsets = h[8], n_random = h[9], n_curve = h[10], n_point = h[11], n_user = h[12];
  m_conditions = h[13];
  m_ex = h[14];
  m_names = h[15];
  const u64 end = std::min<u64>(data_size, r.size());
  if (m_names > end || m_conditions > m_names)
    throw Error("XLink table positions are out of order");
  {
    const auto t = r.slice(m_names, end - m_names, "name table");
    file.set_s(file::NameTablePool, std::string(t.begin(), t.end()));
    const auto c = r.slice(m_conditions, m_names - m_conditions, "condition table");
    file.set_s(file::ConditionPool, std::string(c.begin(), c.end()));
  }

  u64 p = HeaderSize + 4 * u64{n_user};
  const int lists[3] = {file::UserParamDefs, file::AssetParamDefs, file::TriggerParamDefs};
  const u32 counts[3] = {n_up, n_ap, n_tp};
  for (int k = 0; k < 3; ++k)
    for (u32 i = 0; i < counts[k]; ++i, p += 8) {
      Node& d = file.list(lists[k]).append(std::make_unique<Node>(&S.pdef));
      d.set_s(pdef::Name, name(r.u32_at(p)));
      d.set_i(pdef::Type, r.u32_at(p + 4));
    }

  if (n_ap == 0 ? n_res != 0 : n_res % n_ap != 0)
    throw Error("asset parameter count " + std::to_string(n_res) + " is not a multiple of " + std::to_string(n_ap));
  const u32 n_sets = n_ap ? n_res / n_ap : 0;
  for (u32 s = 0; s < n_sets; ++s) {
    Node& set = file.list(file::AssetParamSets).append(std::make_unique<Node>(&S.pset));
    for (u32 k = 0; k < n_ap; ++k, p += 8)
      set.list(pset::Params).append(make_param(p, static_cast<int>(k)));
  }
  for (u32 s = 0; s < n_tsets; ++s) {
    Node& set = file.list(file::TriggerParamSets).append(std::make_unique<Node>(&S.pset));
    const u32 mask = r.u32_at(p);
    p += 4;
    for (u32 k = 0; k < n_tp; ++k, p += 8)
      if (mask >> k & 1)
        set.list(pset::Params).append(make_param(p, static_cast<int>(k)));
      else if (r.u32_at(p) != 0 || r.u32_at(p + 4) != 0)
        throw Error("trigger overwrite set " + std::to_string(s) + " holds data for a parameter it does not set");
  }
  for (u32 k = 0; k < n_random; ++k, p += 8) {
    Node& n = file.list(file::Randoms).append(std::make_unique<Node>(&S.random));
    n.set_i(random::Min, r.u32_at(p));
    n.set_i(random::Max, r.u32_at(p + 4));
  }
  const u64 points = p + 16 * u64{n_curve};
  for (u32 k = 0; k < n_curve; ++k, p += 16) {
    Node& n = file.list(file::Curves).append(std::make_unique<Node>(&S.curve));
    const u16 first = r.u16_at(p), count = r.u16_at(p + 2);
    if (u64{first} + count > n_point)
      throw Error("curve " + std::to_string(k) + " runs past the curve point table");
    n.set_i(curve::CurveType, r.u16_at(p + 4));
    n.set_i(curve::IsPropGlobal, r.u16_at(p + 6));
    n.set_s(curve::PropName, name(r.u32_at(p + 8)));
    n.set_i(curve::PropIdx, r.s32_at(p + 12));
    n.set_i(curve::LocalPropertyNameIdx, -1);
    for (u32 j = 0; j < count; ++j) {
      Node& pt = n.list(curve::Points).append(std::make_unique<Node>(&S.point));
      pt.set_i(point::X, r.u32_at(points + 8 * (u64{first} + j)));
      pt.set_i(point::Y, r.u32_at(points + 8 * (u64{first} + j) + 4));
    }
  }

  const u64 set_bytes = 8 * u64{n_ap};
  const u64 trigger_set_bytes = 4 + 8 * u64{n_tp};
  u64 conditions_seen = 0;
  for (u32 ui = 0; ui < n_user; ++ui) {
    const u64 at = r.u32_at(HeaderSize + 4 * ui);
    Node& u = file.list(file::Users).append(std::make_unique<Node>(&S.user));
    u32 w[25];
    for (int k = 0; k < 25; ++k)
      w[k] = r.u32_at(at + 4 * k);
    conditions_seen = w[3];
    u.set_s(user::Name, name(w[23]));
    u.set_s(user::BaseName, name(w[24]));
    const u32 n_call = w[1];
    u64 q = at + UserHeaderSize;
    for (u32 k = 0; k < n_up; ++k, q += 8)
      u.list(user::UserParams).append(make_param(q, static_cast<int>(k)));
    {
      const auto t = r.slice(q, 2 * u64{n_call}, "sorted call order");
      u.set_s(user::SortedCallOrder, std::string(t.begin(), t.end()));
    }
    q += 2 * u64{n_call} + (n_call & 1 ? 2 : 0);
    const u64 calls = q;
    const u64 containers = calls + CallSize * n_call;
    const u64 triggers = at + w[19];

    std::map<u64, int> container_index;
    for (u64 c = containers; c < triggers;) {
      container_index[c - containers] = u.list(user::Containers).size();
      Node& n = u.list(user::Containers).append(std::make_unique<Node>(&S.ctn));
      const u32 type = r.u32_at(c);
      n.set_i(ctn::Type, from_v19_type(type));
      n.set_i(ctn::ChildStart, r.s32_at(c + 4));
      n.set_i(ctn::ChildEnd, r.s32_at(c + 8));
      if (type == 0) {
        n.set_s(ctn::WatchPropertyName, name(r.u32_at(c + 12)));
        n.set_i(ctn::WatchPropertyId, r.s32_at(c + 16));
        n.set_i(ctn::LocalPropertyNameIdx, r.s16_at(c + 20));
        n.set_i(ctn::IsGlobal, r.u8_at(c + 22));
        n.set_i(ctn::SwitchByte, r.u8_at(c + 23));
        c += 24;
      } else {
        c += 12;
      }
    }

    std::vector<s64> parent_type(n_call, -1);
    std::vector<std::string> parent_prop(n_call);
    for (u32 k = 0; k < n_call; ++k) {
      const u64 c = calls + CallSize * k;
      Node& n = u.list(user::Calls).append(std::make_unique<Node>(&S.call));
      n.set_s(call::KeyName, name(r.u32_at(c)));
      n.set_i(call::AssetId, r.s32_at(c + 4));
      n.set_i(call::Guid, r.u32_at(c + 8));
      n.set_i(call::Duration, r.s32_at(c + 12));
      n.set_i(call::ParentIndex, r.s32_at(c + 16));
      n.set_i(call::Flag, r.u32_at(c + 20));
      const u32 param = r.u32_at(c + 24);
      if (call_is_container(n.i(call::Flag))) {
        if (param != None32) {
          const auto it = container_index.find(param);
          if (it == container_index.end())
            throw Error("call " + n.s(call::KeyName) + " points between containers");
          n.set_i(call::Container, it->second);
          const Node& ct = u.list(user::Containers).at(it->second);
          for (s64 ch = ct.i(ctn::ChildStart); ch <= ct.i(ctn::ChildEnd); ++ch)
            if (ch >= 0 && ch < n_call) {
              parent_type[ch] = ct.i(ctn::Type);
              parent_prop[ch] = ct.s(ctn::WatchPropertyName);
            }
        }
      } else if (set_bytes) {
        if (param % set_bytes != 0 || param / set_bytes >= n_sets)
          throw Error("call " + n.s(call::KeyName) + " points between asset parameter sets");
        n.set_i(call::ParamSet, param / set_bytes);
      }
    }
    // A call outside every child range still names its container by index.
    for (u32 k = 0; k < n_call; ++k) {
      if (parent_type[k] >= 0)
        continue;
      const s64 pi = u.list(user::Calls).at(static_cast<int>(k)).i(call::ParentIndex);
      if (pi < 0 || pi >= n_call)
        continue;
      const Node& pc = u.list(user::Calls).at(static_cast<int>(pi));
      const s64 ci = pc.i(call::Container);
      if (call_is_container(pc.i(call::Flag)) && ci >= 0 && ci < u.list(user::Containers).size()) {
        parent_type[k] = u.list(user::Containers).at(static_cast<int>(ci)).i(ctn::Type);
        parent_prop[k] = u.list(user::Containers).at(static_cast<int>(ci)).s(ctn::WatchPropertyName);
      }
    }
    for (u32 k = 0; k < n_call; ++k) {
      const u32 cond = r.u32_at(calls + CallSize * k + 28);
      if (cond == None32)
        continue;
      Node& n = u.list(user::Calls).at(static_cast<int>(k));
      n.set_i(call::Condition, condition(cond, parent_type[k] < 0 ? s64{CT_Switch} : parent_type[k], parent_prop[k]));
    }

    const auto call_of = [&](u64 pos, u64 key_name) -> s64 {
      if (pos % CallSize != 0 || pos / CallSize >= n_call)
        throw Error("trigger in user " + u.label() + " points between calls");
      const s64 i = static_cast<s64>(pos / CallSize);
      if (name(key_name) != u.list(user::Calls).at(static_cast<int>(i)).s(call::KeyName))
        throw Error("trigger in user " + u.label() + " names a different call than it points at");
      return i;
    };
    const auto overwrite = [&](u32 v) -> s64 {
      if (v == None32)
        return -1;
      if (v % trigger_set_bytes != 0 || v / trigger_set_bytes >= n_tsets)
        throw Error("trigger in user " + u.label() + " points between overwrite sets");
      return v / trigger_set_bytes;
    };
    q = triggers;
    for (u32 k = 0; k < w[4]; ++k, q += 8) {
      Node& n = u.list(user::ActionSlots).append(std::make_unique<Node>(&S.slot));
      n.set_s(slot::Name, name(r.u32_at(q)));
      n.set_i(slot::ActionStart, r.s16_at(q + 4));
      n.set_i(slot::ActionEnd, r.s16_at(q + 6));
    }
    for (u32 k = 0; k < w[5]; ++k, q += 12) {
      Node& n = u.list(user::Actions).append(std::make_unique<Node>(&S.action));
      n.set_s(action::Name, name(r.u32_at(q)));
      n.set_i(action::TriggerStart, r.s32_at(q + 4));
      n.set_i(action::TriggerEnd, r.s32_at(q + 8));
    }
    for (u32 k = 0; k < w[6]; ++k, q += 28) {
      Node& n = u.list(user::ActionTriggers).append(std::make_unique<Node>(&S.atrig));
      n.set_i(atrig::Guid, r.u32_at(q));
      n.set_i(atrig::AssetCall, call_of(r.u32_at(q + 8), r.u32_at(q + 4)));
      n.set_i(atrig::EndFrame, r.s32_at(q + 16));
      n.set_i(atrig::Flag, r.u16_at(q + 20));
      n.set_i(atrig::OverwriteHash, r.u16_at(q + 22));
      if (trigger_names_previous_action(n.i(atrig::Flag)))
        n.set_s(atrig::PreviousActionName, name(r.u32_at(q + 12)));
      else
        n.set_i(atrig::StartFrame, r.s32_at(q + 12));
      n.set_i(atrig::Overwrite, overwrite(r.u32_at(q + 24)));
    }
    for (u32 k = 0; k < w[7]; ++k, q += 16) {
      Node& n = u.list(user::Properties).append(std::make_unique<Node>(&S.prop));
      n.set_s(prop::Name, name(r.u32_at(q)));
      n.set_i(prop::IsGlobal, r.s32_at(q + 4));
      n.set_i(prop::TriggerStart, r.s32_at(q + 8));
      n.set_i(prop::TriggerEnd, r.s32_at(q + 12));
    }
    for (u32 k = 0; k < w[8]; ++k, q += 24) {
      Node& n = u.list(user::PropertyTriggers).append(std::make_unique<Node>(&S.ptrig));
      n.set_i(ptrig::Guid, r.u32_at(q));
      n.set_i(ptrig::AssetCall, call_of(r.u32_at(q + 8), r.u32_at(q + 4)));
      if (const u32 c = r.u32_at(q + 12); c != None32) {
        std::string prop_name;
        for (const auto& pr : u.list(user::Properties))
          if (pr->i(prop::TriggerStart) <= k && k <= pr->i(prop::TriggerEnd))
            prop_name = pr->s(prop::Name);
        n.set_i(ptrig::Condition, condition(c, CT_Switch, prop_name));
      }
      n.set_i(ptrig::Flag, r.u16_at(q + 16));
      n.set_i(ptrig::OverwriteHash, r.u16_at(q + 18));
      n.set_i(ptrig::Overwrite, overwrite(r.u32_at(q + 20)));
    }
    for (u32 k = 0; k < w[9]; ++k, q += 20) {
      Node& n = u.list(user::AlwaysTriggers).append(std::make_unique<Node>(&S.awtrig));
      n.set_i(awtrig::Guid, r.u32_at(q));
      n.set_i(awtrig::AssetCall, call_of(r.u32_at(q + 8), r.u32_at(q + 4)));
      n.set_i(awtrig::Flag, r.u16_at(q + 12));
      n.set_i(awtrig::OverwriteHash, r.u16_at(q + 14));
      n.set_i(awtrig::Overwrite, overwrite(r.u32_at(q + 16)));
    }
  }
  infer_switch_types();
  file.set_i(file::ConditionCountBias,
             static_cast<s64>(conditions_seen) - static_cast<s64>(file.list(file::Conditions).size()));

  // Arrange groups fill the region before the first user; reading it whole
  // keeps groups nothing points at yet.
  std::map<u64, int> arrange_index;
  u64 region_end = m_conditions;
  for (u32 ui = 0; ui < n_user; ++ui)
    region_end = std::min<u64>(region_end, r.u32_at(HeaderSize + 4 * ui));
  const auto read_group = [&](u64 off) {
    arrange_index[off] = file.list(file::ArrangeGroups).size();
    Node& grp = file.list(file::ArrangeGroups).append(std::make_unique<Node>(&S.arrange));
    u64 at = m_ex + off;
    const u32 count = r.u32_at(at);
    at += 4;
    for (u32 j = 0; j < count; ++j, at += 8) {
      Node& e = grp.list(arrange::Entries).append(std::make_unique<Node>(&S.arrange_entry));
      e.set_s(arrange_entry::GroupName, name(r.u32_at(at)));
      e.set_i(arrange_entry::LimitType, r.u8_at(at + 4));
      e.set_i(arrange_entry::LimitThreshold, r.u8_at(at + 5));
      e.set_i(arrange_entry::Unk, r.u8_at(at + 6));
      e.set_i(arrange_entry::Pad, r.u8_at(at + 7));
    }
    return at - m_ex;
  };
  bool whole = true;
  try {
    u64 off = 0;
    while (m_ex + off < region_end)
      off = read_group(off);
    whole = m_ex + off == region_end;
    for (Node* prm : m_arrange_params)
      whole = whole && arrange_index.contains(static_cast<u64>(prm->i(param::Value)));
  } catch (const Error&) {
    whole = false;
  }
  if (!whole) {
    file.list(file::ArrangeGroups).clear();
    arrange_index.clear();
    std::vector<u64> arrange;
    for (Node* prm : m_arrange_params)
      arrange.push_back(static_cast<u64>(prm->i(param::Value)));
    std::sort(arrange.begin(), arrange.end());
    arrange.erase(std::unique(arrange.begin(), arrange.end()), arrange.end());
    for (const u64 off : arrange)
      read_group(off);
  }
  for (Node* prm : m_arrange_params)
    prm->set_i(param::Value, arrange_index.at(static_cast<u64>(prm->i(param::Value))));

  file.set_i(file::System, static_cast<s64>(System::SLink));
  for (const auto& d : file.list(file::AssetParamDefs))
    if (d->s(pdef::Name) == "Eset")
      file.set_i(file::System, static_cast<s64>(System::ELink));
  doc.encoding = detect_encoding(file.s(file::NameTablePool), Encoding::ShiftJis);
  return doc;
}

// ---------------------------------------------------------------- writing

// Version 19 keeps no defaults, so a parameter a set leaves out is written
// as its definition's default from a later version, or zero.
u32 default_word(const Node& def, StringPool& names, u32& ref_type) {
  ref_type = RT_Direct;
  switch (def.i(pdef::Type)) {
    case PT_Float:
      return f32_bits(static_cast<f32>(def.f64v(pdef::Default)));
    case PT_String:
      ref_type = RT_String;
      return names.offset(def.s(pdef::Default));
    default:
      return static_cast<u32>(def.i(pdef::Default));
  }
}

class V19Writer {
public:
  explicit V19Writer(const Document& doc)
      : d(doc), file(*doc.root), w(doc.endian), m_names(file.s(file::NameTablePool)),
        m_cond_pool(file.s(file::ConditionPool)) {}
  Buffer run();

private:
  void param(const Node* p, const Node* def, s64 index);
  u32 condition_offset(s64 index);
  void write_user(const Node& u);

  const Document& d;
  const Node& file;
  Writer w;
  StringPool m_names;
  std::string m_cond_pool;
  std::map<s64, u32> m_cond_offsets;
  std::vector<u64> m_arrange_pos;
  u64 n_sets_bytes = 0;
};

void V19Writer::param(const Node* p, const Node* def, s64 index) {
  if (!p) {
    u32 rt = RT_Direct, v = 0;
    if (def)
      v = default_word(*def, m_names, rt);
    w.w32(rt);
    w.w32(v);
    return;
  }
  (void)index;
  const s64 rt = p->i(param::RefType);
  w.w32(static_cast<u32>(rt));
  if (rt == RT_String)
    w.w32(m_names.offset(p->s(param::Value)));
  else if (rt == RT_ArrangeGroup)
    w.w32(static_cast<u32>(m_arrange_pos.at(static_cast<std::size_t>(p->i(param::Value)))));
  else
    w.w32(static_cast<u32>(p->i(param::Value)));
}

// Conditions share identical byte runs anywhere in the pool, so an unchanged
// condition finds its original bytes again and new ones are appended.
u32 V19Writer::condition_offset(s64 index) {
  if (const auto it = m_cond_offsets.find(index); it != m_cond_offsets.end())
    return it->second;
  if (index >= file.list(file::Conditions).size())
    throw Error("condition " + std::to_string(index) + " does not exist");
  const Node& c = file.list(file::Conditions).at(static_cast<int>(index));
  Writer b(d.endian);
  const s64 type = c.i(cond::ParentType);
  switch (type) {
    case CT_Switch: {
      const s64 pt = c.i(cond::PropertyType);
      b.w32(0);
      b.w32(static_cast<u32>(c.i(cond::CompareType)));
      b.w32(pt == PR_Enum ? m_names.offset(c.s(cond::Value)) : static_cast<u32>(c.i(cond::Value)));
      break;
    }
    case CT_Random:
    case CT_Random2:
      b.w32(static_cast<u32>(c.i(cond::Weight)));
      break;
    case CT_Blend:
      b.w32(static_cast<u32>(c.i(cond::BlendMin)));
      break;
    default:
      b.ws32(static_cast<s32>(c.i(cond::ContinueOnFade)));
      break;
  }
  const std::string bytes(b.buffer().begin(), b.buffer().end());
  const s64 hint = c.i(cond::PoolOffset);
  if (hint >= 0 && static_cast<u64>(hint) + bytes.size() <= m_cond_pool.size() &&
      m_cond_pool.compare(static_cast<std::size_t>(hint), bytes.size(), bytes) == 0) {
    m_cond_offsets.emplace(index, static_cast<u32>(hint));
    return static_cast<u32>(hint);
  }
  std::size_t at = 0;
  for (;;) {
    at = m_cond_pool.find(bytes, at);
    if (at == std::string::npos || at % 4 == 0)
      break;
    ++at;
  }
  if (at == std::string::npos) {
    at = m_cond_pool.size();
    m_cond_pool += bytes;
  }
  m_cond_offsets.emplace(index, static_cast<u32>(at));
  return static_cast<u32>(at);
}

void V19Writer::write_user(const Node& u) {
  const std::size_t at = w.pos();
  const NodeList& calls = u.list(user::Calls);
  const NodeList& ctns = u.list(user::Containers);
  const int n_call = calls.size();
  u64 ctn_bytes = 0;
  std::vector<u64> ctn_pos;
  for (const auto& c : ctns) {
    ctn_pos.push_back(ctn_bytes);
    ctn_bytes += c->i(ctn::Type) == CT_Switch ? 24 : 12;
  }
  const u64 n_up = file.list(file::UserParamDefs).size();
  const u64 trigger_table = UserHeaderSize + 8 * n_up + 2 * u64(n_call) + (n_call & 1 ? 2 : 0) + CallSize * n_call + ctn_bytes;
  int assets = 0;
  for (const auto& c : calls)
    assets += !call_is_container(c->i(call::Flag));

  const s64 bias = file.i(file::ConditionCountBias);
  const s64 n_conds = static_cast<s64>(file.list(file::Conditions).size()) + bias;
  const u32 header[25] = {0,
                          static_cast<u32>(n_call),
                          static_cast<u32>(assets),
                          static_cast<u32>(std::max<s64>(n_conds, 0)),
                          static_cast<u32>(u.list(user::ActionSlots).size()),
                          static_cast<u32>(u.list(user::Actions).size()),
                          static_cast<u32>(u.list(user::ActionTriggers).size()),
                          static_cast<u32>(u.list(user::Properties).size()),
                          static_cast<u32>(u.list(user::PropertyTriggers).size()),
                          static_cast<u32>(u.list(user::AlwaysTriggers).size()),
                          0,
                          0,
                          0,
                          static_cast<u32>(file.list(file::Randoms).size()),
                          0,
                          static_cast<u32>(file.list(file::Curves).size()),
                          0,
                          0,
                          0,
                          static_cast<u32>(trigger_table),
                          0,
                          0,
                          0,
                          m_names.offset(u.s(user::Name)),
                          m_names.offset(u.s(user::BaseName))};
  for (const u32 v : header)
    w.w32(v);

  const NodeList& up = u.list(user::UserParams);
  for (u64 k = 0; k < n_up; ++k) {
    const Node* found = nullptr;
    for (const auto& p : up)
      if (p->i(param::Def) == static_cast<s64>(k)) {
        found = p.get();
        break;
      }
    param(found, &file.list(file::UserParamDefs).at(static_cast<int>(k)), static_cast<s64>(k));
  }

  // The same key-name order the later versions use, with ties kept as read.
  std::vector<int> order(n_call);
  {
    const auto key = [&](int k) -> const std::string& { return calls.at(k).s(call::KeyName); };
    const std::string& hint = u.s(user::SortedCallOrder);
    bool ok = hint.size() == 2 * static_cast<std::size_t>(n_call);
    if (ok) {
      Reader hr(Bytes(reinterpret_cast<const u8*>(hint.data()), hint.size()), d.endian);
      std::vector<bool> seen(n_call);
      for (int k = 0; k < n_call && ok; ++k) {
        order[k] = hr.u16_at(2 * k);
        ok = order[k] < n_call && !seen[order[k]] && (k == 0 || !(key(order[k]) < key(order[k - 1])));
        if (ok)
          seen[order[k]] = true;
      }
    }
    if (!ok) {
      for (int k = 0; k < n_call; ++k)
        order[k] = k;
      std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return key(a) < key(b); });
    }
  }
  for (const int k : order)
    w.w16(static_cast<u16>(k));
  if (n_call & 1)
    w.w16(0);

  for (const auto& c : calls) {
    w.w32(m_names.offset(c->s(call::KeyName)));
    w.ws32(static_cast<s32>(c->i(call::AssetId)));
    w.w32(static_cast<u32>(c->i(call::Guid)));
    w.ws32(static_cast<s32>(c->i(call::Duration)));
    w.ws32(static_cast<s32>(c->i(call::ParentIndex)));
    w.w32(static_cast<u32>(c->i(call::Flag)));
    if (call_is_container(c->i(call::Flag))) {
      const s64 ci = c->i(call::Container);
      w.w32(ci >= 0 && ci < static_cast<s64>(ctn_pos.size()) ? static_cast<u32>(ctn_pos[ci]) : None32);
    } else {
      const s64 si = c->i(call::ParamSet);
      w.w32(si >= 0 ? static_cast<u32>(si * n_sets_bytes) : 0);
    }
    const s64 ci = c->i(call::Condition);
    w.w32(ci >= 0 ? condition_offset(ci) : None32);
  }
  for (const auto& c : ctns) {
    const s64 type = c->i(ctn::Type);
    w.w32(to_v19_type(type));
    w.ws32(static_cast<s32>(c->i(ctn::ChildStart)));
    w.ws32(static_cast<s32>(c->i(ctn::ChildEnd)));
    if (type == CT_Switch) {
      w.w32(m_names.offset(c->s(ctn::WatchPropertyName)));
      w.ws32(static_cast<s32>(c->i(ctn::WatchPropertyId)));
      w.ws16(static_cast<s16>(c->i(ctn::LocalPropertyNameIdx)));
      w.w8(static_cast<u8>(c->i(ctn::IsGlobal)));
      w.w8(static_cast<u8>(c->i(ctn::SwitchByte)));
    }
  }
  if (w.pos() - at != trigger_table)
    throw Error("internal error: trigger table of " + u.label() + " misplaced");

  const u64 tset_bytes = 4 + 8 * u64(file.list(file::TriggerParamDefs).size());
  const auto call_ref = [&](s64 i) {
    if (i < 0 || i >= n_call)
      throw Error("a trigger in " + u.label() + " names call " + std::to_string(i) + " of " + std::to_string(n_call));
    w.w32(m_names.offset(calls.at(static_cast<int>(i)).s(call::KeyName)));
    w.w32(static_cast<u32>(i * CallSize));
  };
  const auto set_ref = [&](s64 i) { return i >= 0 ? static_cast<u32>(i * tset_bytes) : None32; };
  for (const auto& s : u.list(user::ActionSlots)) {
    w.w32(m_names.offset(s->s(slot::Name)));
    w.ws16(static_cast<s16>(s->i(slot::ActionStart)));
    w.ws16(static_cast<s16>(s->i(slot::ActionEnd)));
  }
  for (const auto& a : u.list(user::Actions)) {
    w.w32(m_names.offset(a->s(action::Name)));
    w.ws32(static_cast<s32>(a->i(action::TriggerStart)));
    w.ws32(static_cast<s32>(a->i(action::TriggerEnd)));
  }
  for (const auto& t : u.list(user::ActionTriggers)) {
    w.w32(static_cast<u32>(t->i(atrig::Guid)));
    call_ref(t->i(atrig::AssetCall));
    if (trigger_names_previous_action(t->i(atrig::Flag)))
      w.w32(m_names.offset(t->s(atrig::PreviousActionName)));
    else
      w.ws32(static_cast<s32>(t->i(atrig::StartFrame)));
    w.ws32(static_cast<s32>(t->i(atrig::EndFrame)));
    w.w16(static_cast<u16>(t->i(atrig::Flag)));
    w.w16(static_cast<u16>(t->i(atrig::OverwriteHash)));
    w.w32(set_ref(t->i(atrig::Overwrite)));
  }
  for (const auto& p : u.list(user::Properties)) {
    w.w32(m_names.offset(p->s(prop::Name)));
    w.ws32(static_cast<s32>(p->i(prop::IsGlobal)));
    w.ws32(static_cast<s32>(p->i(prop::TriggerStart)));
    w.ws32(static_cast<s32>(p->i(prop::TriggerEnd)));
  }
  for (const auto& t : u.list(user::PropertyTriggers)) {
    w.w32(static_cast<u32>(t->i(ptrig::Guid)));
    call_ref(t->i(ptrig::AssetCall));
    const s64 ci = t->i(ptrig::Condition);
    w.w32(ci >= 0 ? condition_offset(ci) : None32);
    w.w16(static_cast<u16>(t->i(ptrig::Flag)));
    w.w16(static_cast<u16>(t->i(ptrig::OverwriteHash)));
    w.w32(set_ref(t->i(ptrig::Overwrite)));
  }
  for (const auto& t : u.list(user::AlwaysTriggers)) {
    w.w32(static_cast<u32>(t->i(awtrig::Guid)));
    call_ref(t->i(awtrig::AssetCall));
    w.w16(static_cast<u16>(t->i(awtrig::Flag)));
    w.w16(static_cast<u16>(t->i(awtrig::OverwriteHash)));
    w.w32(set_ref(t->i(awtrig::Overwrite)));
  }
}

Buffer V19Writer::run() {
  const NodeList& users = file.list(file::Users);
  const NodeList& up = file.list(file::UserParamDefs);
  const NodeList& ap = file.list(file::AssetParamDefs);
  const NodeList& tp = file.list(file::TriggerParamDefs);
  n_sets_bytes = 8 * u64(ap.size());

  w.zeros(HeaderSize);
  const std::size_t offsets = w.pos();
  w.zeros(4 * u64(users.size()));
  for (const NodeList* l : {&up, &ap, &tp})
    for (const auto& def : *l) {
      w.w32(m_names.offset(def->s(pdef::Name)));
      w.w32(static_cast<u32>(def->i(pdef::Type)));
    }

  {
    u64 at = 0;
    for (const auto& grp : file.list(file::ArrangeGroups)) {
      m_arrange_pos.push_back(at);
      at += 4 + 8 * u64(grp->list(arrange::Entries).size());
    }
  }

  // Every asset carries its full parameter array, in definition order.
  const auto write_full = [&](const Node& set, const NodeList& defs) {
    for (int k = 0; k < defs.size(); ++k) {
      const Node* found = nullptr;
      for (const auto& p : set.list(pset::Params))
        if (p->i(param::Def) == k) {
          found = p.get();
          break;
        }
      param(found, &defs.at(k), k);
    }
  };
  for (const auto& set : file.list(file::AssetParamSets))
    write_full(*set, ap);
  for (const auto& set : file.list(file::TriggerParamSets)) {
    u32 mask = 0;
    for (const auto& p : set->list(pset::Params))
      if (p->i(param::Def) >= 0 && p->i(param::Def) < 32)
        mask |= 1u << p->i(param::Def);
    w.w32(mask);
    for (int k = 0; k < tp.size(); ++k) {
      const Node* found = nullptr;
      for (const auto& p : set->list(pset::Params))
        if (p->i(param::Def) == k) {
          found = p.get();
          break;
        }
      if (found) {
        param(found, &tp.at(k), k);
      } else {
        w.w32(0);
        w.w32(0);
      }
    }
  }
  for (const auto& rnd : file.list(file::Randoms)) {
    w.w32(static_cast<u32>(rnd->i(random::Min)));
    w.w32(static_cast<u32>(rnd->i(random::Max)));
  }
  u64 point_index = 0;
  for (const auto& c : file.list(file::Curves)) {
    const u64 count = c->list(curve::Points).size();
    w.w16(static_cast<u16>(point_index));
    w.w16(static_cast<u16>(count));
    w.w16(static_cast<u16>(c->i(curve::CurveType)));
    w.w16(static_cast<u16>(c->i(curve::IsPropGlobal)));
    w.w32(m_names.offset(c->s(curve::PropName)));
    w.ws32(static_cast<s32>(c->i(curve::PropIdx)));
    point_index += count;
  }
  for (const auto& c : file.list(file::Curves))
    for (const auto& pt : c->list(curve::Points)) {
      w.w32(static_cast<u32>(pt->i(point::X)));
      w.w32(static_cast<u32>(pt->i(point::Y)));
    }
  const std::size_t ex = w.pos();
  for (const auto& grp : file.list(file::ArrangeGroups)) {
    w.w32(static_cast<u32>(grp->list(arrange::Entries).size()));
    for (const auto& e : grp->list(arrange::Entries)) {
      w.w32(m_names.offset(e->s(arrange_entry::GroupName)));
      w.w8(static_cast<u8>(e->i(arrange_entry::LimitType)));
      w.w8(static_cast<u8>(e->i(arrange_entry::LimitThreshold)));
      w.w8(static_cast<u8>(e->i(arrange_entry::Unk)));
      w.w8(static_cast<u8>(e->i(arrange_entry::Pad)));
    }
  }
  for (int k = 0; k < users.size(); ++k) {
    w.patch_u32(offsets + 4 * k, static_cast<u32>(w.pos()));
    write_user(users.at(k));
  }
  const std::size_t conditions = w.pos();
  w.bytes(std::string_view(m_cond_pool));
  const std::size_t names = w.pos();
  w.bytes(m_names.bytes());
  w.align(4);

  u64 n_res = 0;
  n_res = u64(file.list(file::AssetParamSets).size()) * ap.size();
  const u32 header[16] = {0,
                          static_cast<u32>(w.pos()),
                          static_cast<u32>(file.i(file::Version)),
                          static_cast<u32>(up.size()),
                          static_cast<u32>(ap.size()),
                          static_cast<u32>(file.i(file::NumCustomAssetParam)),
                          static_cast<u32>(tp.size()),
                          static_cast<u32>(n_res),
                          static_cast<u32>(file.list(file::TriggerParamSets).size()),
                          static_cast<u32>(file.list(file::Randoms).size()),
                          static_cast<u32>(file.list(file::Curves).size()),
                          static_cast<u32>(point_index),
                          static_cast<u32>(users.size()),
                          static_cast<u32>(conditions),
                          static_cast<u32>(ex),
                          static_cast<u32>(names)};
  for (int k = 1; k < 16; ++k)
    w.patch_u32(4 * k, header[k]);
  std::copy_n("XLNK", 4, w.buffer().begin());
  return std::move(w.buffer());
}

}  // namespace

Document read(Bytes data) {
  return V19Reader(data).run();
}

Buffer write(const Document& doc) {
  return V19Writer(doc).run();
}

}  // namespace xl::xlink_v19
