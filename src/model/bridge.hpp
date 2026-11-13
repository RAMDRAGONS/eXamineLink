#pragma once

#include <array>
#include <string>
#include <vector>

#include "model/document.hpp"
#include "model/xml_io.hpp"

// Users carried from one database into another of the same link system:
// XLink databases of any version, EffectLink or SoundLink of either version,
// and between EffectLink and XLink ELink databases or SoundLink and XLink
// SLink databases, the systems XLink replaced.
namespace xl {

    struct BridgeOptions {
        // The XLink property each EffectLink effect condition (Condition1 to 3)
        // stands for: the game hands EffectLink three values and plays the effect
        // whose conditions match them; XLink switches on named properties.
        std::array<std::string, 3> conditions;
        // XLink action slot that holds the actions of an EffectLink or SoundLink user.
        std::string action_slot;
    };

    // Why users of `src` cannot go into `doc`, or empty when they can.
    std::string bridge_problem(const Document& doc, const Document& src);
    // Whether the two databases are of different formats, so BridgeOptions apply.
    bool bridge_crosses_formats(const Document& doc, const Document& src);
    // Options read from the databases: condition properties matched by the values
    // both use, the action slot the target uses most.
    BridgeOptions default_bridge_options(const Document& doc, const Document& src);

    // Users `which` of `src` (indices into its user list; all when empty) built
    // for `doc`. What has no place in `doc` is described in `warnings`.
    std::vector<xml::UserImport> bridge_users(const Document& doc, const Document& src, const std::vector<int>& which,
        const BridgeOptions& options, std::vector<std::string>& warnings);

    // Adds the users to `doc` as one undo step, replacing users with the same
    // name or hash when `replace` is set and skipping them otherwise. Returns how
    // many were added or replaced.
    int apply_bridged_users(Document& doc, EditStack& edits, std::vector<xml::UserImport> imports, bool replace,
        std::vector<std::string>& warnings);

} // namespace xl
