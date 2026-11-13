#pragma once

#include "model/node.hpp"

// EffectLink ("eflk") and SoundLink ("SLNK"), the effect and sound link
// databases that came before XLink: per user, call tables naming effects or
// sound assets, actions and their triggers, all of them fixed-size records.
// 3DS versions move assets and action triggers into pools shared by users.
namespace xl::eslink {

    enum class Family { EffectLink, SoundLink };

    // How each 4-byte slot of a record is read.
    enum class SlotKind {
        String, // offset into the user's string table
        Runtime, // filled in by the game at load time; kept but not shown
        Int,
        Float,
        Hex,
        Range, // two s16 halves: first and last index into a user list
        Pair, // two s16 halves: a minimum and a maximum
        CallRef, // index of one of the user's call tables
        // Bytes and halves the game reads one by one; each keeps its own width, so
        // the slot is laid out correctly in either byte order.
        Packed,
    };

    struct Slot {
        SlotKind kind;
        std::string name;
        // Packed parts in order: 'b' a byte, 'H' an unsigned half, 'h' a signed half.
        std::string parts;
    };

    struct RecordLayout {
        Schema schema;
        std::vector<Slot> slot_list;
        // Field index of each slot; the second half of a Range or Pair and the
        // further parts of a Packed slot follow.
        std::vector<int> slot_field;
        int name_field = -1;
        int size() const { return static_cast<int>(slot_list.size()) * 4; }
    };

    extern const std::vector<std::string> UserTypeNames;
    enum UserType { UT_User, UT_Alias, UT_Empty };

    namespace file {
        enum Field { Version, StringPool };
        enum List { SharedAssets, SharedTriggers, Users };
    } // namespace file
    namespace user {
        enum Field {
            Name,
            BaseName,
            Type,
            Kind,
            PooledAssets,
            PooledTriggers,
            ParamsPerAsset,
            // SoundLink: MaxVolumeDistance, UnitDistance, InteriorSize, DistanceFlags.
            Header1,
            Header2,
            Header3,
            Header4,
            StringPool,
            OwnStrings,
            // Bytes between the end of a user's block and the next user.
            Tail,
        };
        enum List { CallTables, Actions, Assets, Triggers, AssetRefs, TriggerRefs };
    } // namespace user
    namespace pool_ref {
        enum Field { Index };
    }
    namespace asset_param {
        enum Field { Value };
    }
    // EffectLink stores parameter words per effect slot of a user; they live on
    // the user's effect record or, for pooled users, on its pool reference, so
    // they move with the effect.
    namespace slot_params {
        enum List { Params };
    }

    struct FamilySchemas {
        FamilySchemas() = default;
        FamilySchemas(const FamilySchemas&) = delete;
        FamilySchemas& operator=(const FamilySchemas&) = delete;
        Family family = Family::EffectLink;
        RecordLayout call, action, asset, trigger;
        Schema pool_ref_asset, pool_ref_trigger, asset_param, user, file;
    };

    // EffectLink versions 9 and 10 share record layouts; SoundLink 69 and 73 do not.
    const FamilySchemas& effect_link();
    const FamilySchemas& sound_link(u32 version);
    // The family schema set a root node belongs to, or nullptr.
    const FamilySchemas* family_of(const Node& root);

    // The list a user's call table asset range (or action trigger range) indexes:
    // the pool reference list for pooled users, the user's own records otherwise.
    NodeList* asset_list_of(const Node& user);
    NodeList* trigger_list_of(const Node& user);

} // namespace xl::eslink
