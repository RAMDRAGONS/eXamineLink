#include "model/dslink_schema.hpp"

#include <cstdio>

namespace xl::dslink {

const std::vector<std::string> UserTypeNames = {"User", "Alias", "Empty"};

namespace {

struct SlotSpec {
  SlotKind kind;
  const char* name;
  const char* parts = "";
};

std::string offset_name(int offset) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "Field0x%02X", offset);
  return buf;
}

std::string slot_name(const char* given, int index) {
  return given && *given ? std::string(given) : offset_name(index * 4);
}

// Builds a record schema from its slot_list. Range and CallRef targets resolve
// through the owning user; pooled records have no owning user and leave
// those fields unresolved.
void build_record(RecordLayout& r, const char* name, const std::vector<SlotSpec>& slot_list, FamilySchemas& fs,
                  int range_list) {
  r.schema.name = name;
  for (std::size_t i = 0; i < slot_list.size(); ++i) {
    const SlotSpec& sp = slot_list[i];
    const std::string n = slot_name(sp.name, static_cast<int>(i));
    r.slot_list.push_back({sp.kind, n, sp.parts});
    r.slot_field.push_back(static_cast<int>(r.schema.fields.size()));
    const Schema* user = &fs.user;
    switch (sp.kind) {
      case SlotKind::String:
        if (r.name_field < 0)
          r.name_field = static_cast<int>(r.schema.fields.size());
        r.schema.fields.push_back(field(n, FieldType::String));
        break;
      case SlotKind::Runtime:
        r.schema.fields.push_back(hidden_field(n, FieldType::Hex32));
        break;
      case SlotKind::Int:
        r.schema.fields.push_back(field(n, FieldType::S32));
        break;
      case SlotKind::Float:
        r.schema.fields.push_back(field(n, FieldType::F32));
        break;
      case SlotKind::Hex:
        r.schema.fields.push_back(field(n, FieldType::Hex32));
        break;
      case SlotKind::Range: {
        const auto target = [user, range_list](const Node& node) -> NodeList* {
          Node* u = const_cast<Node*>(node.ancestor(user));
          if (!u)
            return nullptr;
          return range_list == user::Assets ? asset_list_of(*u) : trigger_list_of(*u);
        };
        r.schema.fields.push_back(ref_field(n + "Start", FieldType::RangeStart, target));
        r.schema.fields.push_back(ref_field(n + "End", FieldType::RangeEnd, target));
        break;
      }
      case SlotKind::Pair:
        r.schema.fields.push_back(field(n + "Min", FieldType::S16));
        r.schema.fields.push_back(field(n + "Max", FieldType::S16));
        break;
      case SlotKind::CallRef:
        r.schema.fields.push_back(ref_field(n, FieldType::Ref, ancestor_list(user, user::CallTables)));
        break;
      case SlotKind::Packed: {
        // Parts are named by their byte within the slot.
        int at = 0;
        for (const char* c = sp.parts; *c; ++c) {
          const std::string pn = sp.name && *sp.name ? n + std::to_string(at) : offset_name(static_cast<int>(i) * 4 + at);
          const FieldType t = *c == 'b' ? FieldType::Hex8 : *c == 'H' ? FieldType::Hex16 : FieldType::S16;
          r.schema.fields.push_back(field(pn, t));
          at += *c == 'b' ? 1 : 2;
        }
        break;
      }
    }
  }
  const int nf = r.name_field;
  r.schema.label = [nf, name](const Node& node) {
    const std::string s = nf >= 0 ? node.s(nf) : std::string();
    return "#" + std::to_string(node.row()) + " " + (s.empty() ? std::string(name) : s);
  };
}

