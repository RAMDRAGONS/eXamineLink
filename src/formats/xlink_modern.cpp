#include <algorithm>
#include <bit>
#include <map>
#include <set>

#include "core/crc32.hpp"
#include "formats/codecs.hpp"
#include "formats/string_pool.hpp"
#include "formats/xlink_words.hpp"
#include "model/xlink_schema.hpp"

// Layout of XLink resource versions 27 to 36, as read by
// xlink2::ResourceParamCreator: a header, the user name hashes and offsets, the
// parameter definition table, the common tables (asset and trigger parameter
// sets, local property names, direct values, random and curve tables, the
// arrange-group region), every user's block, the condition table and the name
// table. 64-bit builds widen every stored position to 8 bytes.
namespace xl::xlink_modern {

using namespace xlink;

int switch_layout(u32 version, int system);

namespace {

constexpr u32 None32 = 0xFFFFFFFF;

struct Geometry {
   bool wide;
   bool user_locals;
   bool global_count;
   // 0: 32-bit words; 1: byte-sized type fields then value and enum index;
   // 2 (64-bit): enum index, then the value or an 8-byte enum name position.
   int switch_layout;
   u64 header() const
   {
      return wide ? 0x60 : 0x48;
   }
   u64 ptr() const
   {
      return wide ? 8 : 4;
   }
   u64 align() const
   {
      return wide ? 8 : 4;
   }
   u64 pdt_header() const
   {
      return wide ? 0x18 : 0x14;
   }
   u64 def_size() const
   {
      return wide ? 0x18 : 0xC;
   }
   u64 curve_size() const
   {
      return wide ? 0x18 : 0x14;
   }
   u64 arrange_entry() const
   {
      return wide ? 0x10 : 0x8;
   }
   u64 user_header() const
   {
      return wide ? 0x38 : user_locals ? 0x30 : 0x2C;
   }
   u64 call_size() const
   {
      return wide ? 0x30 : 0x20;
   }
   u64 slot_size() const
   {
      return wide ? 0x10 : 0x8;
   }
   u64 action_size() const
   {
      return wide ? 0x10 : 0xC;
   }
   u64 atrig_size() const
   {
      return wide ? 0x28 : 0x18;
   }
   u64 prop_size() const
   {
      return wide ? 0x18 : 0x10;
   }
   u64 ptrig_size() const
   {
      return wide ? 0x20 : 0x14;
   }
   u64 awtrig_size() const
   {
      return wide ? 0x18 : 0x10;
   }
};

Geometry
geometry(u32 version, int system)
{
   return {is_wide(version, system), has_user_locals(version, system), has_global_count(version, system),
           switch_layout(version, system)};
}

u64
align_up(u64 v, u64 a)
{
   return (v + a - 1) / a * a;
}

bool
has_switch_params(s64 type, bool blend_by)
{
   return type == CT_Switch || (type == CT_Blend && blend_by);
}

u64
container_size(const Geometry &g, s64 type, bool blend_by, u64 n1, u64 n2)
{
   if (has_switch_params(type, blend_by))
      return g.wide ? 0x20 : 0x18;
   if (type == CT_Grid && g.wide)
      return 0x28 + 4 * (n1 + n2 + n1 * n2);
   return g.wide ? 0x10 : 0xC;
}

u64
condition_size(const Geometry &g, const Node &c)
{
   switch (c.i(cond::ParentType)) {
   case CT_Switch:
      if (g.switch_layout == 2)
         return c.i(cond::PropertyType) == PR_Enum ? 0x18 : 0x10;
      return g.switch_layout == 1 ? 0x10 : 0x14;
   case CT_Blend:
      return 0x10;
   default:
      return 0x8;
   }
}

// ---------------------------------------------------------------- reading

class ModernReader {
public:
   explicit ModernReader(Bytes data): r(data, Endian::Little)
   {
      r.require(0, 0x48, "XLink header");
      if (!r.magic_at(0, "XLNK"))
         throw Error("not an XLink database");
      if (r.u32_at(0x8) > 0xFFFF)
         r.set_endian(Endian::Big);
      // A 64-bit trigger parameter set position at 0x18 leaves one half zero,
      // where the 32-bit layout keeps two nonzero positions; the version alone
      // cannot tell, as ELink2 31 is 32-bit and SLink2 31 64-bit.
      g.wide = r.u32_at(0x18) == 0 || r.u32_at(0x1C) == 0;
   }

   Document run();

private:
   u64 ptr(u64 at) const
   {
      return g.wide ? r.u64_at(at) : r.u32_at(at);
   }
   std::string name(u64 off) const
   {
      return read_pool_string(r, m_names, off, "name table");
   }
   void read_pdt(Node &file, u64 pdt);
   std::unique_ptr<Node> make_param(u32 raw, int def);
   u64 read_param_sets(Node &file, int list, u64 start, u32 count, bool wide_mask, std::map<u64, int> &offsets);
   void read_conditions(Node &file, u64 start, u64 end);
   void read_user(Node &file, Node &user, u64 at, u32 user_params);
   void read_containers(Node &user, u64 start, u64 end, std::map<u64, int> &offsets);

