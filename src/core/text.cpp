#include "core/text.hpp"

#include <array>
#include <unordered_map>

#include "core/bytes.hpp"

namespace xl {

    namespace {

        constexpr u8 FirstTrail = 0x40, LastTrail = 0xFC;
        constexpr std::size_t Leads = 60, Trails = LastTrail - FirstTrail + 1;

        constexpr std::array<u16, Leads * Trails> DoubleByte = {
#include "core/shift_jis_table.inc"
        };

        // Undecodable bytes travel through UTF-8 as lone low surrogates, the same
        // escape Python's "surrogateescape" uses.
        constexpr u32 EscapeBase = 0xDC00;

        int lead_slot(u8 b) {
            if (b >= 0x81 && b <= 0x9F)
                return b - 0x81;
            if (b >= 0xE0 && b <= 0xFC)
                return b - 0xE0 + 0x1F;
            return -1;
        }

        u8 slot_lead(std::size_t slot) { return static_cast<u8>(slot < 0x1F ? 0x81 + slot : 0xE0 + (slot - 0x1F)); }

        void append_utf8(std::string& out, u32 cp) {
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | cp >> 6));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | cp >> 12));
                out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | cp >> 18));
                out.push_back(static_cast<char>(0x80 | (cp >> 12 & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        // Decodes one code point, or returns false on a malformed sequence. Lone
        // surrogates are accepted because the byte escape relies on them.
        bool next_code_point(std::string_view s, std::size_t& i, u32& cp) {
            const u8 b = static_cast<u8>(s[i]);
            int extra;
            if (b < 0x80) {
                cp = b;
                extra = 0;
            } else if ((b & 0xE0) == 0xC0) {
                cp = b & 0x1F;
                extra = 1;
            } else if ((b & 0xF0) == 0xE0) {
                cp = b & 0x0F;
                extra = 2;
            } else if ((b & 0xF8) == 0xF0) {
                cp = b & 0x07;
                extra = 3;
            } else {
                return false;
            }
            if (i + extra >= s.size() && extra > 0)
                return false;
            for (int k = 1; k <= extra; ++k) {
                const u8 c = static_cast<u8>(s[i + k]);
                if ((c & 0xC0) != 0x80)
                    return false;
                cp = cp << 6 | (c & 0x3F);
            }
            i += extra + 1;
            return true;
        }

        // First Shift-JIS spelling of each code point; CP932 maps a few characters
        // from more than one byte pair (NEC and IBM extensions), and the earliest is
        // the one Windows encodes to.
        const std::unordered_map<u32, u16>& reverse_table() {
            static const auto table = [] {
                std::unordered_map<u32, u16> t;
                for (std::size_t slot = 0; slot < Leads; ++slot)
                    for (std::size_t trail = 0; trail < Trails; ++trail) {
                        const u16 cp = DoubleByte[slot * Trails + trail];
                        if (cp != 0)
                            t.emplace(cp, static_cast<u16>(slot_lead(slot) << 8 | (FirstTrail + trail)));
                    }
                return t;
            }();
            return table;
        }

    } // namespace

    std::string shift_jis_to_utf8(std::string_view s) {
        std::string out;
        out.reserve(s.size() * 3 / 2);
        for (std::size_t i = 0; i < s.size(); ++i) {
            const u8 b = static_cast<u8>(s[i]);
            if (b < 0x80) {
                out.push_back(static_cast<char>(b));
                continue;
            }
            if (b >= 0xA1 && b <= 0xDF) {
                append_utf8(out, 0xFF61 + (b - 0xA1));
                continue;
            }
            const int slot = lead_slot(b);
            if (slot >= 0 && i + 1 < s.size()) {
                const u8 t = static_cast<u8>(s[i + 1]);
                const u16 cp = t >= FirstTrail && t <= LastTrail
                    ? DoubleByte[static_cast<std::size_t>(slot) * Trails + (t - FirstTrail)]
                    : 0;
                if (cp != 0) {
                    append_utf8(out, cp);
                    ++i;
                    continue;
                }
            }
            append_utf8(out, EscapeBase + b);
        }
        return out;
    }

    std::string utf8_to_shift_jis(std::string_view s) {
        const auto& rev = reverse_table();
        std::string out;
        out.reserve(s.size());
        std::size_t i = 0;
        while (i < s.size()) {
            u32 cp;
            const std::size_t start = i;
            if (!next_code_point(s, i, cp)) {
                out.push_back(s[start]);
                i = start + 1;
                continue;
            }
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp >= 0xFF61 && cp <= 0xFF9F) {
                out.push_back(static_cast<char>(0xA1 + (cp - 0xFF61)));
            } else if (cp >= EscapeBase + 0x80 && cp <= EscapeBase + 0xFF) {
                out.push_back(static_cast<char>(cp - EscapeBase));
            } else if (const auto it = rev.find(cp); it != rev.end()) {
                out.push_back(static_cast<char>(it->second >> 8));
                out.push_back(static_cast<char>(it->second & 0xFF));
            } else {
                out.push_back('?');
            }
        }
        return out;
    }

    std::string to_utf8(std::string_view raw, Encoding e) {
        return e == Encoding::ShiftJis ? shift_jis_to_utf8(raw) : std::string(raw);
    }

    std::string from_utf8(std::string_view utf8, Encoding e) {
        return e == Encoding::ShiftJis ? utf8_to_shift_jis(utf8) : std::string(utf8);
    }

    bool is_valid_utf8(std::string_view s) {
        std::size_t i = 0;
        while (i < s.size()) {
            u32 cp;
            if (!next_code_point(s, i, cp))
                return false;
        }
        return true;
    }

} // namespace xl
