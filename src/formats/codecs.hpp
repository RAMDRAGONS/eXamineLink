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
}  // namespace xlink_modern

// XLink resource version 19 (Splatoon on Wii U).
namespace xlink_v19 {
Document read(Bytes data);
Buffer write(const Document& doc);
}  // namespace xlink_v19

// EffectLink 9 and 10, SoundLink 69 and 73.
namespace dslink_codec {
Document read(Bytes data);
Buffer write(const Document& doc);
}  // namespace dslink_codec

// Shift-JIS when any non-ASCII string fails to decode as UTF-8.
Encoding detect_encoding(std::string_view string_table, Encoding fallback);

}  // namespace xl