void build_common(FamilySchemas& fs, const char* family_name, const char* asset_noun) {
  const bool sound = fs.family == Family::SoundLink;
  fs.pool_ref_asset.name = std::string(asset_noun) + "Ref";
  // A user that keeps its own records still stores an index list, which the
  // game ignores; only pooled users' indices point into the pool.
  fs.pool_ref_asset.fields = {ref_field("Index", FieldType::Ref, [&fs](const Node& n) -> NodeList* {
    Node* root = const_cast<Node*>(n.root());
    const Node* u = n.ancestor(&fs.user);
    if (u && !u->i(user::PooledAssets))
      return nullptr;
    return &root->schema() == &fs.file ? &root->list(file::SharedAssets) : nullptr;
  })};
  fs.pool_ref_asset.label = [&fs](const Node& n) {
    const Node* root = n.root();
    const s64 i = n.i(pool_ref::Index);
    if (&root->schema() == &fs.file && i >= 0 && i < root->list(file::SharedAssets).size())
      return "-> " + root->list(file::SharedAssets).at(static_cast<int>(i)).label();
    return "-> #" + std::to_string(i);
  };
  fs.pool_ref_trigger.name = "TriggerRef";
  fs.pool_ref_trigger.fields = {ref_field("Index", FieldType::Ref, [&fs](const Node& n) -> NodeList* {
    Node* root = const_cast<Node*>(n.root());
    const Node* u = n.ancestor(&fs.user);
    if (u && !u->i(user::PooledTriggers))
      return nullptr;
    return &root->schema() == &fs.file ? &root->list(file::SharedTriggers) : nullptr;
  })};
  fs.pool_ref_trigger.label = [&fs](const Node& n) {
    const Node* root = n.root();
    const s64 i = n.i(pool_ref::Index);
    if (&root->schema() == &fs.file && i >= 0 && i < root->list(file::SharedTriggers).size())
      return "-> " + root->list(file::SharedTriggers).at(static_cast<int>(i)).label();
    return "-> #" + std::to_string(i);
  };
  fs.asset_param.name = std::string(asset_noun) + "Param";
  fs.asset_param.fields = {field("Value", FieldType::Hex32)};
  fs.asset_param.label = [](const Node& n) { return hex(static_cast<u32>(n.i(asset_param::Value))); };

  fs.user.name = "User";
  fs.user.fields = {
      field("Name", FieldType::String),
      field("BaseName", FieldType::String, "User this one builds on or, for aliases, stands in for"),
      enum_field("Type", &UserTypeNames),
      hidden_field("Kind", FieldType::U32),
      field("PooledAssets", FieldType::Bool, "Draw assets from the shared pool by index (3DS versions)"),
      field("PooledTriggers", FieldType::Bool, "Draw action triggers from the shared pool by index (3DS versions)"),
      field("ParamsPerAsset", FieldType::U32, "EffectLink: parameter words each effect carries"),
      // SoundLink 69 hands these to the 3D sound calculation; Mario Kart 8
      // skips a 0 and keeps its own default.
      sound ? field("MaxVolumeDistance", FieldType::F32,
                    "Version 69: distance up to which sounds play at full volume; 0 keeps the game's default "
                    "(80 in Mario Kart 8)")
            : hidden_field("Header1", FieldType::F32),
      sound ? field("UnitDistance", FieldType::F32,
                    "Version 69: distance unit of the volume falloff; 0 keeps the game's default (250 in Mario Kart 8)")
            : hidden_field("Header2", FieldType::F32),
      sound ? field("InteriorSize", FieldType::F32,
                    "Version 69: size of the sound source; 0 keeps the game's default (70 in Mario Kart 8)")
            : hidden_field("Header3", FieldType::F32),
      sound ? field("DistanceFlags", FieldType::Hex32,
                    "Version 69: bit 31 measures distance horizontally, ignoring height")
            : field("Header4", FieldType::Hex32, "Version 10: word after the parameter count; aliases keep a word here"),
      hidden_field("StringPool", FieldType::String),
      hidden_field("OwnStrings", FieldType::Bool),
      hidden_field("Tail", FieldType::String),
  };
  fs.user.lists = {{"CallTables", &fs.call.schema},       {"Actions", &fs.action.schema},
                   {asset_noun + std::string("s"), &fs.asset.schema},
                   {"Triggers", &fs.trigger.schema},      {asset_noun + std::string("Refs"), &fs.pool_ref_asset},
                   {"TriggerRefs", &fs.pool_ref_trigger}};
  if (fs.family == Family::EffectLink) {
    fs.asset.schema.lists = {{"Params", &fs.asset_param, false, "Parameter words this user gives the effect"}};
    fs.pool_ref_asset.lists = {{"Params", &fs.asset_param, false, "Parameter words this user gives the effect"}};
  }
  fs.user.label = [](const Node& n) {
    std::string s = n.s(user::Name);
    if (n.i(user::Type) == UT_Alias)
      s += "  -> " + n.s(user::BaseName);
    return s;
  };

  fs.file.name = family_name;
  fs.file.fields = {field("Version", FieldType::U32, "Database version written on save"),
                    hidden_field("StringPool", FieldType::String)};
  fs.file.lists = {{std::string("Shared") + asset_noun + "s", &fs.asset.schema},
                   {"SharedTriggers", &fs.trigger.schema},
                   {"Users", &fs.user}};
  fs.file.label = [family_name](const Node& n) {
    return std::string(family_name) + " v" + std::to_string(n.i(file::Version));
  };
}