   Reader r;
   Geometry g{};
   u64 m_names = 0;
   std::vector<u32> m_direct;
   std::map<u64, int> m_asset_sets, m_trigger_sets, m_conditions;
   // Parameters that name an arrange group by its byte offset until the region is read.
   std::vector<Node *> m_arrange_params;
   // Calls and triggers whose positions resolve once every table is known.
   struct Pending {
      Node *node;
      int field;
      u64 offset;
      const std::map<u64, int> *map;
      const u64 *table_size;
   };
   u64 m_asset_sets_size = 0, m_trigger_sets_size = 0, m_conditions_size = 0;
   std::vector<Pending> m_pending;
};

std::unique_ptr<Node>
ModernReader::make_param(u32 raw, int def)
{
   auto p = std::make_unique<Node>(&schemas().param);
   p->set_i(param::Def, def);
   read_param_word(*p, raw, m_direct, [this](u64 off) { return name(off); });
   if (p->i(param::RefType) == RT_ArrangeGroup)
      m_arrange_params.push_back(p.get());
   return p;
}

void
ModernReader::read_pdt(Node &file, u64 pdt)
{
   const auto &S = schemas();
   const u32 size = r.u32_at(pdt);
   const u32 counts[3] = {r.u32_at(pdt + 4), r.u32_at(pdt + 8), r.u32_at(pdt + 16)};
   file.set_i(file::NumCustomAssetParam, r.u32_at(pdt + 12));
   const u64 defs = pdt + g.pdt_header();
   const u64 total = u64{counts[0]} + counts[1] + counts[2];
   const u64 strings = defs + total * g.def_size();
   if (strings > pdt + size)
      throw Error("parameter definitions overrun their table");
   const auto bytes = r.slice(strings, pdt + size - strings, "parameter definition names");
   file.set_s(file::PdtStringPool, std::string(bytes.begin(), bytes.end()));
   const int lists[3] = {file::UserParamDefs, file::AssetParamDefs, file::TriggerParamDefs};
   u64 at = defs;
   for (int k = 0; k < 3; ++k) {
      for (u32 i = 0; i < counts[k]; ++i, at += g.def_size()) {
         Node &d = file.list(lists[k]).append(std::make_unique<Node>(&S.pdef));
         const u64 name_off = ptr(at);
         const u32 type = r.u32_at(at + (g.wide ? 8 : 4));
         const u64 raw = g.wide ? r.u64_at(at + 16) : r.u32_at(at + 8);
         d.set_s(pdef::Name, read_pool_string(r, strings, name_off, "parameter name"));
         d.set_i(pdef::Type, type);
         if (type == PT_String)
            d.set_s(pdef::Default, read_pool_string(r, strings, raw, "parameter default"));
         else if (type == PT_Float)
            d.set_f64(pdef::Default, g.wide ? std::bit_cast<f64>(raw) : f64(bits_f32(static_cast<u32>(raw))));
         else
            d.set_i(pdef::Default, g.wide ? static_cast<s64>(raw) : static_cast<s32>(raw));
      }
   }
}

u64
ModernReader::read_param_sets(Node &file, int list, u64 start, u32 count, bool wide_mask, std::map<u64, int> &offsets)
{
   const auto &S = schemas();
   u64 at = start;
   for (u32 i = 0; i < count; ++i) {
      offsets[at - start] = static_cast<int>(i);
      const u64 mask = wide_mask ? r.u64_at(at) : r.u32_at(at);
      at += wide_mask ? 8 : 4;
      Node &set = file.list(list).append(std::make_unique<Node>(&S.pset));
      for (int bit = 0; bit < (wide_mask ? 64 : 32); ++bit) {
         if (!(mask >> bit & 1))
            continue;
         set.list(pset::Params).append(make_param(r.u32_at(at), bit));
         at += 4;
      }
   }
   return at - start;
}

void
ModernReader::read_conditions(Node &file, u64 start, u64 end)
{
   const auto &S = schemas();
   u64 at = start;
   while (at < end) {
      m_conditions[at - start] = file.list(file::Conditions).size();
      Node &c = file.list(file::Conditions).append(std::make_unique<Node>(&S.cond));
      const u32 type = r.u32_at(at);
      c.set_i(cond::ParentType, type);
      switch (type) {
      case CT_Switch:
         if (g.switch_layout == 1) {
            const u8 pt = r.u8_at(at + 4);
            c.set_i(cond::PropertyType, pt);
            c.set_i(cond::CompareType, r.u8_at(at + 5));
            c.set_i(cond::IsSolved, r.u8_at(at + 6));
            c.set_i(cond::IsGlobal, r.u8_at(at + 7));
            if (pt == PR_Enum)
               c.set_s(cond::Value, name(r.u32_at(at + 8)));
            else
               c.set_i(cond::Value, r.u32_at(at + 8));
            c.set_i(cond::LocalPropertyEnumNameIdx, r.s32_at(at + 12));
         } else if (g.wide) {
            const u8 pt = r.u8_at(at + 4);
            c.set_i(cond::PropertyType, pt);
            c.set_i(cond::CompareType, r.u8_at(at + 5));
            c.set_i(cond::IsSolved, r.u8_at(at + 6));
            c.set_i(cond::IsGlobal, r.u8_at(at + 7));
            c.set_i(cond::LocalPropertyEnumNameIdx, r.s32_at(at + 8));
            if (pt == PR_Enum)
               c.set_s(cond::Value, name(r.u64_at(at + 16)));
            else
               c.set_i(cond::Value, r.u32_at(at + 12));
         } else {
            const u32 pt = r.u32_at(at + 4);
            c.set_i(cond::PropertyType, pt);
            c.set_i(cond::CompareType, r.u32_at(at + 8));
            if (pt == PR_Enum)
               c.set_s(cond::Value, name(r.u32_at(at + 12)));
            else
               c.set_i(cond::Value, r.u32_at(at + 12));
            c.set_i(cond::LocalPropertyEnumNameIdx, r.s16_at(at + 16));
            c.set_i(cond::IsSolved, r.u8_at(at + 18));
            c.set_i(cond::IsGlobal, r.u8_at(at + 19));
         }
         break;
      case CT_Random:
      case CT_Random2:
         c.set_i(cond::Weight, r.u32_at(at + 4));
         break;
      case CT_Blend:
         c.set_i(cond::BlendMin, r.u32_at(at + 4));
         c.set_i(cond::BlendMax, r.u32_at(at + 8));
         c.set_i(cond::BlendTypeToMax, r.u8_at(at + 12));
         c.set_i(cond::BlendTypeToMin, r.u8_at(at + 13));
         break;
      default:
         c.set_i(cond::ContinueOnFade, r.s32_at(at + 4));
         break;
      }
      at += condition_size(g, c);
   }
}

void
ModernReader::read_containers(Node &user, u64 start, u64 end, std::map<u64, int> &offsets)
{
   const auto &S = schemas();
   u64 at = start;
   while (at < end) {
      offsets[at - start] = user.list(user::Containers).size();
      Node &c = user.list(user::Containers).append(std::make_unique<Node>(&S.ctn));
      // 64-bit versions narrow the type to a byte; the next marks a blend
      // container that blends by a property, and the two after it are
      // written at run time.
      const u32 type = g.wide ? r.u8_at(at) : r.u32_at(at);
      const bool blend_by = g.wide && r.u8_at(at + 1) != 0;
      c.set_i(ctn::Type, type);
      c.set_i(ctn::BlendByProperty, blend_by);
      c.set_i(ctn::ChildStart, r.s32_at(at + 4));
      c.set_i(ctn::ChildEnd, r.s32_at(at + 8));
      u64 n1 = 0, n2 = 0;
      if (has_switch_params(type, blend_by)) {
         const u64 p = g.wide ? at + 16 : at + 12;
         c.set_s(ctn::WatchPropertyName, name(ptr(p)));
         c.set_i(ctn::WatchPropertyId, r.s32_at(p + g.ptr()));
         c.set_i(ctn::LocalPropertyNameIdx, r.s16_at(p + g.ptr() + 4));
         c.set_i(ctn::IsGlobal, r.u8_at(p + g.ptr() + 6));
         c.set_i(ctn::SwitchByte, r.u8_at(p + g.ptr() + 7));
      } else if (type == CT_Grid && g.wide) {
         c.set_s(ctn::WatchPropertyName, name(r.u64_at(at + 16)));
         c.set_s(ctn::WatchPropertyName2, name(r.u64_at(at + 24)));
         c.set_i(ctn::LocalPropertyNameIdx, r.s16_at(at + 32));
         c.set_i(ctn::LocalPropertyNameIdx2, r.s16_at(at + 34));
         c.set_i(ctn::GridFlags1, r.u8_at(at + 36));
         c.set_i(ctn::GridFlags2, r.u8_at(at + 37));
         n1 = r.u8_at(at + 38);
         n2 = r.u8_at(at + 39);
         u64 p = at + 40;
         for (u64 k = 0; k < n1; ++k, p += 4)
            c.list(ctn::Axis1).append_new().set_i(grid_value::Value, r.u32_at(p));
         for (u64 k = 0; k < n2; ++k, p += 4)
            c.list(ctn::Axis2).append_new().set_i(grid_value::Value, r.u32_at(p));
         for (u64 k = 0; k < n1 * n2; ++k, p += 4)
            c.list(ctn::Cells).append_new().set_i(grid_cell::Call, r.s32_at(p));
      }
      at += container_size(g, type, blend_by, n1, n2);
   }
   if (at != end)
      throw Error("container table of user " + user.label() + " does not end at its trigger table");
}

void
ModernReader::read_user(Node &file, Node &user, u64 at, u32 user_params)
{
   (void)file;
   const auto &S = schemas();
   // Counts in the order of a header with local properties; one without them
   // lacks that count.
   u32 w[11] = {};
   for (int k = 0, j = 0; k < 11; ++k)
      if (k != 1 || g.user_locals)
         w[k] = r.u32_at(at + 4 * j++);
   if (g.global_count) {
      w[1] = r.u16_at(at + 4);
      user.set_i(user::GlobalPropertyCount, r.u16_at(at + 6));
   }
   const u64 trigger_table = at + (g.wide ? r.u64_at(at + 0x30) : r.u32_at(at + g.user_header() - 4));
   const u32 n_local = w[1], n_call = w[2];
   u64 p = at + g.user_header();
   for (u32 k = 0; k < n_local; ++k, p += g.ptr())
      user.list(user::LocalProperties).append(std::make_unique<Node>(&S.name)).set_s(name::Name, name(ptr(p)));
   for (u32 k = 0; k < user_params; ++k, p += 4)
      user.list(user::UserParams).append(make_param(r.u32_at(p), static_cast<int>(k)));
   {
      const auto t = r.slice(p, 2 * u64{n_call}, "sorted call order");
      user.set_s(user::SortedCallOrder, std::string(t.begin(), t.end()));
   }
   p += 2 * u64{n_call} + (n_call & 1 ? 2 : 0);
   const u64 calls = p;
   const u64 containers = calls + n_call * g.call_size();
   std::map<u64, int> container_offsets;
   read_containers(user, containers, trigger_table, container_offsets);

   for (u32 k = 0; k < n_call; ++k) {
      const u64 c = calls + k * g.call_size();
      Node &n = user.list(user::Calls).append(std::make_unique<Node>(&S.call));
      const u64 o = g.wide ? 8 : 4;
      n.set_s(call::KeyName, name(ptr(c)));
      n.set_i(call::AssetId, r.s16_at(c + o));
      n.set_i(call::Flag, r.u16_at(c + o + 2));
      n.set_i(call::Duration, r.s32_at(c + o + 4));
      n.set_i(call::ParentIndex, r.s32_at(c + o + 8));
      n.set_i(call::Guid, r.u32_at(c + o + 12));
      n.set_i(call::KeyNameHash, r.u32_at(c + o + 16));
      const u64 param_at = g.wide ? c + 32 : c + 24;
      const u64 cond_at = g.wide ? c + 40 : c + 28;
      // 64-bit positions hold "none" as a 32-bit -1, so both widths fit 32 bits.
      const u32 param = static_cast<u32>(ptr(param_at));
      const u32 condition = static_cast<u32>(ptr(cond_at));
      if (call_is_container(n.i(call::Flag))) {
         if (param != None32) {
            const auto it = container_offsets.find(param);
            if (it == container_offsets.end())
               throw Error("call " + n.s(call::KeyName) + " points between containers");
            n.set_i(call::Container, it->second);
         }
      } else {
         m_pending.push_back({&n, call::ParamSet, param, &m_asset_sets, &m_asset_sets_size});
      }
      if (condition != None32)
         m_pending.push_back({&n, call::Condition, condition, &m_conditions, &m_conditions_size});
   }

   // Trigger tables point at calls by their byte position in the call table.
   const auto call_index = [&](u64 pos) -> s64 {
      if (pos % g.call_size() != 0 || pos / g.call_size() >= n_call)
         throw Error("trigger in user " + user.label() + " points between calls");
      return static_cast<s64>(pos / g.call_size());
   };
   const auto overwrite = [&](Node &n, int f, u64 at_) {
      const u32 v = static_cast<u32>(ptr(at_));
      if (v != None32)
         m_pending.push_back({&n, f, v, &m_trigger_sets, &m_trigger_sets_size});
   };
   p = trigger_table;
   for (u32 k = 0; k < w[5]; ++k, p += g.slot_size()) {
      Node &n = user.list(user::ActionSlots).append(std::make_unique<Node>(&S.slot));
      n.set_s(slot::Name, name(ptr(p)));
      n.set_i(slot::ActionStart, r.s16_at(p + g.ptr()));
      n.set_i(slot::ActionEnd, r.s16_at(p + g.ptr() + 2));
   }
   for (u32 k = 0; k < w[6]; ++k, p += g.action_size()) {
      Node &n = user.list(user::Actions).append(std::make_unique<Node>(&S.action));
      n.set_s(action::Name, name(ptr(p)));
      if (g.wide) {
         n.set_i(action::TriggerStart, r.s16_at(p + 8));
         n.set_i(action::Flag, r.u16_at(p + 10));
      } else {
         n.set_i(action::TriggerStart, r.s32_at(p + 4));
      }
      n.set_i(action::TriggerEnd, r.s32_at(p + g.ptr() + 4));
   }
   for (u32 k = 0; k < w[7]; ++k, p += g.atrig_size()) {
      Node &n = user.list(user::ActionTriggers).append(std::make_unique<Node>(&S.atrig));
      n.set_i(atrig::Guid, r.u32_at(p));
      const u64 q = g.wide ? p + 8 : p + 4;
      n.set_i(atrig::AssetCall, call_index(ptr(q)));
      const u64 start = ptr(q + g.ptr());
      const u64 rest = q + 2 * g.ptr();
      n.set_i(atrig::EndFrame, r.s32_at(rest));
      n.set_i(atrig::Flag, r.u16_at(rest + 4));
      n.set_i(atrig::OverwriteHash, r.u16_at(rest + 6));
      if (trigger_names_previous_action(n.i(atrig::Flag)))
         n.set_s(atrig::PreviousActionName, name(start));
      else
         n.set_i(atrig::StartFrame, static_cast<s32>(start));
      overwrite(n, atrig::Overwrite, rest + 8);
   }
   for (u32 k = 0; k < w[8]; ++k, p += g.prop_size()) {
      Node &n = user.list(user::Properties).append(std::make_unique<Node>(&S.prop));
      n.set_s(prop::Name, name(ptr(p)));
      n.set_i(prop::IsGlobal, r.s32_at(p + g.ptr()));
      n.set_i(prop::TriggerStart, r.s32_at(p + g.ptr() + 4));
      n.set_i(prop::TriggerEnd, r.s32_at(p + g.ptr() + 8));
   }
   for (u32 k = 0; k < w[9]; ++k, p += g.ptrig_size()) {
      Node &n = user.list(user::PropertyTriggers).append(std::make_unique<Node>(&S.ptrig));
      n.set_i(ptrig::Guid, r.u32_at(p));
      if (g.wide) {
         n.set_i(ptrig::Flag, r.u16_at(p + 4));
         n.set_i(ptrig::OverwriteHash, r.u16_at(p + 6));
         n.set_i(ptrig::AssetCall, call_index(r.u64_at(p + 8)));
         if (const u32 c = static_cast<u32>(r.u64_at(p + 16)); c != None32)
            m_pending.push_back({&n, ptrig::Condition, c, &m_conditions, &m_conditions_size});
         overwrite(n, ptrig::Overwrite, p + 24);
      } else {
         n.set_i(ptrig::AssetCall, call_index(r.u32_at(p + 4)));
         if (const u32 c = r.u32_at(p + 8); c != None32)
            m_pending.push_back({&n, ptrig::Condition, c, &m_conditions, &m_conditions_size});
         n.set_i(ptrig::Flag, r.u16_at(p + 12));
         n.set_i(ptrig::OverwriteHash, r.u16_at(p + 14));
         overwrite(n, ptrig::Overwrite, p + 16);
      }
   }
   for (u32 k = 0; k < w[10]; ++k, p += g.awtrig_size()) {
      Node &n = user.list(user::AlwaysTriggers).append(std::make_unique<Node>(&S.awtrig));
      n.set_i(awtrig::Guid, r.u32_at(p));
      if (g.wide) {
         n.set_i(awtrig::Flag, r.u16_at(p + 4));
         n.set_i(awtrig::OverwriteHash, r.u16_at(p + 6));
         n.set_i(awtrig::AssetCall, call_index(r.u64_at(p + 8)));
         overwrite(n, awtrig::Overwrite, p + 16);
      } else {
         n.set_i(awtrig::AssetCall, call_index(r.u32_at(p + 4)));
         n.set_i(awtrig::Flag, r.u16_at(p + 8));
         n.set_i(awtrig::OverwriteHash, r.u16_at(p + 10));
         overwrite(n, awtrig::Overwrite, p + 12);
      }
   }
}

Document
ModernReader::run()
{
   const auto &S = schemas();
   Document doc;
   doc.endian = r.endian();
   doc.root = std::make_unique<Node>(&S.file);
   Node &file = *doc.root;

   const u32 data_size = r.u32_at(0x4);
   const u32 version = r.u32_at(0x8);
   file.set_i(file::Version, version);
   const u32 n_asset_sets = r.u32_at(0x10), n_trigger_sets = r.u32_at(0x14);
   const u64 trigger_sets = ptr(0x18);
   const u64 local = ptr(g.wide ? 0x20 : 0x1C);
   const u64 cb = g.wide ? 0x28 : 0x20;
   const u32 n_local = r.u32_at(cb), n_enum = r.u32_at(cb + 4), n_direct = r.u32_at(cb + 8),
             n_random = r.u32_at(cb + 12), n_curve = r.u32_at(cb + 16), n_point = r.u32_at(cb + 20);
   const u64 ex_region = ptr(g.wide ? 0x40 : 0x38);
   const u32 n_user = r.u32_at(g.wide ? 0x48 : 0x3C);
   const u64 conditions = ptr(g.wide ? 0x50 : 0x40);
   m_names = ptr(g.wide ? 0x58 : 0x44);
   const u64 end = std::min<u64>(data_size, r.size());
   if (m_names > end || conditions > m_names)
      throw Error("XLink table positions are out of order");
   {
      const auto t = r.slice(m_names, end - m_names, "name table");
      file.set_s(file::NameTablePool, std::string(t.begin(), t.end()));
   }

   const u64 hashes = g.header();
   const u64 offsets = g.wide ? align_up(hashes + 4 * u64{n_user}, 8) : hashes + 4 * u64{n_user};
   const u64 pdt = offsets + n_user * g.ptr();
   read_pdt(file, pdt);
   {
      const int system = static_cast<int>(system_of(file));
      doc.format = xlink_format(system);
      g.user_locals = has_user_locals(version, system);
      g.global_count = has_global_count(version, system);
      g.switch_layout = g.wide ? 2 : switch_layout(version, system);
   }
   const u32 n_user_params = static_cast<u32>(file.list(file::UserParamDefs).size());

   // Direct values come before the parameters that index them.
   u64 p = local + (u64{n_local} + n_enum) * g.ptr();
   {
      const auto t = r.slice(p, 4 * u64{n_direct}, "direct value table");
      file.set_s(file::DirectValuePool, std::string(t.begin(), t.end()));
   }
   m_direct.resize(n_direct);
   for (u32 k = 0; k < n_direct; ++k)
      m_direct[k] = r.u32_at(p + 4 * k);

   const u64 asset_sets = align_up(pdt + r.u32_at(pdt), g.align());
   m_asset_sets_size = read_param_sets(file, file::AssetParamSets, asset_sets, n_asset_sets, true, m_asset_sets);
   m_trigger_sets_size =
      read_param_sets(file, file::TriggerParamSets, trigger_sets, n_trigger_sets, false, m_trigger_sets);

   p = local;
   for (u32 k = 0; k < n_local; ++k, p += g.ptr())
      file.list(file::LocalPropertyNames).append(std::make_unique<Node>(&S.name)).set_s(name::Name, name(ptr(p)));
   for (u32 k = 0; k < n_enum; ++k, p += g.ptr())
      file.list(file::LocalPropertyEnumNames).append(std::make_unique<Node>(&S.name)).set_s(name::Name, name(ptr(p)));
   p += 4 * u64{n_direct};
   for (u32 k = 0; k < n_random; ++k, p += 8) {
      Node &n = file.list(file::Randoms).append(std::make_unique<Node>(&S.random));
      n.set_i(random::Min, r.u32_at(p));
      n.set_i(random::Max, r.u32_at(p + 4));
   }
   const u64 points = p + n_curve * g.curve_size();
   for (u32 k = 0; k < n_curve; ++k, p += g.curve_size()) {
      Node &n = file.list(file::Curves).append(std::make_unique<Node>(&S.curve));
      const u16 first = r.u16_at(p), count = r.u16_at(p + 2);
      if (u64{first} + count > n_point)
         throw Error("curve " + std::to_string(k) + " runs past the curve point table");
      n.set_i(curve::CurveType, r.u16_at(p + 4));
      n.set_i(curve::IsPropGlobal, r.u16_at(p + 6));
      n.set_s(curve::PropName, name(ptr(p + 8)));
      const u64 q = p + 8 + g.ptr();
      n.set_i(curve::PropIdx, r.s32_at(q));
      n.set_i(curve::LocalPropertyNameIdx, r.s16_at(q + 4));
      n.set_i(curve::UpdateType, r.s16_at(q + 6));
      for (u32 j = 0; j < count; ++j) {
         Node &pt = n.list(curve::Points).append(std::make_unique<Node>(&S.point));
         pt.set_i(point::X, r.u32_at(points + 8 * (u64{first} + j)));
         pt.set_i(point::Y, r.u32_at(points + 8 * (u64{first} + j) + 4));
      }
   }

   read_conditions(file, conditions, m_names);
   m_conditions_size = m_names - conditions;

   std::vector<u64> user_offsets(n_user);
   for (u32 k = 0; k < n_user; ++k)
      user_offsets[k] = ptr(offsets + k * g.ptr());
   for (u32 k = 0; k < n_user; ++k) {
      Node &u = file.list(file::Users).append(std::make_unique<Node>(&S.user));
      u.set_i(user::Hash, r.u32_at(hashes + 4 * k));
      read_user(file, u, user_offsets[k], n_user_params);
   }

   // Arrange groups fill the region between the common tables and the users;
   // reading it whole keeps groups nothing points at yet.
   std::map<u64, int> arrange_index;
   u64 region_end = r.size();
   for (const u64 o : user_offsets)
      region_end = std::min(region_end, o);
   const auto read_group = [&](u64 off) {
      arrange_index[off] = file.list(file::ArrangeGroups).size();
      Node &grp = file.list(file::ArrangeGroups).append(std::make_unique<Node>(&S.arrange));
      u64 at = ex_region + off;
      const u32 count = r.u32_at(at);
      at += 4;
      for (u32 j = 0; j < count; ++j, at += g.arrange_entry()) {
         Node &e = grp.list(arrange::Entries).append(std::make_unique<Node>(&S.arrange_entry));
         e.set_s(arrange_entry::GroupName, name(ptr(at)));
         e.set_i(arrange_entry::LimitType, r.u8_at(at + g.ptr()));
         e.set_i(arrange_entry::LimitThreshold, r.u8_at(at + g.ptr() + 1));
         e.set_i(arrange_entry::Unk, r.u8_at(at + g.ptr() + 2));
         e.set_i(arrange_entry::Pad, r.u8_at(at + g.ptr() + 3));
      }
      return at - ex_region;
   };
   bool whole = true;
   try {
      u64 off = 0;
      while (ex_region + off < region_end)
         off = read_group(off);
      whole = ex_region + off == region_end;
      for (Node *prm : m_arrange_params)
         whole = whole && arrange_index.contains(static_cast<u64>(prm->i(param::Value)));
   } catch (const Error &) {
      whole = false;
   }
   if (!whole) {
      // Not a plain run of groups: keep just the ones parameters point at.
      file.list(file::ArrangeGroups).clear();
      arrange_index.clear();
      std::vector<u64> arrange;
      for (Node *prm : m_arrange_params)
         arrange.push_back(static_cast<u64>(prm->i(param::Value)));
      std::sort(arrange.begin(), arrange.end());
      arrange.erase(std::unique(arrange.begin(), arrange.end()), arrange.end());
      for (const u64 off : arrange)
         read_group(off);
   }
   for (Node *prm : m_arrange_params)
      prm->set_i(param::Value, arrange_index.at(static_cast<u64>(prm->i(param::Value))));

   for (const Pending &pd : m_pending) {
      if (pd.offset == *pd.table_size) {
         pd.node->set_i(pd.field, RefTableEnd);
         continue;
      }
      const auto it = pd.map->find(pd.offset);
      if (it == pd.map->end())
         throw Error("a reference in " + pd.node->label() + " points between table entries (" + hex(pd.offset) + ")");
      pd.node->set_i(pd.field, it->second);
   }

   doc.encoding = detect_encoding(file.s(file::NameTablePool), g.wide ? Encoding::Utf8 : Encoding::ShiftJis);
   return doc;
}

// ---------------------------------------------------------------- writing

class ModernWriter {
public:
   ModernWriter(const Document &doc, u32 version)
       : d(doc), file(*doc.root), w(doc.endian), g{geometry(version, xlink_system(doc.format))},
         m_names(file.s(file::NameTablePool)), m_direct(file.s(file::DirectValuePool), doc.endian)
   {
   }

