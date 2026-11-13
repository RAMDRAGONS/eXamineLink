#include "model/factory.hpp"

#include <random>
#include <set>

#include "core/crc32.hpp"
#include "model/dslink_schema.hpp"
#include "model/xlink_schema.hpp"

namespace xl {

namespace {

u32 random_guid() {
  static std::mt19937 rng(std::random_device{}());
  return std::uniform_int_distribution<u32>(1, 0x7FFFFFFF)(rng);
}

std::unique_ptr<Node> default_param(const Node& def, int index) {
  using namespace xlink;
  auto p = std::make_unique<Node>(&schemas().param);
  p->set_i(param::Def, index);
  p->set_i(param::DirectHint, -1);
  switch (def.i(pdef::Type)) {
    case PT_String:
      p->set_i(param::RefType, RT_String);
      p->set_s(param::Value, def.s(pdef::Default));
      break;
    case PT_Float:
      p->set_i(param::RefType, RT_Direct);
      p->set_i(param::Value, f32_bits(static_cast<f32>(def.f64v(pdef::Default))));
      break;
    default:
      p->set_i(param::RefType, RT_Direct);
      p->set_i(param::Value, static_cast<u32>(def.i(pdef::Default)));
      break;
  }
  return p;
}

}  // namespace

std::unique_ptr<Node> make_element(const Document& doc, const NodeList& list) {
  const Schema* el = list.def().element;
  auto n = std::make_unique<Node>(el);
  if (is_xlink(doc.format)) {
    using namespace xlink;
    const auto& S = schemas();
    const Node& file = *doc.root;
    if (el == &S.user) {
      n->set_s(user::Name, "NewUser");
      const NodeList& defs = file.list(file::UserParamDefs);
      for (int k = 0; k < defs.size(); ++k)
        n->list(user::UserParams).append(default_param(defs.at(k), k));
    } else if (el == &S.call) {
      n->set_s(call::KeyName, "NewAsset");
      n->set_i(call::KeyNameHash, crc32("NewAsset"));
      n->set_i(call::Guid, random_guid());
      n->set_i(call::Duration, 1);
      n->set_i(call::ParentIndex, -1);
    } else if (el == &S.atrig || el == &S.ptrig || el == &S.awtrig) {
      n->set_i(0, random_guid());
    } else if (el == &S.param) {
      // The first definition this set does not give yet.
      const NodeList* defs = nullptr;
      if (&list.owner()->schema() == &S.user)
        defs = &file.list(file::UserParamDefs);
      else if (const NodeList* sets = list.owner()->parent_list())
        defs = sets->index() == file::TriggerParamSets ? &file.list(file::TriggerParamDefs)
                                                       : &file.list(file::AssetParamDefs);
      std::set<s64> used;
      for (const auto& p : list)
        used.insert(p->i(param::Def));
      if (defs)
        for (int k = 0; k < defs->size(); ++k)
          if (!used.contains(k))
            return default_param(defs->at(k), k);
      n->set_i(param::Def, 0);
    } else if (el == &S.cond) {
      n->set_i(cond::LocalPropertyEnumNameIdx, -1);
      n->set_i(cond::PoolOffset, -1);
    } else if (el == &S.ctn) {
      n->set_i(ctn::WatchPropertyId, -1);
      n->set_i(ctn::LocalPropertyNameIdx, -1);
      n->set_i(ctn::LocalPropertyNameIdx2, -1);
    } else if (el == &S.curve) {
      n->set_i(curve::LocalPropertyNameIdx, -1);
      n->set_i(curve::PropIdx, -1);
    } else if (el == &S.pdef) {
      n->set_s(pdef::Name, "NewParam");
    }
    return n;
  }

  using namespace dslink;
  const FamilySchemas* fs = family_of(*doc.root);
  if (!fs)
    return n;
  if (el == &fs->user) {
    n->set_s(user::Name, "NewUser");
    n->set_i(user::Type, UT_User);
  } else if (doc.format == Format::ELink && (el == &fs->asset.schema || el == &fs->pool_ref_asset) &&
             &list.owner()->schema() == &fs->user) {
    // Every effect of a user carries the same number of parameter words.
    const s64 words = list.owner()->i(user::ParamsPerAsset);
    for (s64 k = 0; k < words; ++k)
      n->list(slot_params::Params).append_new();
  }
  if (el == &fs->pool_ref_asset || el == &fs->pool_ref_trigger)
    n->set_i(pool_ref::Index, 0);
  return n;
}

std::vector<FieldUpdate> follow_up_edits(const Document& doc, Node& n, int field, const Value& next) {
  std::vector<FieldUpdate> out;
  if (!is_xlink(doc.format))
    return out;
  using namespace xlink;
  const auto& S = schemas();
  if (&n.schema() == &S.user && field == user::Name) {
    // Later versions find users by the hash of their name, so the hash
    // follows the name; clearing the name keeps the last hash.
    Value v;
    const std::string& old = n.s(user::Name);
    v.i = !next.s.empty() ? crc32(next.s) : old.empty() ? n.i(user::Hash) : crc32(old);
    out.push_back({&n, user::Hash, v});
  } else if (&n.schema() == &S.call && field == call::KeyName) {
    if (static_cast<u32>(n.i(call::KeyNameHash)) == crc32(n.s(call::KeyName)) || n.i(call::KeyNameHash) == 0) {
      Value v;
      v.i = crc32(next.s);
      out.push_back({&n, call::KeyNameHash, v});
    }
  }
  return out;
}

}  // namespace xl