using K = SlotKind;

// Schemas point at each other and lambdas capture their addresses, so a set is
// filled in place and never copied.
void fill_effect_link(FamilySchemas& fs) {
  fs.family = Family::EffectLink;
  build_record(fs.call, "EffectCallTable",
               {{K::String, "Name"}, {K::Runtime, ""}, {K::Runtime, ""}, {K::Packed, "Flags", "bbbb"},
                {K::Range, "Effect"}},
               fs,
               user::Assets);
  build_record(fs.action, "EffectAction", {{K::String, "Name"}, {K::Runtime, ""}, {K::Range, "Trigger"}}, fs,
               user::Triggers);
  std::vector<SlotSpec> effect = {{K::Int, "Id"},           {K::Runtime, ""},  {K::String, "Condition1"},
                                  {K::Runtime, ""},         {K::String, "Condition2"}, {K::Runtime, ""},
                                  {K::String, "Condition3"}, {K::Runtime, ""}, {K::String, "EmitterSet"},
                                  {K::Runtime, ""},         {K::String, "Bone"}, {K::String, "Group"},
                                  {K::Packed, "", "bbbb"},  {K::Packed, "", "bbbb"}, {K::Packed, "", "hbb"}};
  for (const char* f : {"Scale", "PositionX", "PositionY", "PositionZ", "RotationX", "RotationY", "RotationZ", "Red",
                        "Green", "Blue", "Alpha"})
    effect.push_back({K::Float, f});
  build_record(fs.asset, "Effect", effect, fs, -1);
  // The emitter set names an effect better than its first condition does.
  const int eset = fs.asset.slot_field[8];
  fs.asset.schema.label = [eset](const Node& n) { return "#" + std::to_string(n.row()) + " " + n.s(eset); };
  // A trigger fires while StartFrame <= frame < EndFrame. A nonzero
  // HandoverId lets the trigger of the next action with the same id take the
  // running emitter over instead of starting it again. The floats override
  // the effect's scale, position and rotation.
  std::vector<SlotSpec> trig = {{K::Int, "Id"},     {K::Int, "HandoverId"}, {K::String, "CallName"},
                                {K::Runtime, ""},   {K::Int, "StartFrame"}, {K::Int, "EndFrame"},
                                {K::Packed, "Flags", "Hbb"}, {K::String, "Bone"},
                                {K::Float, "Scale"}, {K::Float, "PositionX"}, {K::Float, "PositionY"},
                                {K::Float, "PositionZ"}, {K::Float, "RotationX"}, {K::Float, "RotationY"},
                                {K::Float, "RotationZ"}};
  build_record(fs.trigger, "EffectTrigger", trig, fs, -1);
  build_common(fs, "EffectLink", "Effect");
}