   Buffer run();

private:
   void wptr(u64 v)
   {
      g.wide ? w.w64(v) : w.w32(static_cast<u32>(v));
   }
   void wstr(const std::string &s)
   {
      wptr(m_names.offset(s));
   }
   // Positions in 64-bit layouts keep "none" as a 32-bit -1.
   void wpos(s64 v)
   {
      g.wide ? w.w64(v < 0 ? None32 : static_cast<u64>(v)) : w.w32(v < 0 ? None32 : static_cast<u32>(v));
   }
   u32 word(const Node &p)
   {
      return param_word(p, m_names, m_direct, m_arrange_pos);
   }
   void write_pdt();
   void write_user(const Node &u, std::size_t at);

   const Document &d;
   const Node &file;
   Writer w;
   Geometry g;
   StringPool m_names;
   DirectValues m_direct;
   std::vector<u64> m_asset_set_pos, m_trigger_set_pos, m_condition_pos, m_arrange_pos;
   u64 m_asset_sets_end = 0, m_trigger_sets_end = 0, m_conditions_end = 0;
   // Position of entry `i` of a table, its end for RefTableEnd, or -1.
   static s64 table_pos(const std::vector<u64> &pos, u64 end, s64 i)
   {
      if (i == RefTableEnd)
         return static_cast<s64>(end);
      return i >= 0 && i < static_cast<s64>(pos.size()) ? static_cast<s64>(pos[i]) : -1;
   }
   std::vector<int> sorted_calls(const Node &u) const;
};

void
ModernWriter::write_pdt()
{
   const std::size_t pdt = w.pos();
   const NodeList *lists[3] = {&file.list(file::UserParamDefs), &file.list(file::AssetParamDefs),
                               &file.list(file::TriggerParamDefs)};
   // An empty seed is rebuilt the way the shipped tables are: sorted, with the
   // empty string first.
   std::string seed = file.s(file::PdtStringPool);
   if (seed.empty()) {
      std::vector<std::string> all{""};
      for (const NodeList *l : lists)
         for (const auto &def : *l) {
            all.push_back(def->s(pdef::Name));
            if (def->i(pdef::Type) == PT_String)
               all.push_back(def->s(pdef::Default));
         }
      std::sort(all.begin(), all.end());
      all.erase(std::unique(all.begin(), all.end()), all.end());
      for (const auto &s : all)
         seed += s + '\0';
   }
   StringPool strings(seed);
   w.w32(0);
   w.w32(static_cast<u32>(lists[0]->size()));
   w.w32(static_cast<u32>(lists[1]->size()));
   w.w32(static_cast<u32>(file.i(file::NumCustomAssetParam)));
   w.w32(static_cast<u32>(lists[2]->size()));
   if (g.wide)
      w.w32(0);
   for (const NodeList *l : lists)
      for (const auto &def : *l) {
         const s64 type = def->i(pdef::Type);
         const u64 name = strings.offset(def->s(pdef::Name));
         u64 raw;
         if (type == PT_String)
            raw = strings.offset(def->s(pdef::Default));
         else if (type == PT_Float)
            raw =
               g.wide ? static_cast<u64>(def->i(pdef::Default)) : f32_bits(static_cast<f32>(def->f64v(pdef::Default)));
         else
            raw = g.wide ? static_cast<u64>(def->i(pdef::Default)) : static_cast<u32>(def->i(pdef::Default));
         if (g.wide) {
            w.w64(name);
            w.w32(static_cast<u32>(type));
            w.w32(0);
            w.w64(raw);
         } else {
            w.w32(static_cast<u32>(name));
            w.w32(static_cast<u32>(type));
            w.w32(static_cast<u32>(raw));
         }
      }
   w.bytes(strings.bytes());
   w.align(g.align());
   w.patch_u32(pdt, static_cast<u32>(w.pos() - pdt));
}

void
ModernWriter::write_user(const Node &u, std::size_t at)
{
   const NodeList &calls = u.list(user::Calls);
   const NodeList &ctns = u.list(user::Containers);
   const int n_call = calls.size();
   if (!g.user_locals && !u.list(user::LocalProperties).empty())
      throw Error("user " + u.label() + " has local properties, which this version cannot store");

   std::vector<u64> ctn_pos;
   u64 ctn_bytes = 0;
   for (const auto &c : ctns) {
      ctn_pos.push_back(ctn_bytes);
      ctn_bytes += container_size(g, c->i(ctn::Type), g.wide && c->i(ctn::BlendByProperty) != 0,
                                  c->list(ctn::Axis1).size(), c->list(ctn::Axis2).size());
   }
   const u64 user_params = 4 * u64(file.list(file::UserParamDefs).size());
   const u64 calls_at = g.user_header() + u.list(user::LocalProperties).size() * g.ptr() + user_params +
                        2 * u64(n_call) + (n_call & 1 ? 2 : 0);
   const u64 trigger_table = calls_at + n_call * g.call_size() + ctn_bytes;

   int random2 = 0;
   for (const auto &c : calls)
      if (call_is_container(c->i(call::Flag)) && c->i(call::Container) >= 0 && c->i(call::Container) < ctns.size() &&
          ctns.at(static_cast<int>(c->i(call::Container))).i(ctn::Type) == CT_Random2)
         ++random2;

   w.w32(0);
   if (g.global_count) {
      if (u.list(user::LocalProperties).size() > 0xFFFF)
         throw Error("user " + u.label() + " has more local properties than this version can count");
      w.w16(static_cast<u16>(u.list(user::LocalProperties).size()));
      w.w16(static_cast<u16>(u.i(user::GlobalPropertyCount)));
   } else if (g.user_locals) {
      w.w32(static_cast<u32>(u.list(user::LocalProperties).size()));
   }
   w.w32(static_cast<u32>(n_call));
   int assets = 0;
   for (const auto &c : calls)
      assets += !call_is_container(c->i(call::Flag));
   w.w32(static_cast<u32>(assets));
   w.w32(static_cast<u32>(random2));
   for (const int l : {user::ActionSlots, user::Actions, user::ActionTriggers, user::Properties, user::PropertyTriggers,
                       user::AlwaysTriggers})
      w.w32(static_cast<u32>(u.list(l).size()));
   if (g.wide) {
      w.w32(0);
      w.w64(trigger_table);
   } else {
      w.w32(static_cast<u32>(trigger_table));
   }
   for (const auto &lp : u.list(user::LocalProperties))
      wstr(lp->s(name::Name));
   // Exactly one word per user parameter definition, in definition order;
   // definitions the user leaves out get their default.
   const NodeList &defs = file.list(file::UserParamDefs);
   for (int k = 0; k < defs.size(); ++k) {
      const Node *found = nullptr;
      for (const auto &p : u.list(user::UserParams))
         if (p->i(param::Def) == k) {
            found = p.get();
            break;
         }
      if (found) {
         w.w32(word(*found));
         continue;
      }
      Node fallback(&schemas().param);
      const Node &def = defs.at(k);
      const s64 type = def.i(pdef::Type);
      fallback.set_i(param::Def, k);
      fallback.set_i(param::DirectHint, -1);
      if (type == PT_String) {
         fallback.set_i(param::RefType, RT_String);
         fallback.set_s(param::Value, def.s(pdef::Default));
      } else {
         fallback.set_i(param::RefType, RT_Direct);
         fallback.set_i(param::Value, type == PT_Float ? f32_bits(static_cast<f32>(def.f64v(pdef::Default)))
                                                       : static_cast<u32>(def.i(pdef::Default)));
      }
      w.w32(word(fallback));
   }

   for (const int k : sorted_calls(u))
      w.w16(static_cast<u16>(k));
   if (n_call & 1)
      w.w16(0);

   for (const auto &c : calls) {
      wstr(c->s(call::KeyName));
      w.ws16(static_cast<s16>(c->i(call::AssetId)));
      w.w16(static_cast<u16>(c->i(call::Flag)));
      w.ws32(static_cast<s32>(c->i(call::Duration)));
      w.ws32(static_cast<s32>(c->i(call::ParentIndex)));
      w.w32(static_cast<u32>(c->i(call::Guid)));
      w.w32(static_cast<u32>(c->i(call::KeyNameHash)));
      if (g.wide)
         w.w32(0);
      s64 param;
      if (call_is_container(c->i(call::Flag))) {
         const s64 ci = c->i(call::Container);
         param = ci >= 0 && ci < static_cast<s64>(ctn_pos.size()) ? static_cast<s64>(ctn_pos[ci]) : -1;
      } else {
         param = table_pos(m_asset_set_pos, m_asset_sets_end, c->i(call::ParamSet));
         if (param < 0)
            param = 0;
      }
      wpos(param);
      wpos(table_pos(m_condition_pos, m_conditions_end, c->i(call::Condition)));
   }

   for (const auto &c : ctns) {
      const s64 type = c->i(ctn::Type);
      const bool blend_by = g.wide && c->i(ctn::BlendByProperty) != 0;
      if (g.wide) {
         w.w8(static_cast<u8>(type));
         w.w8(blend_by);
         w.w16(0);
      } else {
         w.w32(static_cast<u32>(type));
      }
      w.ws32(static_cast<s32>(c->i(ctn::ChildStart)));
      w.ws32(static_cast<s32>(c->i(ctn::ChildEnd)));
      if (g.wide)
         w.w32(0);
      if (has_switch_params(type, blend_by)) {
         wstr(c->s(ctn::WatchPropertyName));
         w.ws32(static_cast<s32>(c->i(ctn::WatchPropertyId)));
         w.ws16(static_cast<s16>(c->i(ctn::LocalPropertyNameIdx)));
         w.w8(static_cast<u8>(c->i(ctn::IsGlobal)));
         w.w8(static_cast<u8>(c->i(ctn::SwitchByte)));
      } else if (type == CT_Grid && g.wide) {
         const NodeList &a1 = c->list(ctn::Axis1);
         const NodeList &a2 = c->list(ctn::Axis2);
         const NodeList &cells = c->list(ctn::Cells);
         if (cells.size() != a1.size() * a2.size())
            throw Error("grid container in " + u.label() + " needs " + std::to_string(a1.size() * a2.size()) +
                        " cells but has " + std::to_string(cells.size()));
         wstr(c->s(ctn::WatchPropertyName));
         wstr(c->s(ctn::WatchPropertyName2));
         w.ws16(static_cast<s16>(c->i(ctn::LocalPropertyNameIdx)));
         w.ws16(static_cast<s16>(c->i(ctn::LocalPropertyNameIdx2)));
         w.w8(static_cast<u8>(c->i(ctn::GridFlags1)));
         w.w8(static_cast<u8>(c->i(ctn::GridFlags2)));
         w.w8(static_cast<u8>(a1.size()));
         w.w8(static_cast<u8>(a2.size()));
         for (const auto &v : a1)
            w.w32(static_cast<u32>(v->i(grid_value::Value)));
         for (const auto &v : a2)
            w.w32(static_cast<u32>(v->i(grid_value::Value)));
         for (const auto &v : cells)
            w.ws32(static_cast<s32>(v->i(grid_cell::Call)));
      }
   }
   if (w.pos() - at != trigger_table)
      throw Error("internal error: trigger table of " + u.label() + " misplaced");

   const auto call_pos = [&](s64 i) {
      if (i < 0 || i >= n_call)
         throw Error("a trigger in " + u.label() + " names call " + std::to_string(i) + " of " +
                     std::to_string(n_call));
      return static_cast<u64>(i) * g.call_size();
   };
   const auto set_pos = [&](s64 i) { return table_pos(m_trigger_set_pos, m_trigger_sets_end, i); };
   const auto cond_pos = [&](s64 i) { return table_pos(m_condition_pos, m_conditions_end, i); };
   for (const auto &s : u.list(user::ActionSlots)) {
      wstr(s->s(slot::Name));
      w.ws16(static_cast<s16>(s->i(slot::ActionStart)));
      w.ws16(static_cast<s16>(s->i(slot::ActionEnd)));
      if (g.wide)
         w.w32(0);
   }
   for (const auto &a : u.list(user::Actions)) {
      wstr(a->s(action::Name));
      if (g.wide) {
         w.ws16(static_cast<s16>(a->i(action::TriggerStart)));
         w.w16(static_cast<u16>(a->i(action::Flag)));
      } else {
         w.ws32(static_cast<s32>(a->i(action::TriggerStart)));
      }
      w.ws32(static_cast<s32>(a->i(action::TriggerEnd)));
   }
   for (const auto &t : u.list(user::ActionTriggers)) {
      w.w32(static_cast<u32>(t->i(atrig::Guid)));
      if (g.wide)
         w.w32(0);
      wptr(call_pos(t->i(atrig::AssetCall)));
      if (trigger_names_previous_action(t->i(atrig::Flag)))
         wstr(t->s(atrig::PreviousActionName));
      else
         wptr(static_cast<u32>(t->i(atrig::StartFrame)));
      w.ws32(static_cast<s32>(t->i(atrig::EndFrame)));
      w.w16(static_cast<u16>(t->i(atrig::Flag)));
      w.w16(static_cast<u16>(t->i(atrig::OverwriteHash)));
      wpos(set_pos(t->i(atrig::Overwrite)));
   }
   for (const auto &pr : u.list(user::Properties)) {
      wstr(pr->s(prop::Name));
      w.ws32(static_cast<s32>(pr->i(prop::IsGlobal)));
      w.ws32(static_cast<s32>(pr->i(prop::TriggerStart)));
      w.ws32(static_cast<s32>(pr->i(prop::TriggerEnd)));
      if (g.wide)
         w.w32(0);
   }
   for (const auto &t : u.list(user::PropertyTriggers)) {
      w.w32(static_cast<u32>(t->i(ptrig::Guid)));
      if (g.wide) {
         w.w16(static_cast<u16>(t->i(ptrig::Flag)));
         w.w16(static_cast<u16>(t->i(ptrig::OverwriteHash)));
         w.w64(call_pos(t->i(ptrig::AssetCall)));
         wpos(cond_pos(t->i(ptrig::Condition)));
         wpos(set_pos(t->i(ptrig::Overwrite)));
      } else {
         w.w32(static_cast<u32>(call_pos(t->i(ptrig::AssetCall))));
         wpos(cond_pos(t->i(ptrig::Condition)));
         w.w16(static_cast<u16>(t->i(ptrig::Flag)));
         w.w16(static_cast<u16>(t->i(ptrig::OverwriteHash)));
         wpos(set_pos(t->i(ptrig::Overwrite)));
      }
   }
   for (const auto &t : u.list(user::AlwaysTriggers)) {
      w.w32(static_cast<u32>(t->i(awtrig::Guid)));
      if (g.wide) {
         w.w16(static_cast<u16>(t->i(awtrig::Flag)));
         w.w16(static_cast<u16>(t->i(awtrig::OverwriteHash)));
         w.w64(call_pos(t->i(awtrig::AssetCall)));
      } else {
         w.w32(static_cast<u32>(call_pos(t->i(awtrig::AssetCall))));
         w.w16(static_cast<u16>(t->i(awtrig::Flag)));
         w.w16(static_cast<u16>(t->i(awtrig::OverwriteHash)));
      }
      wpos(set_pos(t->i(awtrig::Overwrite)));
   }
}

// Calls sorted by key name for the game's binary search. Equal names may come
// in any order, so the order the file was read with is kept while it still
// sorts the current calls.
std::vector<int>
ModernWriter::sorted_calls(const Node &u) const
{
   const NodeList &calls = u.list(user::Calls);
   const int n = calls.size();
   const auto key = [&](int k) -> const std::string & { return calls.at(k).s(call::KeyName); };
   const std::string &hint = u.s(user::SortedCallOrder);
   if (hint.size() == 2 * static_cast<std::size_t>(n)) {
      Reader hr(Bytes(reinterpret_cast<const u8 *>(hint.data()), hint.size()), d.endian);
      std::vector<int> order(n);
      std::vector<bool> seen(n);
      bool ok = true;
      for (int k = 0; k < n && ok; ++k) {
         order[k] = hr.u16_at(2 * k);
         ok = order[k] < n && !seen[order[k]] && (k == 0 || !(key(order[k]) < key(order[k - 1])));
         if (ok)
            seen[order[k]] = true;
      }
      if (ok)
         return order;
   }
   std::vector<int> order(n);
   for (int k = 0; k < n; ++k)
      order[k] = k;
   std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return key(a) < key(b); });
   return order;
}

