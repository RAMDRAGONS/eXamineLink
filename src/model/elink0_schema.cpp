#include "model/elink0_schema.hpp"

namespace xl::elink0 {

const std::vector<std::string> FollowNames = {"All", "PositionRotation", "Position", "None", "PositionScale"};
const std::vector<std::string> PlayTypeNames = {"Once", "EachTrigger", "Hold"};
// The game's own names, which it prints when no record fits; 0 is empty.
const std::vector<std::string> VariantNames = {"Any", "TV1", "TV2",  "TV3",   "TV4",    "DRC",  "Op0",  "Op1", "Op2",
                                               "Op3", "Op4", "Sand", "Water", "Ground", "Kusa", "Rock", "Mud", "Snow"};
const std::vector<std::string> CameraShakeNames = {"None", "Short", "Medium", "Long"};

namespace {

void
fill(Schemas &s)
{
   s.record.name = "Record";
   s.record.fields = {
      field("Scale", FieldType::F32, "Uniform scale of the emitter set"),
      field("Red", FieldType::F32),
      field("Green", FieldType::F32),
      field("Blue", FieldType::F32),
      field("Alpha", FieldType::F32),
      enum_field("Follow", &FollowNames,
                 "Which parts of the bone or model transform the effect takes: All, rotation and position without "
                 "scale, position only (also any value above 4), none, or position and scale without rotation"),
      field("FixedAfterEmit", FieldType::Bool,
            "Place the effect when it is emitted and stop following the transform afterwards"),
      field("PositionX", FieldType::F32, "Offset in the followed transform's space"),
      field("PositionY", FieldType::F32),
      field("PositionZ", FieldType::F32),
      field("Frame1", FieldType::S32,
            "Animation frames that emit the record; the list ends at the first -1 without a trigger name"),
      field("Frame2", FieldType::S32),
      field("Frame3", FieldType::S32),
      field("Frame4", FieldType::S32),
      field("Frame5", FieldType::S32),
      field("Frame6", FieldType::S32),
      field("Frame7", FieldType::S32),
      field("Frame8", FieldType::S32),
      field("Trigger1", FieldType::String,
            "Named animation trigger that emits the record in place of the frame of the same number"),
      field("Trigger2", FieldType::String),
      field("Trigger3", FieldType::String),
      field("Trigger4", FieldType::String),
      field("Trigger5", FieldType::String),
      field("Trigger6", FieldType::String),
      field("Trigger7", FieldType::String),
      field("Trigger8", FieldType::String),
      field("StopFrame", FieldType::S32, "Animation frame that stops the effect; -1 for none"),
      enum_field("PlayType", &PlayTypeNames,
                 "Once: emit on the first trigger only; EachTrigger: emit on every trigger; Hold: keep the "
                 "effect alive and loop the sound"),
      enum_field("Variant", &VariantNames,
                 "Any plays for every owner. TV1 to Op4 play only for an owner of that variant, ahead of records "
                 "with Any. Sand to Snow play only on that ground material, Ground meaning any material no other "
                 "record of the same event names"),
      field("Event", FieldType::String, "Name the game emits the record by"),
      field("EmitterSet", FieldType::String),
      field("Bone", FieldType::String, "Bone the effect follows; empty for the model's root"),
      field("Animation1", FieldType::String,
            "Animations whose frames Frame1 to Frame8 and StopFrame count; empty for none"),
      field("Animation2", FieldType::String),
      field("Animation3", FieldType::String),
      field("Animation4", FieldType::String),
      field("Unused0x84", FieldType::Hex8, "Never read by the game"),
      field("ControllerParams", FieldType::Bool,
            "Drive the emitter set with the eight parameters its controller holds"),
      field("Unused0x86", FieldType::Hex16, "Never read by the game"),
      field("Group", FieldType::S32, "Effect group the emitter set is created in; -1 keeps the controller's"),
      enum_field("CameraShake", &CameraShakeNames,
                 "Shakes every camera when the record emits, for 20 (Short), 30 (Medium) or 60 frames (Long and any "
                 "higher value)"),
      field("CameraShakeStrength", FieldType::F32, "How far the cameras shake; 0 or less means 1"),
      field("Sound", FieldType::String, "Sound started with the effect; empty for none"),
      field("Volume", FieldType::F32),
      field("Pitch", FieldType::F32),
      field("SoundFadeFrames", FieldType::S32,
            "Frames the sound fades out over when the effect stops; -1 leaves it playing"),
   };
   s.record.label = [](const Node &n) {
      std::string l = "#" + std::to_string(n.row()) + " " + n.s(record::Event);
      const std::string &eset = n.s(record::EmitterSet);
      const std::string &sound = n.s(record::Sound);
      if (!eset.empty() || !sound.empty())
         l += "  -> " + (eset.empty() ? sound : sound.empty() ? eset : eset + ", " + sound);
      return l;
   };

   s.user.name = "User";
   s.user.fields = {field("Name", FieldType::String, "Name the game finds the user by")};
   s.user.lists = {{"Records", &s.record}};
   s.user.label = [](const Node &n) { return n.s(user::Name); };

   s.file.name = "ELink0";
   s.file.fields = {hidden_field("StringPool", FieldType::String)};
   s.file.lists = {
      {"UnlistedRecords", &s.record, false, "Records of no listed user, which the game loads but never plays"},
      {"Users", &s.user}};
   s.file.label = [](const Node &) { return std::string("ELink0"); };
}

} // namespace

const Schemas &
schemas()
{
   static Schemas s;
   static const bool filled = (fill(s), true);
   (void)filled;
   return s;
}

} // namespace xl::elink0