void fill_sound_link(FamilySchemas& fs, u32 version) {
  fs.family = Family::SoundLink;
  build_record(fs.call, "SoundCallTable",
               {{K::String, "Name"}, {K::Runtime, ""}, {K::Runtime, ""}, {K::Packed, "Flags", "bbbb"},
                {K::Range, "Asset"}},
               fs,
               user::Assets);
  build_record(fs.action, "SoundAction",
               {{K::String, "Name"}, {K::Runtime, ""}, {K::Hex, ""}, {K::Hex, ""}, {K::Range, "Trigger"}}, fs,
               user::Triggers);
  // Sound parameters as ranges: a value, and the top of the range a random
  // value is drawn from when it lies above the value (0 for none). Version 69
  // adds a surround pan; the names match across versions so records convert
  // field by field.
  const std::vector<SlotSpec> params4 = {{K::Float, "Volume"}, {K::Float, "VolumeMax"}, {K::Float, "Pitch"},
                                         {K::Float, "PitchMax"}, {K::Float, "Lpf"}, {K::Float, "LpfMax"},
                                         {K::Float, "Pan"}, {K::Float, "PanMax"}};
  const auto append = [](std::vector<SlotSpec>& v, const std::vector<SlotSpec>& more) {
    v.insert(v.end(), more.begin(), more.end());
  };
  std::vector<SlotSpec> asset = {{K::Int, "Id"},       {K::Runtime, ""}, {K::String, "Name08"}, {K::Int, ""},
                                 {K::String, "Name10"}, {K::Int, ""},     {K::String, "Label"},  {K::Int, "LabelId"}};
  // EndFrame -1 lasts until the action ends.
  std::vector<SlotSpec> trig = {{K::Int, "Id"},         {K::Runtime, ""},      {K::String, "CallName"},
                                {K::Runtime, ""},       {K::Int, "StartFrame"}, {K::Int, "EndFrame"},
                                {K::Packed, "Flags", "Hh"}};
  if (version == 69) {
    asset.push_back({K::String, "Name20"});
    asset.push_back({K::Runtime, ""});
    trig[2] = {K::CallRef, "Call"};
  }
  append(asset, params4);
  append(trig, params4);
  if (version == 69) {
    append(asset, {{K::Float, "SurroundPan"}, {K::Float, "SurroundPanMax"}});
    append(trig, {{K::Float, "SurroundPan"}, {K::Float, "SurroundPanMax"}});
  }
  append(asset, {{K::Pair, "Delay"}, {K::String, "Bone"}, {K::Packed, "Flags", "bbbb"}});
  trig.push_back({K::Pair, "Delay"});
  if (version == 69)
    trig.push_back({K::Hex, ""});
  build_record(fs.asset, "SoundAsset", asset, fs, -1);
  build_record(fs.trigger, "SoundTrigger", trig, fs, -1);
  const int label = fs.asset.slot_field[6];
  fs.asset.schema.label = [label](const Node& n) { return "#" + std::to_string(n.row()) + " " + n.s(label); };
  build_common(fs, "SoundLink", "Asset");
}

}  // namespace

const FamilySchemas& effect_link() {
  static FamilySchemas fs;
  static const bool filled = (fill_effect_link(fs), true);
  (void)filled;
  return fs;
}

const FamilySchemas& sound_link(u32 version) {
  static FamilySchemas v69, v73;
  static const bool filled = (fill_sound_link(v69, 69), fill_sound_link(v73, 73), true);
  (void)filled;
  return version == 69 ? v69 : v73;
}

const FamilySchemas* family_of(const Node& root) {
  for (const FamilySchemas* fs : {&effect_link(), &sound_link(69), &sound_link(73)})
    if (&root.schema() == &fs->file)
      return fs;
  return nullptr;
}

NodeList* asset_list_of(const Node& u) {
  Node& n = const_cast<Node&>(u);
  return n.i(user::PooledAssets) ? &n.list(user::AssetRefs) : &n.list(user::Assets);
}

NodeList* trigger_list_of(const Node& u) {
  Node& n = const_cast<Node&>(u);
  return n.i(user::PooledTriggers) ? &n.list(user::TriggerRefs) : &n.list(user::Triggers);
}

}  // namespace xl::dslink