Buffer
ModernWriter::run()
{
   const NodeList &users = file.list(file::Users);
   const u32 n_user = static_cast<u32>(users.size());

   // The game binary-searches the hash table, so users go out in hash order.
   std::vector<std::pair<u32, const Node *>> order;
   for (const auto &u : users) {
      const u32 h = u->s(user::Name).empty() ? static_cast<u32>(u->i(user::Hash)) : crc32(u->s(user::Name));
      order.emplace_back(h, u.get());
   }
   std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

   // Positions every earlier section needs from later ones.
   {
      u64 at = 0;
      for (const auto &set : file.list(file::AssetParamSets)) {
         m_asset_set_pos.push_back(at);
         at += 8 + 4 * u64(set->list(pset::Params).size());
      }
      m_asset_sets_end = at;
      at = 0;
      for (const auto &set : file.list(file::TriggerParamSets)) {
         m_trigger_set_pos.push_back(at);
         at += 4 + 4 * u64(set->list(pset::Params).size());
      }
      m_trigger_sets_end = at;
      at = 0;
      for (const auto &c : file.list(file::Conditions)) {
         m_condition_pos.push_back(at);
         at += condition_size(g, *c);
      }
      m_conditions_end = at;
      at = 0;
      for (const auto &grp : file.list(file::ArrangeGroups)) {
         m_arrange_pos.push_back(at);
         at += 4 + g.arrange_entry() * grp->list(arrange::Entries).size();
      }
   }

   w.zeros(g.header());
   for (const auto &[h, u] : order)
      w.w32(h);
   if (g.wide)
      w.align(8);
   const std::size_t offsets = w.pos();
   w.zeros(n_user * g.ptr());
   write_pdt();

   w.align(g.align());
   // Parameters are written in definition order; the mask records which are set.
   const auto write_set = [&](const Node &set, bool wide_mask) {
      std::vector<const Node *> params;
      for (const auto &p : set.list(pset::Params))
         params.push_back(p.get());
      std::stable_sort(params.begin(), params.end(),
                       [](const Node *a, const Node *b) { return a->i(param::Def) < b->i(param::Def); });
      u64 mask = 0;
      for (const Node *p : params) {
         const s64 def = p->i(param::Def);
         if (def < 0 || def >= (wide_mask ? 64 : 32))
            throw Error("parameter definition index " + std::to_string(def) + " is out of range");
         if (mask >> def & 1)
            throw Error("a parameter set gives definition " + std::to_string(def) + " twice");
         mask |= u64{1} << def;
      }
      wide_mask ? w.w64(mask) : w.w32(static_cast<u32>(mask));
      for (const Node *p : params)
         w.w32(word(*p));
   };
   for (const auto &set : file.list(file::AssetParamSets))
      write_set(*set, true);
   const std::size_t trigger_sets = w.pos();
   for (const auto &set : file.list(file::TriggerParamSets))
      write_set(*set, false);

   const std::size_t local = w.pos();
   for (const auto &n : file.list(file::LocalPropertyNames))
      wstr(n->s(name::Name));
   for (const auto &n : file.list(file::LocalPropertyEnumNames))
      wstr(n->s(name::Name));

   // Users can add direct values, so they are serialized before the direct
   // table is emitted, then appended after the common tables.
   Writer users_out(d.endian);
   std::vector<u64> user_rel;
   {
      Writer saved = std::move(w);
      w = std::move(users_out);
      for (const auto &[h, u] : order) {
         user_rel.push_back(w.pos());
         write_user(*u, w.pos());
      }
      users_out = std::move(w);
      w = std::move(saved);
   }

   for (const u32 v : m_direct.values())
      w.w32(v);
   for (const auto &rnd : file.list(file::Randoms)) {
      w.w32(static_cast<u32>(rnd->i(random::Min)));
      w.w32(static_cast<u32>(rnd->i(random::Max)));
   }
   u64 point_index = 0;
   for (const auto &c : file.list(file::Curves)) {
      const u64 count = c->list(curve::Points).size();
      if (point_index + count > 0xFFFF)
         throw Error("curve points exceed the 65535 a curve table can index");
      w.w16(static_cast<u16>(point_index));
      w.w16(static_cast<u16>(count));
      w.w16(static_cast<u16>(c->i(curve::CurveType)));
      w.w16(static_cast<u16>(c->i(curve::IsPropGlobal)));
      wstr(c->s(curve::PropName));
      w.ws32(static_cast<s32>(c->i(curve::PropIdx)));
      w.ws16(static_cast<s16>(c->i(curve::LocalPropertyNameIdx)));
      w.ws16(static_cast<s16>(c->i(curve::UpdateType)));
      point_index += count;
   }
   for (const auto &c : file.list(file::Curves))
      for (const auto &pt : c->list(curve::Points)) {
         w.w32(static_cast<u32>(pt->i(point::X)));
         w.w32(static_cast<u32>(pt->i(point::Y)));
      }

   const std::size_t ex_region = w.pos();
   for (const auto &grp : file.list(file::ArrangeGroups)) {
      w.w32(static_cast<u32>(grp->list(arrange::Entries).size()));
      for (const auto &e : grp->list(arrange::Entries)) {
         wstr(e->s(arrange_entry::GroupName));
         w.w8(static_cast<u8>(e->i(arrange_entry::LimitType)));
         w.w8(static_cast<u8>(e->i(arrange_entry::LimitThreshold)));
         w.w8(static_cast<u8>(e->i(arrange_entry::Unk)));
         w.w8(static_cast<u8>(e->i(arrange_entry::Pad)));
         if (g.wide)
            w.w32(0);
      }
   }

   const std::size_t users_at = w.pos();
   w.bytes(Bytes(users_out.buffer()));
   for (u32 k = 0; k < n_user; ++k) {
      const u64 pos = users_at + user_rel[k];
      g.wide ? w.patch_u64(offsets + 8 * k, pos) : w.patch_u32(offsets + 4 * k, static_cast<u32>(pos));
   }

   const std::size_t conditions = w.pos();
   for (const auto &c : file.list(file::Conditions)) {
      const s64 type = c->i(cond::ParentType);
      w.w32(static_cast<u32>(type));
      switch (type) {
      case CT_Switch: {
         const s64 pt = c->i(cond::PropertyType);
         if (g.switch_layout == 1) {
            w.w8(static_cast<u8>(pt));
            w.w8(static_cast<u8>(c->i(cond::CompareType)));
            w.w8(static_cast<u8>(c->i(cond::IsSolved)));
            w.w8(static_cast<u8>(c->i(cond::IsGlobal)));
            w.w32(pt == PR_Enum ? m_names.offset(c->s(cond::Value)) : static_cast<u32>(c->i(cond::Value)));
            w.ws32(static_cast<s32>(c->i(cond::LocalPropertyEnumNameIdx)));
         } else if (g.wide) {
            w.w8(static_cast<u8>(pt));
            w.w8(static_cast<u8>(c->i(cond::CompareType)));
            w.w8(static_cast<u8>(c->i(cond::IsSolved)));
            w.w8(static_cast<u8>(c->i(cond::IsGlobal)));
            w.ws32(static_cast<s32>(c->i(cond::LocalPropertyEnumNameIdx)));
            if (pt == PR_Enum) {
               w.w32(0);
               w.w64(m_names.offset(c->s(cond::Value)));
            } else {
               w.w32(static_cast<u32>(c->i(cond::Value)));
            }
         } else {
            w.w32(static_cast<u32>(pt));
            w.w32(static_cast<u32>(c->i(cond::CompareType)));
            w.w32(pt == PR_Enum ? m_names.offset(c->s(cond::Value)) : static_cast<u32>(c->i(cond::Value)));
            w.ws16(static_cast<s16>(c->i(cond::LocalPropertyEnumNameIdx)));
            w.w8(static_cast<u8>(c->i(cond::IsSolved)));
            w.w8(static_cast<u8>(c->i(cond::IsGlobal)));
         }
         break;
      }
      case CT_Random:
      case CT_Random2:
         w.w32(static_cast<u32>(c->i(cond::Weight)));
         break;
      case CT_Blend:
         w.w32(static_cast<u32>(c->i(cond::BlendMin)));
         w.w32(static_cast<u32>(c->i(cond::BlendMax)));
         w.w8(static_cast<u8>(c->i(cond::BlendTypeToMax)));
         w.w8(static_cast<u8>(c->i(cond::BlendTypeToMin)));
         w.w16(0);
         break;
      default:
         w.ws32(static_cast<s32>(c->i(cond::ContinueOnFade)));
         break;
      }
   }

   const std::size_t names = w.pos();
   w.bytes(m_names.bytes());
   w.align(4);

   Buffer &b = w.buffer();
   Writer h(d.endian);
   h.bytes(std::string_view("XLNK"));
   h.w32(static_cast<u32>(b.size()));
   h.w32(static_cast<u32>(file.i(file::Version)));
   u64 n_params = 0;
   for (const auto &set : file.list(file::AssetParamSets))
      n_params += set->list(pset::Params).size();
   h.w32(static_cast<u32>(n_params));
   h.w32(static_cast<u32>(file.list(file::AssetParamSets).size()));
   h.w32(static_cast<u32>(file.list(file::TriggerParamSets).size()));
   const auto hp = [&](u64 v) { g.wide ? h.w64(v) : h.w32(static_cast<u32>(v)); };
   hp(trigger_sets);
   hp(local);
   h.w32(static_cast<u32>(file.list(file::LocalPropertyNames).size()));
   h.w32(static_cast<u32>(file.list(file::LocalPropertyEnumNames).size()));
   h.w32(static_cast<u32>(m_direct.values().size()));
   h.w32(static_cast<u32>(file.list(file::Randoms).size()));
   h.w32(static_cast<u32>(file.list(file::Curves).size()));
   h.w32(static_cast<u32>(point_index));
   hp(ex_region);
   h.w32(n_user);
   if (g.wide)
      h.w32(0);
   hp(conditions);
   hp(names);
   std::copy(h.buffer().begin(), h.buffer().end(), b.begin());
   return std::move(b);
}

} // namespace

