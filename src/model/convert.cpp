#include "model/convert.hpp"

#include <algorithm>
#include <functional>
#include <map>

#include "core/crc32.hpp"
#include "formats/codecs.hpp"
#include "model/dslink_schema.hpp"
#include "model/value_text.hpp"
#include "model/xlink_schema.hpp"

namespace xl {

namespace {

std::string count_text(std::size_t n, const std::string& what) {
  return std::to_string(n) + " " + what;
}

// Re-encodes every string; characters the target encoding lacks become '?'.
std::size_t transcode(Node& root, Encoding from, Encoding to) {
  std::size_t lossy = 0;
  walk(root, [&](Node& n) {
    const Schema& sc = n.schema();
    for (int f = 0; f < static_cast<int>(sc.fields.size()); ++f) {
      if (sc.fields[f].hidden || field_kind(n, f) != DynKind::String)
        continue;
      const std::string utf8 = to_utf8(n.s(f), from);
      std::string out = from_utf8(utf8, to);
      if (to_utf8(out, to) != utf8)
        ++lossy;
      n.set_s(f, std::move(out));
    }
  });
  return lossy;
}

// String tables kept to write names back where they were found, and v19
// conditions that hold name offsets into them; a new encoding or layout
// rebuilds them.
void drop_string_layout(Document& d) {
  Node& file = *d.root;
  if (d.format == Format::XLink) {
    using namespace xlink;
    for (const int f : {file::NameTablePool, file::PdtStringPool, file::ConditionPool})
      file.set_s(f, {});
    file.set_i(file::ConditionCountBias, 0);
    for (auto& c : file.list(file::Conditions))
      c->set_i(cond::PoolOffset, -1);
    return;
  }
  file.set_s(dslink::file::StringPool, {});
  for (auto& u : file.list(dslink::file::Users))
    u->set_s(dslink::user::StringPool, {});
}

void convert_xlink(Document& d, const Document& src, const Target& t, std::vector<std::string>& losses) {
  using namespace xlink;
  Node& file = *d.root;
  const u32 from = src.version();
  const bool from_v19 = from == 19, to_v19 = t.version == 19;
  const bool to_wide = !to_v19 && xlink_modern::is_wide(t.version, src.system());

  // Tables kept from the original layout only fit that layout.
  drop_string_layout(d);
  file.set_s(file::DirectValuePool, {});

  std::size_t grids = 0, jumps = 0, blend_by = 0, local_props = file.list(file::LocalPropertyNames).size() +
                                       file.list(file::LocalPropertyEnumNames).size();
  std::size_t unnamed = 0, based = 0, wide_ids = 0;
  for (auto& u : file.list(file::Users)) {
    for (auto& c : u->list(user::Containers)) {
      grids += c->i(ctn::Type) == CT_Grid;
      jumps += c->i(ctn::Type) == CT_Jump;
      blend_by += c->i(ctn::BlendByProperty) != 0;
    }
    local_props += u->list(user::LocalProperties).size();
    if (u->s(user::Name).empty())
      ++unnamed;
    if (!u->s(user::BaseName).empty())
      ++based;
    for (auto& c : u->list(user::Calls))
      wide_ids += c->i(call::AssetId) < -0x8000 || c->i(call::AssetId) > 0x7FFF;
  }

  if (to_v19) {
    if (unnamed)
      losses.push_back(count_text(unnamed, "users have only a hash; version 19 finds users by name, so they are "
                                           "named after their hash"));
    for (auto& u : file.list(file::Users))
      if (u->s(user::Name).empty())
        u->set_s(user::Name, from_utf8(hex(static_cast<u32>(u->i(user::Hash)), 8), d.encoding));
    if (local_props)
      losses.push_back(count_text(local_props, "local property names dropped (version 19 has none)"));
    file.list(file::LocalPropertyNames).clear();
    file.list(file::LocalPropertyEnumNames).clear();
    for (auto& u : file.list(file::Users))
      u->list(user::LocalProperties).clear();
    std::size_t defaults = 0;
    for (const int l : {file::UserParamDefs, file::AssetParamDefs, file::TriggerParamDefs})
      for (auto& def : file.list(l))
        defaults += def->i(pdef::Default) != 0 || !def->s(pdef::Default).empty();
    if (defaults)
      losses.push_back(count_text(defaults, "parameter defaults are not stored by version 19; parameters an asset "
                                            "leaves unset are written out with them instead"));
    std::size_t switch_extra = 0, blend_extra = 0, new_refs = 0;
    for (auto& c : file.list(file::Conditions)) {
      if (c->i(cond::ParentType) == CT_Switch)
        switch_extra += c->i(cond::IsSolved) || c->i(cond::IsGlobal) || c->i(cond::LocalPropertyEnumNameIdx) > 0;
      if (c->i(cond::ParentType) == CT_Blend)
        blend_extra += c->i(cond::BlendMax) || c->i(cond::BlendTypeToMax) || c->i(cond::BlendTypeToMin);
    }
    walk(file, [&](Node& n) {
      if (&n.schema() == &schemas().param && n.i(param::RefType) > RT_ArrangeGroup)
        ++new_refs;
    });
    if (switch_extra)
      losses.push_back(count_text(switch_extra, "switch conditions lose their global and solved flags"));
    if (blend_extra)
      losses.push_back(count_text(blend_extra, "blend conditions keep only their minimum"));
    if (new_refs)
      losses.push_back(count_text(new_refs, "parameters use reference types newer than version 19"));
  }
  if (!to_v19 && !xlink_modern::has_user_locals(t.version, src.system())) {
    std::size_t user_locals = 0;
    for (auto& u : file.list(file::Users)) {
      user_locals += u->list(user::LocalProperties).size();
      u->list(user::LocalProperties).clear();
    }
    if (user_locals)
      losses.push_back(count_text(user_locals, "user local property names dropped (this version lists them only "
                                               "for the whole database)"));
  }
  if (from_v19 && !to_v19) {
    if (based)
      losses.push_back(count_text(based, "users name a base user, which later versions do not store"));
    for (auto& u : file.list(file::Users)) {
      u->set_s(user::BaseName, {});
      for (auto& c : u->list(user::Calls))
        c->set_i(call::KeyNameHash, crc32(c->s(call::KeyName)));
    }
    if (wide_ids)
      losses.push_back(count_text(wide_ids, "asset ids do not fit 16 bits and wrap"));
  }
  if (grids && (to_v19 || !to_wide))
    losses.push_back(count_text(grids, "grid containers cannot be stored by this version and lose their table"));
  if (jumps && (to_v19 || !xlink_modern::has_global_count(t.version, src.system())))
    losses.push_back(count_text(jumps, "jump containers are kept but this version's games cannot play them"));
  if (blend_by && (to_v19 || !to_wide)) {
    for (auto& u : file.list(file::Users))
      for (auto& c : u->list(user::Containers))
        c->set_i(ctn::BlendByProperty, 0);
    losses.push_back(count_text(blend_by, "blend-by-property containers become plain blend containers and lose "
                                          "their property"));
  }
  if (!to_wide && !to_v19 && xlink_modern::is_wide(from, src.system())) {
    std::size_t rounded = 0;
    for (const int l : {file::UserParamDefs, file::AssetParamDefs, file::TriggerParamDefs})
      for (auto& def : file.list(l))
        if (def->i(pdef::Type) == PT_Float &&
            static_cast<f64>(static_cast<f32>(def->f64v(pdef::Default))) != def->f64v(pdef::Default))
          ++rounded;
    if (rounded)
      losses.push_back(count_text(rounded, "float parameter defaults lose precision going to 32 bits"));
  }
}

const Node* find_user(const NodeList& users, const std::string& name) {
  for (const auto& u : users)
    if (u->s(dslink::user::Name) == name && u->i(dslink::user::Type) == dslink::UT_User)
      return u.get();
  return nullptr;
}

// Rebuilds `src` under the schema `dst` field by field and list by list,
// matching names. Values with nowhere to go are counted in `dropped`.
std::unique_ptr<Node> transfer(const Node& src, const Schema* dst, std::map<std::string, std::size_t>& dropped,
                               const std::function<void(const Node&, Node&)>& fixup) {
  auto out = std::make_unique<Node>(dst);
  const Schema& ss = src.schema();
  const Node blank(&ss);
  for (int f = 0; f < static_cast<int>(ss.fields.size()); ++f) {
    const int g = dst->field_index(ss.fields[f].name);
    if (g >= 0 && dst->fields[g].type == ss.fields[f].type)
      out->value(g) = src.value(f);
    else if (!ss.fields[f].hidden && !(src.value(f) == blank.value(f)))
      ++dropped[ss.name + "." + ss.fields[f].name];
  }
  for (int l = 0; l < src.list_count(); ++l) {
    const int m = dst->list_index(ss.lists[l].name);
    if (m < 0) {
      if (!src.list(l).empty())
        dropped[ss.name + "." + ss.lists[l].name] += src.list(l).size();
      continue;
    }
    NodeList& to = out->list(m);
    for (const auto& c : src.list(l))
      to.append(transfer(*c, to.def().element, dropped, fixup));
  }
  fixup(src, *out);
  return out;
}

// EffectLink 9 / SoundLink 69 (Wii U) and EffectLink 10 / SoundLink 73
// (3DS). The Wii U versions have no shared pools, aliases or base users, so
// those are expanded into each user going there.
void convert_old_link(Document& d, const Document& src, const Target& t, std::vector<std::string>& losses) {
  using namespace dslink;
  const bool effect = d.format == Format::EffectLink;
  const bool to_wiiu = t.version == 9 || t.version == 69;
  const s64 kind = effect ? t.version : 1;
  const std::string noun = effect ? "effects" : "assets";
  Node& file = *d.root;
  file.set_s(file::StringPool, {});
  NodeList& users = file.list(file::Users);
  if (to_wiiu) {
    // Shared records are copied into each user that lists them.
    std::size_t pooled = 0;
    for (auto& u : users) {
      if (u->i(user::Type) != UT_User)
        continue;
      pooled += u->i(user::PooledAssets) || u->i(user::PooledTriggers);
      const auto materialize = [&](int flag, int refs, int own, int pool) {
        if (!u->i(flag))
          return;
        NodeList& dst = u->list(own);
        dst.clear();
        for (const auto& r : u->list(refs)) {
          const s64 i = r->i(pool_ref::Index);
          Node& copy = i >= 0 && i < file.list(pool).size() ? dst.append(file.list(pool).at(static_cast<int>(i)).clone())
                                                             : dst.append_new();
          // The user's parameter words for the effect move with it.
          if (copy.list_count() > 0 && r->list_count() > 0) {
            copy.list(slot_params::Params).clear();
            for (const auto& p : r->list(slot_params::Params))
              copy.list(slot_params::Params).append(p->clone());
          }
        }
        u->set_i(flag, 0);
      };
      materialize(user::PooledAssets, user::AssetRefs, user::Assets, file::SharedAssets);
      materialize(user::PooledTriggers, user::TriggerRefs, user::Triggers, file::SharedTriggers);
      u->list(user::AssetRefs).clear();
      u->list(user::TriggerRefs).clear();
    }
    if (pooled)
      losses.push_back(count_text(pooled, "users drew " + noun + " or triggers from shared pools; each now carries copies"));
    file.list(file::SharedAssets).clear();
    file.list(file::SharedTriggers).clear();

    // Aliases become full copies of the user they stand in for.
    std::size_t aliases = 0, empties = 0, based = 0;
    for (int k = 0; k < users.size(); ++k) {
      Node& u = users.at(k);
      if (u.i(user::Type) == UT_Empty) {
        ++empties;
        users.take(k--);
        continue;
      }
      if (u.i(user::Type) == UT_Alias) {
        const Node* base = find_user(users, u.s(user::BaseName));
        if (!base) {
          ++empties;
          users.take(k--);
          continue;
        }
        auto copy = base->clone();
        copy->set_s(user::Name, u.s(user::Name));
        users.take(k);
        users.insert(k, std::move(copy));
        ++aliases;
        continue;
      }
      if (!u.s(user::BaseName).empty())
        ++based;
    }
    if (aliases)
      losses.push_back(count_text(aliases, "aliases become copies of the user they stood in for"));
    if (empties)
      losses.push_back(count_text(empties, "empty or dangling users dropped"));
    if (based)
      losses.push_back(count_text(based, "users name a base user, which this version does not store"));
  }
  for (auto& u : users) {
    u->set_s(user::StringPool, {});
    u->set_s(user::Tail, {});
    // EffectLink 9 users always carry their own strings.
    u->set_i(user::OwnStrings, effect && to_wiiu);
    u->set_i(user::Kind, u->i(user::Type) == UT_User ? kind : u->i(user::Kind));
    if (to_wiiu) {
      u->set_s(user::BaseName, {});
      u->set_i(user::Type, UT_User);
    }
  }
  if (effect || src.version() == t.version)
    return;

  // SoundLink 69 and 73 lay records out differently; rebuild them under the
  // target's schemas. Triggers name their call by index in 69 and by name
  // in 73.
  std::size_t header = 0;
  for (const auto& u : users)
    for (const int f : {user::Header1, user::Header2, user::Header3, user::Header4})
      header += t.version == 73 && u->i(user::Type) == UT_User && u->i(f) != 0;
  if (header)
    losses.push_back(count_text(header, "user distance settings of version 69 have no place in version 73"));
  if (t.version == 69)
    losses.push_back("version 73 has no user distance settings; users are written with 0, which Mario Kart 8 "
                     "replaces with its defaults (full volume within 80, unit distance 250, interior size 70)");

  const FamilySchemas& to = sound_link(t.version);
  const FamilySchemas& from = sound_link(src.version());
  std::size_t unmatched = 0;
  const auto fixup = [&](const Node& s, Node& o) {
    if (&s.schema() != &from.trigger.schema)
      return;
    const Node* su = s.ancestor(&from.user);
    if (!su)
      return;
    const NodeList& calls = su->list(user::CallTables);
    const int name_field = from.call.name_field;
    if (t.version == 73) {
      const s64 ci = s.i(from.trigger.schema.field_index("Call"));
      const int out_field = to.trigger.schema.field_index("CallName");
      if (ci >= 0 && ci < calls.size())
        o.set_s(out_field, calls.at(static_cast<int>(ci)).s(name_field));
      else
        ++unmatched;
    } else {
      const std::string& wanted = s.s(from.trigger.schema.field_index("CallName"));
      s64 found = -1;
      for (int k = 0; k < calls.size() && found < 0; ++k)
        if (calls.at(k).s(name_field) == wanted)
          found = k;
      if (found < 0)
        ++unmatched;
      o.set_i(to.trigger.schema.field_index("Call"), found);
    }
  };
  std::map<std::string, std::size_t> dropped;
  auto rebuilt = transfer(file, &to.file, dropped, fixup);
  rebuilt->set_i(file::Version, t.version);
  d.root = std::move(rebuilt);
  if (unmatched)
    losses.push_back(count_text(unmatched, "triggers name no call table of their user"));
  for (const auto& [what, n] : dropped)
    if (what.find(".Call") == std::string::npos)
      losses.push_back(count_text(n, "values of " + what + " have no place in version " + std::to_string(t.version)));
}

}  // namespace

std::vector<const Version*> conversion_targets(const Document& doc) {
  std::vector<const Version*> out;
  for (const Version& v : versions()) {
    if (v.format != doc.format)
      continue;
    if (v.system >= 0 && v.system != doc.system())
      continue;
    out.push_back(&v);
  }
  return out;
}

namespace {

// Hidden pools keep runs of the file's own words so unedited tables write back
// as found; a new byte order needs them swapped word by word.
void swap_words(std::string& bytes, std::size_t width) {
  for (std::size_t k = 0; k + width <= bytes.size(); k += width)
    std::reverse(bytes.begin() + static_cast<std::ptrdiff_t>(k), bytes.begin() + static_cast<std::ptrdiff_t>(k + width));
}

void swap_raw_pools(Document& d) {
  if (d.format != Format::XLink)
    return;
  Node& file = *d.root;
  for (const int f : {xlink::file::DirectValuePool, xlink::file::ConditionPool}) {
    std::string pool = file.s(f);
    swap_words(pool, 4);
    file.set_s(f, pool);
  }
  for (auto& u : file.list(xlink::file::Users)) {
    std::string order = u->s(xlink::user::SortedCallOrder);
    swap_words(order, 2);
    u->set_s(xlink::user::SortedCallOrder, order);
  }
}

}  // namespace

Target default_output(const Version& version, const Document& doc) {
  Target t{version.format, version.version, doc.endian, doc.encoding};
  if (version.format == doc.format && version.version == doc.version())
    return t;
  // Every preset is a real game's settings; among those using the version,
  // the one closest to the database's own disturbs it least.
  const Preset* best = nullptr;
  int best_score = -1;
  for (const Preset& p : presets()) {
    if (preset_version(p, version.format, doc.system()) != version.version)
      continue;
    const int score = (p.endian == doc.endian) * 2 + (p.encoding == doc.encoding);
    if (score > best_score) {
      best = &p;
      best_score = score;
    }
  }
  if (best) {
    t.endian = best->endian;
    t.encoding = best->encoding;
  }
  return t;
}

Document convert(const Document& doc, const Target& t, std::vector<std::string>& losses) {
  Document d = doc.clone();
  if (doc.version() == t.version && doc.endian == t.endian && doc.encoding == t.encoding)
    return d;
  if (t.format != doc.format)
    throw Error("cannot convert " + format_name(doc.format) + " to " + format_name(t.format));
  if (doc.encoding != t.encoding) {
    // Key name hashes are of the encoded name; those that match follow it.
    std::vector<Node*> hashed;
    if (doc.format == Format::XLink)
      for (auto& u : d.root->list(xlink::file::Users))
        for (auto& c : u->list(xlink::user::Calls))
          if (static_cast<u32>(c->i(xlink::call::KeyNameHash)) == crc32(c->s(xlink::call::KeyName)))
            hashed.push_back(c.get());
    if (const std::size_t lossy = transcode(*d.root, doc.encoding, t.encoding))
      losses.push_back(count_text(lossy, "strings hold characters the target encoding lacks; they become '?'"));
    for (Node* c : hashed)
      c->set_i(xlink::call::KeyNameHash, crc32(c->s(xlink::call::KeyName)));
    drop_string_layout(d);
  }
  if (doc.endian != t.endian)
    swap_raw_pools(d);
  d.endian = t.endian;
  d.encoding = t.encoding;
  if (doc.version() == t.version)
    return d;
  d.root->set_i(0, t.version);
  switch (doc.format) {
    case Format::XLink:
      convert_xlink(d, doc, t, losses);
      break;
    case Format::EffectLink:
    case Format::SoundLink:
      convert_old_link(d, doc, t, losses);
      break;
  }
  return d;
}

}  // namespace xl
