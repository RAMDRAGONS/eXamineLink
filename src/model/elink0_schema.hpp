#pragma once

#include "model/node.hpp"

// ELink0 ("BLNK"), Nintendo Land's link database: each record ties an event
// of a user to an effect and a sound at once, with fixed-size records and
// self-relative string offsets.
namespace xl::elink0 {

    extern const std::vector<std::string> FollowNames;
    extern const std::vector<std::string> PlayTypeNames;
    extern const std::vector<std::string> VariantNames;
    extern const std::vector<std::string> CameraShakeNames;

    namespace file {
        enum Field { StringPool };
        enum List { UnlistedRecords, Users };
    } // namespace file
    namespace user {
        enum Field { Name };
        enum List { Records };
    } // namespace user
    namespace record {
        enum Field {
            Scale,
            Red,
            Green,
            Blue,
            Alpha,
            Follow,
            FixedAfterEmit,
            PositionX,
            PositionY,
            PositionZ,
            Frame1,
            Frame2,
            Frame3,
            Frame4,
            Frame5,
            Frame6,
            Frame7,
            Frame8,
            Trigger1,
            Trigger2,
            Trigger3,
            Trigger4,
            Trigger5,
            Trigger6,
            Trigger7,
            Trigger8,
            StopFrame,
            PlayType,
            Variant,
            Event,
            EmitterSet,
            Bone,
            Animation1,
            Animation2,
            Animation3,
            Animation4,
            Unused0x84,
            ControllerParams,
            Unused0x86,
            Group,
            CameraShake,
            CameraShakeStrength,
            Sound,
            Volume,
            Pitch,
            SoundFadeFrames,
        };
        constexpr int FrameCount = 8;
        constexpr int AnimationCount = 4;
    } // namespace record

    struct Schemas {
        Schemas() = default;
        Schemas(const Schemas&) = delete;
        Schemas& operator=(const Schemas&) = delete;
        Schema record, user, file;
    };

    const Schemas& schemas();

} // namespace xl::elink0