// Effect and sound databases of one library build differ in version:
// ELink2 30 pairs with SLink2 28, ELink2 31 with SLink2 29, ELink2 34 with
// SLink2 31, the first 64-bit build.
bool
is_wide(u32 version, int system)
{
   return system == static_cast<int>(System::SLink) ? version >= 31 : version >= 32;
}

// The build of ELink2 29 and SLink2 27 has no local property count in the
// user header.
bool
has_user_locals(u32 version, int system)
{
   return version >= (system == static_cast<int>(System::SLink) ? 28u : 30u);
}

u16
global_property_count(const Node &file, const Node &u)
{
   std::set<std::string> names;
   for (const auto &c : u.list(user::Containers)) {
      if (has_switch_params(c->i(ctn::Type), c->i(ctn::BlendByProperty) != 0) && c->i(ctn::IsGlobal) &&
          !c->s(ctn::WatchPropertyName).empty())
         names.insert(c->s(ctn::WatchPropertyName));
      // A grid's flag bits 0 and 1 mark its first and second axis global.
      if (c->i(ctn::Type) == CT_Grid) {
         if (c->i(ctn::GridFlags1) & 1 && !c->s(ctn::WatchPropertyName).empty())
            names.insert(c->s(ctn::WatchPropertyName));
         if (c->i(ctn::GridFlags1) & 2 && !c->s(ctn::WatchPropertyName2).empty())
            names.insert(c->s(ctn::WatchPropertyName2));
      }
   }
   const NodeList &sets = file.list(file::AssetParamSets);
   const NodeList &curves = file.list(file::Curves);
   for (const auto &c : u.list(user::Calls)) {
      const s64 set = call_is_container(c->i(call::Flag)) ? -1 : c->i(call::ParamSet);
      if (set < 0 || set >= sets.size())
         continue;
      for (const auto &p : sets.at(static_cast<int>(set)).list(pset::Params)) {
         const s64 k = p->i(param::Value);
         if (p->i(param::RefType) == RT_Curve && k >= 0 && k < curves.size() &&
             curves.at(static_cast<int>(k)).i(curve::IsPropGlobal))
            names.insert(curves.at(static_cast<int>(k)).s(curve::PropName));
      }
   }
   for (const auto &p : u.list(user::Properties))
      if (p->i(prop::IsGlobal))
         names.insert(p->s(prop::Name));
   return static_cast<u16>(std::min<std::size_t>(names.size(), 0xFFFF));
}

bool
has_global_count(u32 version, int system)
{
   return version >= (system == static_cast<int>(System::SLink) ? 33u : 36u);
}

int
switch_layout(u32 version, int system)
{
   if (is_wide(version, system))
      return 2;
   return version >= (system == static_cast<int>(System::SLink) ? 29u : 31u) ? 1 : 0;
}

Document
read(Bytes data)
{
   return ModernReader(data).run();
}

Buffer
write(const Document &doc)
{
   return ModernWriter(doc, static_cast<u32>(doc.root->i(file::Version))).run();
}

} // namespace xl::xlink_modern
