#pragma once

#include <string>
#include <string_view>

namespace xl {

// Strings are kept in the file's own encoding so they write back byte for
// byte; this names that encoding for display and text export.
enum class Encoding { Utf8, ShiftJis };

// Shift-JIS as extended by Windows code page 932. Bytes that do not form a
// defined character are carried through as U+DC00 + byte, so a decode followed
// by an encode reproduces the input exactly.
std::string shift_jis_to_utf8(std::string_view s);
std::string utf8_to_shift_jis(std::string_view s);

std::string to_utf8(std::string_view raw, Encoding e);
std::string from_utf8(std::string_view utf8, Encoding e);

// True when every byte sequence is well-formed UTF-8.
bool is_valid_utf8(std::string_view s);

} // namespace xl
