#pragma once

#include "model/node.hpp"

// One model for every XLink resource version. Fields a layout lacks keep
// their defaults when read and are dropped when written, which is what makes
// saving into another version possible.
namespace xl::xlink {

extern const std::vector<std::string> SystemNames;
extern const std::vector<std::string> ParamTypeNames;
extern const std::vector<std::string> RefTypeNames;
extern const std::vector<std::string> ContainerTypeNames;
extern const std::vector<std::string> PropertyTypeNames;
extern const std::vector<std::string> CompareTypeNames;
extern const std::vector<std::string> CurveUpdateTypeNames;

enum class System { ELink, SLink };
enum ParamType { PT_Int, PT_Float, PT_Bool, PT_Enum, PT_String, PT_Bitfield };
enum RefType { RT_Direct, RT_String, RT_Curve, RT_Random, RT_ArrangeGroup, RT_Bitflag, RT_RandomLast = 0x11 };
enum ContainerType { CT_Switch, CT_Random, CT_Random2, CT_Blend, CT_Sequence, CT_Grid, CT_Jump };
enum PropertyType { PR_Enum, PR_S32, PR_F32 };

bool is_random_ref(s64 ref_type);
bool is_direct_table_ref(s64 ref_type);

namespace file {
enum Field {
  Version,
  System,
  NumCustomAssetParam,
  // Original tables of the version the file was read from; reusing them keeps
  // an unedited file byte-identical.
  NameTablePool,
  PdtStringPool,
  DirectValuePool,
  ConditionPool,
  ConditionCountBias,
};
enum List {
  UserParamDefs,
  AssetParamDefs,
  TriggerParamDefs,
  LocalPropertyNames,
  LocalPropertyEnumNames,
  Curves,
  Randoms,
  ArrangeGroups,
  AssetParamSets,
  TriggerParamSets,
  Conditions,
  Users,
};
}  // namespace file

namespace pdef {
enum Field { Name, Type, Default };
}
namespace name {
enum Field { Name };
}
namespace curve {
enum Field { CurveType, IsPropGlobal, PropName, PropIdx, LocalPropertyNameIdx, UpdateType };
enum List { Points };
}  // namespace curve
namespace point {
enum Field { X, Y };
}
namespace random {
enum Field { Min, Max };
}
namespace arrange {
enum List { Entries };
}
namespace arrange_entry {
enum Field { GroupName, LimitType, LimitThreshold, Unk, Pad };
}
namespace pset {
enum List { Params };
}
namespace param {
enum Field { Def, RefType, Value, DirectHint };
}
namespace cond {
enum Field {
  ParentType,
  PropertyType,
  CompareType,
  Value,
  LocalPropertyEnumNameIdx,
  IsSolved,
  IsGlobal,
  Weight,
  BlendMin,
  BlendMax,
  BlendTypeToMax,
  BlendTypeToMin,
  ContinueOnFade,
  PoolOffset,
};
}
// Reference value for a position just past the end of a table, which some
// shipped databases contain; kept so they write back unchanged.
constexpr s64 RefTableEnd = -2;

namespace user {
enum Field { Name, Hash, BaseName, SortedCallOrder };
enum List {
  LocalProperties,
  UserParams,
  Calls,
  Containers,
  ActionSlots,
  Actions,
  ActionTriggers,
  Properties,
  PropertyTriggers,
  AlwaysTriggers,
};
}  // namespace user
namespace call {
enum Field { KeyName, AssetId, Flag, Duration, ParentIndex, Guid, KeyNameHash, ParamSet, Container, Condition };
}
namespace ctn {
enum Field {
  Type,
  ChildStart,
  ChildEnd,
  WatchPropertyName,
  WatchPropertyId,
  LocalPropertyNameIdx,
  IsGlobal,
  WatchPropertyName2,
  LocalPropertyNameIdx2,
  GridFlags1,
  GridFlags2,
  SwitchByte,
  BlendByProperty,
};
enum List { Axis1, Axis2, Cells };
}  // namespace ctn
namespace grid_value {
enum Field { Value };
}
namespace grid_cell {
enum Field { Call };
}
namespace slot {
enum Field { Name, ActionStart, ActionEnd };
}
namespace action {
enum Field { Name, TriggerStart, TriggerEnd, Flag };
}
namespace atrig {
enum Field { Guid, AssetCall, StartFrame, PreviousActionName, EndFrame, Flag, OverwriteHash, Overwrite };
}
namespace prop {
enum Field { Name, IsGlobal, TriggerStart, TriggerEnd };
}
namespace ptrig {
enum Field { Guid, AssetCall, Condition, Flag, OverwriteHash, Overwrite };
}
namespace awtrig {
enum Field { Guid, AssetCall, Flag, OverwriteHash, Overwrite };
}

struct Schemas {
  Schemas() = default;
  Schemas(const Schemas&) = delete;
  Schemas& operator=(const Schemas&) = delete;
  Schema file, pdef, name, curve, point, random, arrange, arrange_entry, pset, param, cond, user, call, ctn,
      grid_value, grid_cell, slot, action, atrig, prop, ptrig, awtrig;
};
const Schemas& schemas();

// The parameter definition a parameter is an instance of, or nullptr.
const Node* param_def(const Node& param);
// The definition list parameters in `owner` draw from.
const NodeList* param_defs_for(const Node& param);

// Action trigger flags whose start frame field holds the name of the action
// that must have played before, rather than a frame number.
constexpr bool trigger_names_previous_action(s64 flag) {
  return (flag & 0x1C) == 0x10;
}

// Calls are containers when bit 0 of their flags is set.
constexpr bool call_is_container(s64 flag) {
  return (flag & 1) != 0;
}

}  // namespace xl::xlink
