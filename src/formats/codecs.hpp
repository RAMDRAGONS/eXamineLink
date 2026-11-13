#pragma once

#include "model/document.hpp"

namespace xl {

    // XLink resource versions 27 to 36 (Splatoon 2, Splatoon 3 and others). The
    // layout follows the version and system (xlink::System, ELink or SLink).
    namespace xlink_modern {
        Document read(Bytes data);
        Buffer write(const Document& doc);
        // 64-bit layout: ELink2 from 32, SLink2 from 31.
        bool is_wide(u32 version, int system);
        // Whether users list their own local property names.
        bool has_user_locals(u32 version, int system);
        // Whether user headers pair a 16-bit local property count with a count of
        // the global properties the user reads.
        bool has_global_count(u32 version, int system);
        // That count as Nintendo's tools derive it: distinct global properties read
        // through global switch and blend-by containers, grid axes marked global,
        // curves in the user's asset parameters, and its global properties.
        u16 global_property_count(const Node& file, const Node& user);
    } // namespace xlink_modern

    // XLink resource versions 19 (Splatoon) and 21 (Super Mario Maker), which
    // find users by name and store conditions without a type.
    namespace xlink_early {
        Document read(Bytes data);
        Buffer write(const Document& doc);
        bool is_early(u32 version);
        // Version 21 calls carry no guid.
        bool has_call_guid(u32 version);
    } // namespace xlink_early

    // ELink0 (Nintendo Land).
    namespace elink0_codec {
        Document read(Bytes data);
        Buffer write(const Document& doc);
    } // namespace elink0_codec

    // EffectLink 9 and 10, SoundLink 50, 69 and 73.
    namespace eslink_codec {
        Document read(Bytes data);
        Buffer write(const Document& doc);
    } // namespace eslink_codec

    // Shift-JIS when any non-ASCII string fails to decode as UTF-8.
    Encoding detect_encoding(std::string_view string_table, Encoding fallback);

} // namespace xl
