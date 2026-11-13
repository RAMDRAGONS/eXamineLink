#include "model/xlink_schema.hpp"

#include <cstdio>

namespace xl::xlink {

const std::vector<std::string> ParamTypeNames = {"Int", "Float", "Bool", "Enum", "String", "Bitfield"};
const std::vector<std::string> RefTypeNames = {
    "Direct",          "String",          "Curve",           "Random",          "ArrangeGroup",    "Bitflag",
    "Random2Pow",      "Random3Pow",      "Random4Pow",      "Random1.5Pow",    "Random2PowMin",   "Random3PowMin",
    "Random4PowMin",   "Random1.5PowMin", "Random2PowMax",   "Random3PowMax",   "Random4PowMax",   "Random1.5PowMax",
};
const std::vector<std::string> ContainerTypeNames = {"Switch", "Random", "Random2", "Blend", "Sequence", "Grid", "Jump"};
const std::vector<std::string> PropertyTypeNames = {"Enum", "S32", "F32"};
const std::vector<std::string> CurveUpdateTypeNames = {"Update", "NoUpdate"};
const std::vector<std::string> CompareTypeNames = {"Equal",    "GreaterThan",     "GreaterThanOrEqual",
                                                   "LessThan", "LessThanOrEqual", "NotEqual"};

System system_of(const Node& file) {
  for (const auto& d : file.list(file::AssetParamDefs))
    if (d->s(pdef::Name) == "Eset")
      return System::ELink;
  return System::SLink;
}

bool is_random_ref(s64 t) {
  return t == RT_Random || (t >= 6 && t <= RT_RandomLast);
}

bool is_direct_table_ref(s64 t) {
  return t == RT_Direct || t == RT_Bitflag;
}

namespace {

std::string enum_text(const std::vector<std::string>& names, s64 v) {
  return v >= 0 && v < static_cast<s64>(names.size()) ? names[v] : std::to_string(v);
}

std::string float_text(f64 v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

std::string range_text(s64 a, s64 b) {
  if (b < a)
    return "[]";
  return "[" + std::to_string(a) + ".." + std::to_string(b) + "]";
}

const Node* file_of(const Node& n) {
  return n.ancestor(&schemas().file);
}

void build(Schemas& s) {
  const auto file_list = [&s](int l) { return ancestor_list(&s.file, l); };
  const auto user_list = [&s](int l) { return ancestor_list(&s.user, l); };

  // Parameter definitions: the default's meaning follows the declared type.
  s.pdef.name = "ParamDefine";
  s.pdef.fields = {field("Name", FieldType::String), enum_field("Type", &ParamTypeNames),
                   field("Default", FieldType::Dynamic, "Value used when an asset does not set the parameter")};
  s.pdef.fields[pdef::Default].dyn_kind = [](const Node& n) {
    switch (n.i(pdef::Type)) {
      case PT_Float:
        return DynKind::Float64;
      case PT_String:
        return DynKind::String;
      case PT_Bool:
        return DynKind::Bool;
      default:
        return DynKind::Int64;
    }
  };
  s.pdef.label = [](const Node& n) { return n.s(pdef::Name) + " : " + enum_text(ParamTypeNames, n.i(pdef::Type)); };

  s.name.name = "Name";
  s.name.fields = {field("Name", FieldType::String)};
  s.name.label = [](const Node& n) { return n.s(name::Name); };

  s.point.name = "Point";
  s.point.fields = {field("X", FieldType::F32), field("Y", FieldType::F32)};
  s.point.label = [](const Node& n) {
    return "(" + float_text(n.f32v(point::X)) + ", " + float_text(n.f32v(point::Y)) + ")";
  };

  s.curve.name = "Curve";
  s.curve.fields = {field("CurveType", FieldType::U16), field("IsPropGlobal", FieldType::U16),
                    field("PropName", FieldType::String, "Property the curve samples"),
                    field("PropIdx", FieldType::S32), field("LocalPropertyNameIdx", FieldType::S16),
                    enum_field("UpdateType", &CurveUpdateTypeNames,
                               "Whether the curve follows the property as it changes (Update) or not (NoUpdate)")};
  s.curve.lists = {{"Points", &s.point}};
  s.curve.label = [](const Node& n) {
    return "#" + std::to_string(n.row()) + " " + n.s(curve::PropName) + " (" +
           std::to_string(n.list(curve::Points).size()) + " points)";
  };

  s.random.name = "Random";
  s.random.fields = {field("Min", FieldType::F32), field("Max", FieldType::F32)};
  s.random.label = [](const Node& n) {
    return "#" + std::to_string(n.row()) + " " + float_text(n.f32v(random::Min)) + " .. " + float_text(n.f32v(random::Max));
  };

  s.arrange_entry.name = "ArrangeGroupEntry";
  s.arrange_entry.fields = {field("GroupName", FieldType::String), field("LimitType", FieldType::U8),
                            field("LimitThreshold", FieldType::U8), field("Unk", FieldType::U8),
                            hidden_field("Pad", FieldType::U8)};
  s.arrange_entry.label = [](const Node& n) { return n.s(arrange_entry::GroupName); };

  s.arrange.name = "ArrangeGroup";
  s.arrange.lists = {{"Entries", &s.arrange_entry}};
  s.arrange.label = [](const Node& n) {
    return "#" + std::to_string(n.row()) + " (" + std::to_string(n.list(arrange::Entries).size()) + " entries)";
  };

  // A parameter's value means different things by reference type: an index
  // into the curve, random or arrange tables, a string, or a literal.
  s.param.name = "Param";
  s.param.fields = {ref_field("Def", FieldType::Ref,
                              [](const Node& n) { return const_cast<NodeList*>(param_defs_for(n)); },
                              "The parameter definition this sets"),
                    enum_field("RefType", &RefTypeNames), field("Value", FieldType::Dynamic),
                    hidden_field("DirectHint", FieldType::S64)};
  s.param.fields[param::Value].dyn_kind = [](const Node& n) {
    const s64 rt = n.i(param::RefType);
    if (rt == RT_String)
      return DynKind::String;
    if (rt == RT_Curve || rt == RT_ArrangeGroup || is_random_ref(rt))
      return DynKind::Ref;
    if (rt == RT_Bitflag)
      return DynKind::Hex;
    const Node* def = param_def(n);
    if (!def)
      return DynKind::Hex;
    switch (def->i(pdef::Type)) {
      case PT_Float:
        return DynKind::Float32;
      case PT_Bool:
        return DynKind::Bool;
      case PT_Int:
      case PT_Enum:
        return DynKind::Int;
      default:
        return DynKind::Hex;
    }
  };
  s.param.fields[param::Value].target = [](const Node& n) -> NodeList* {
    Node* f = const_cast<Node*>(file_of(n));
    if (!f)
      return nullptr;
    const s64 rt = n.i(param::RefType);
    if (rt == RT_Curve)
      return &f->list(file::Curves);
    if (rt == RT_ArrangeGroup)
      return &f->list(file::ArrangeGroups);
    if (is_random_ref(rt))
      return &f->list(file::Randoms);
    return nullptr;
  };
  s.param.label = [](const Node& n) {
    const Node* def = param_def(n);
    const std::string nm = def ? def->s(pdef::Name) : "#" + std::to_string(n.i(param::Def));
    const s64 rt = n.i(param::RefType);
    std::string v;
    if (rt == RT_String)
      v = "\"" + n.s(param::Value) + "\"";
    else if (rt == RT_Direct && def && def->i(pdef::Type) == PT_Float)
      v = float_text(n.f32v(param::Value));
    else if (rt == RT_Direct || rt == RT_Bitflag)
      v = std::to_string(n.i(param::Value));
    else
      v = enum_text(RefTypeNames, rt) + " #" + std::to_string(n.i(param::Value));
    return nm + " = " + v;
  };

  s.pset.name = "ParamSet";
  s.pset.lists = {{"Params", &s.param}};
  s.pset.label = [](const Node& n) {
    return "#" + std::to_string(n.row()) + " (" + std::to_string(n.list(pset::Params).size()) + " params)";
  };

  s.cond.name = "Condition";
  s.cond.fields = {enum_field("ParentType", &ContainerTypeNames, "Type of the container the condition belongs to"),
                   enum_field("PropertyType", &PropertyTypeNames),
                   enum_field("CompareType", &CompareTypeNames),
                   field("Value", FieldType::Dynamic, "Switch value; an enum name for enum properties"),
                   field("LocalPropertyEnumNameIdx", FieldType::S32),
                   field("IsSolved", FieldType::U8),
                   field("IsGlobal", FieldType::U8),
                   field("Weight", FieldType::F32, "Random and Random2 containers"),
                   field("BlendMin", FieldType::F32),
                   field("BlendMax", FieldType::F32),
                   field("BlendTypeToMax", FieldType::U8),
                   field("BlendTypeToMin", FieldType::U8),
                   field("ContinueOnFade", FieldType::S32, "Sequence containers"),
                   hidden_field("PoolOffset", FieldType::S64)};
  s.cond.fields[cond::Value].dyn_kind = [](const Node& n) {
    switch (n.i(cond::PropertyType)) {
      case PR_Enum:
        return DynKind::String;
      case PR_F32:
        return DynKind::Float32;
      default:
        return DynKind::Int;
    }
  };
  s.cond.label = [](const Node& n) {
    const s64 t = n.i(cond::ParentType);
    std::string d = "#" + std::to_string(n.row()) + " " + enum_text(ContainerTypeNames, t);
    if (t == CT_Switch) {
      const s64 pt = n.i(cond::PropertyType);
      d += " " + enum_text(CompareTypeNames, n.i(cond::CompareType)) + " ";
      d += pt == PR_Enum ? n.s(cond::Value) : pt == PR_F32 ? float_text(n.f32v(cond::Value)) : std::to_string(n.i(cond::Value));
    } else if (t == CT_Random || t == CT_Random2) {
      d += " weight " + float_text(n.f32v(cond::Weight));
    } else if (t == CT_Blend) {
      d += " " + float_text(n.f32v(cond::BlendMin)) + " .. " + float_text(n.f32v(cond::BlendMax));
    }
    return d;
  };

  s.call.name = "AssetCall";
  s.call.fields = {
      field("KeyName", FieldType::String),
      field("AssetId", FieldType::S32, "Index among the user's assets; -1 for containers"),
      field("Flag", FieldType::Hex32, "Bit 0 container, bit 3 one-shot, bit 4 not paused with the game; bits 8 to 15 are the game's own"),
      field("Duration", FieldType::S32),
      ref_field("ParentIndex", FieldType::Ref, user_list(user::Calls), "Call whose container holds this one"),
      field("Guid", FieldType::Hex32),
      field("KeyNameHash", FieldType::Hex32, "CRC-32 of the key name"),
      ref_field("ParamSet", FieldType::Ref, file_list(file::AssetParamSets), "Asset parameters, for assets"),
      ref_field("Container", FieldType::Ref, user_list(user::Containers), "Container parameters, for containers"),
      ref_field("Condition", FieldType::Ref, file_list(file::Conditions), "Condition checked by the parent container"),
  };
  s.call.label = [](const Node& n) {
    return n.s(call::KeyName) + (call_is_container(n.i(call::Flag)) ? "  [container]" : "");
  };

  s.grid_value.name = "GridValue";
  s.grid_value.fields = {field("Value", FieldType::Hex32)};
  s.grid_value.label = [](const Node& n) { return hex(static_cast<u32>(n.i(grid_value::Value))); };
  s.grid_cell.name = "GridCell";
  s.grid_cell.fields = {ref_field("Call", FieldType::Ref, user_list(user::Calls))};
  s.grid_cell.label = [](const Node& n) { return "call " + std::to_string(n.i(grid_cell::Call)); };

  s.ctn.name = "Container";
  s.ctn.fields = {
      enum_field("Type", &ContainerTypeNames),
      ref_field("ChildStart", FieldType::RangeStart, user_list(user::Calls)),
      ref_field("ChildEnd", FieldType::RangeEnd, user_list(user::Calls)),
      field("WatchPropertyName", FieldType::String, "Switch and grid containers"),
      field("WatchPropertyId", FieldType::S32),
      field("LocalPropertyNameIdx", FieldType::S16),
      field("IsGlobal", FieldType::U8),
      field("WatchPropertyName2", FieldType::String, "Second axis of a grid container"),
      field("LocalPropertyNameIdx2", FieldType::S16),
      field("GridFlags1", FieldType::U8),
      field("GridFlags2", FieldType::U8),
      field("SwitchByte", FieldType::U8, "Byte after IsGlobal in switch containers; 64-bit versions set it to switch on an action slot's action instead of a property"),
      field("BlendByProperty", FieldType::Bool,
            "Blend containers (64-bit versions): blend by the watched property's value, with switch container fields"),
  };
  s.ctn.lists = {{"Axis1", &s.grid_value}, {"Axis2", &s.grid_value}, {"Cells", &s.grid_cell}};
  s.ctn.label = [](const Node& n) {
    std::string d = "#" + std::to_string(n.row()) + " " + enum_text(ContainerTypeNames, n.i(ctn::Type)) +
                    (n.i(ctn::BlendByProperty) ? "By" : "") + " " + range_text(n.i(ctn::ChildStart), n.i(ctn::ChildEnd));
    if (!n.s(ctn::WatchPropertyName).empty())
      d += " " + n.s(ctn::WatchPropertyName);
    return d;
  };

  s.slot.name = "ActionSlot";
  s.slot.fields = {field("Name", FieldType::String),
                   ref_field("ActionStart", FieldType::RangeStart, user_list(user::Actions)),
                   ref_field("ActionEnd", FieldType::RangeEnd, user_list(user::Actions))};
  s.slot.label = [](const Node& n) {
    return n.s(slot::Name) + " " + range_text(n.i(slot::ActionStart), n.i(slot::ActionEnd));
  };

  s.action.name = "Action";
  s.action.fields = {field("Name", FieldType::String),
                     ref_field("TriggerStart", FieldType::RangeStart, user_list(user::ActionTriggers)),
                     ref_field("TriggerEnd", FieldType::RangeEnd, user_list(user::ActionTriggers)),
                     field("Flag", FieldType::Hex16, "64-bit versions: the low byte marks the name as a prefix of the actions it matches")};
  s.action.label = [](const Node& n) {
    return n.s(action::Name) + " " + range_text(n.i(action::TriggerStart), n.i(action::TriggerEnd));
  };

  const auto call_name = [](const Node& n, int f) {
    const Node* u = n.ancestor(&schemas().user);
    const s64 c = n.i(f);
    if (!u || c < 0 || c >= u->list(user::Calls).size())
      return std::string("(no call)");
    return u->list(user::Calls).at(static_cast<int>(c)).s(call::KeyName);
  };

  s.atrig.name = "ActionTrigger";
  s.atrig.fields = {field("Guid", FieldType::Hex32),
                    ref_field("AssetCall", FieldType::Ref, user_list(user::Calls)),
                    field("StartFrame", FieldType::S32),
                    field("PreviousActionName", FieldType::String,
                          "Used instead of the start frame when the flags select a previous action"),
                    field("EndFrame", FieldType::S32),
                    field("Flag", FieldType::Hex16,
                          "Bit 0 fires once per action; bit 2 fires whenever the action is set, bit 3 when it "
                          "leaves, bit 4 after the previous action; none of these: within the frame window"),
                    field("OverwriteHash", FieldType::Hex16),
                    ref_field("Overwrite", FieldType::Ref, file_list(file::TriggerParamSets))};
  s.atrig.label = [call_name](const Node& n) {
    std::string d = call_name(n, atrig::AssetCall);
    if (trigger_names_previous_action(n.i(atrig::Flag)))
      d += " after " + n.s(atrig::PreviousActionName);
    else
      d += " @" + std::to_string(n.i(atrig::StartFrame)) + ".." + std::to_string(n.i(atrig::EndFrame));
    return d;
  };

  s.prop.name = "Property";
  s.prop.fields = {field("Name", FieldType::String), field("IsGlobal", FieldType::S32),
                   ref_field("TriggerStart", FieldType::RangeStart, user_list(user::PropertyTriggers)),
                   ref_field("TriggerEnd", FieldType::RangeEnd, user_list(user::PropertyTriggers))};
  s.prop.label = [](const Node& n) {
    return n.s(prop::Name) + " " + range_text(n.i(prop::TriggerStart), n.i(prop::TriggerEnd));
  };

  s.ptrig.name = "PropertyTrigger";
  s.ptrig.fields = {field("Guid", FieldType::Hex32),
                    ref_field("AssetCall", FieldType::Ref, user_list(user::Calls)),
                    ref_field("Condition", FieldType::Ref, file_list(file::Conditions)),
                    field("Flag", FieldType::Hex16),
                    field("OverwriteHash", FieldType::Hex16),
                    ref_field("Overwrite", FieldType::Ref, file_list(file::TriggerParamSets))};
  s.ptrig.label = [call_name](const Node& n) { return call_name(n, ptrig::AssetCall); };

  s.awtrig.name = "AlwaysTrigger";
  s.awtrig.fields = {field("Guid", FieldType::Hex32),
                     ref_field("AssetCall", FieldType::Ref, user_list(user::Calls)),
                     field("Flag", FieldType::Hex16),
                     field("OverwriteHash", FieldType::Hex16),
                     ref_field("Overwrite", FieldType::Ref, file_list(file::TriggerParamSets))};
  s.awtrig.label = [call_name](const Node& n) { return call_name(n, awtrig::AssetCall); };

  s.user.name = "User";
  s.user.fields = {field("Name", FieldType::String,
                         "Version 19 stores names; later versions keep only the hash, so names are optional there"),
                   field("Hash", FieldType::Hex32, "CRC-32 of the name, the key later versions look users up by"),
                   field("BaseName", FieldType::String, "User this one is based on (version 19)"),
                   hidden_field("SortedCallOrder", FieldType::String),
                   field("GlobalPropertyCount", FieldType::U16,
                         "Distinct global properties the user reads (ELink2 36, SLink2 33 and later); the game's "
                         "loader does not read it, and shipped databases do not always match their contents")};
  s.user.lists = {{"LocalProperties", &s.name},       {"UserParams", &s.param},
                  {"Calls", &s.call},                 {"Containers", &s.ctn},
                  {"ActionSlots", &s.slot},           {"Actions", &s.action},
                  {"ActionTriggers", &s.atrig},       {"Properties", &s.prop},
                  {"PropertyTriggers", &s.ptrig},     {"AlwaysTriggers", &s.awtrig}};
  s.user.label = [](const Node& n) {
    return n.s(user::Name).empty() ? hex(static_cast<u32>(n.i(user::Hash)), 8) : n.s(user::Name);
  };

  s.file.name = "XLink";
  s.file.fields = {field("Version", FieldType::U32, "Resource version written on save"),
                   field("NumCustomAssetParam", FieldType::U32,
                         "How many of the last asset parameter definitions the game defines"),
                   hidden_field("NameTablePool", FieldType::String),
                   hidden_field("PdtStringPool", FieldType::String),
                   hidden_field("DirectValuePool", FieldType::String),
                   hidden_field("ConditionPool", FieldType::String),
                   hidden_field("ConditionCountBias", FieldType::S64)};
  s.file.lists = {{"UserParamDefines", &s.pdef},     {"AssetParamDefines", &s.pdef},
                  {"TriggerParamDefines", &s.pdef},  {"LocalPropertyNames", &s.name},
                  {"LocalPropertyEnumNames", &s.name}, {"Curves", &s.curve},
                  {"RandomTable", &s.random},        {"ArrangeGroups", &s.arrange},
                  {"AssetParamSets", &s.pset},       {"TriggerParamSets", &s.pset},
                  {"Conditions", &s.cond},           {"Users", &s.user}};
  s.file.label = [](const Node& n) {
    return std::string(system_of(n) == System::ELink ? "ELink2" : "SLink2") + " v" + std::to_string(n.i(file::Version));
  };
}

}  // namespace

// Filled in place: schemas point at one another, so a copy would dangle.
const Schemas& schemas() {
  static Schemas s;
  static const bool filled = (build(s), true);
  (void)filled;
  return s;
}

const NodeList* param_defs_for(const Node& param) {
  const Node* f = file_of(param);
  if (!f)
    return nullptr;
  const Node* holder = param.parent();
  if (!holder)
    return nullptr;
  if (&holder->schema() == &schemas().user)
    return &f->list(file::UserParamDefs);
  const NodeList* sets = holder->parent_list();
  if (sets && sets->owner() == f)
    return sets->index() == file::TriggerParamSets ? &f->list(file::TriggerParamDefs) : &f->list(file::AssetParamDefs);
  return nullptr;
}

const Node* param_def(const Node& param) {
  const NodeList* defs = param_defs_for(param);
  const s64 i = param.i(param::Def);
  if (!defs || i < 0 || i >= defs->size())
    return nullptr;
  return &defs->at(static_cast<int>(i));
}

}  // namespace xl::xlink
