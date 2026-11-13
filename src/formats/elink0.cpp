#include <algorithm>

#include "core/crc32.hpp"
#include "formats/codecs.hpp"
#include "formats/string_pool.hpp"
#include "model/elink0_schema.hpp"

namespace xl::elink0_codec {

    using namespace elink0;

    namespace {

        constexpr u32 HeaderSize = 0x20;
        constexpr u32 UserSize = 0x14;
        constexpr u32 RecordSize = 0xCC;
        constexpr u32 UserFirstAt = 0x04, UserCountAt = 0x08, UserNameEntryAt = 0x0C;

        // Record words as the game reads them; the hashes beside each string are
        // its CRC-32, and every word from 0x94 to 0xB7 and at 0xC8 is filled in
        // at load time.
        constexpr u32 UserHash = 0x00, EventHash = 0x04, EmitterSetHash = 0x08, BoneHash = 0x28;
        constexpr u32 FirstFloat = 0x0C, FollowAt = 0x20, FixedAt = 0x24, PositionAt = 0x2C, FramesAt = 0x38;
        constexpr u32 StopFrameAt = 0x58, PlayTypeAt = 0x5C, VariantAt = 0x60;
        constexpr u32 UserNameAt = 0x64, EventAt = 0x68, EmitterSetAt = 0x6C, BoneAt = 0x70, AnimationsAt = 0x74;
        constexpr u32 FlagsAt = 0x84, GroupAt = 0x88, ShakeAt = 0x8C, ShakeStrengthAt = 0x90;
        constexpr u32 SoundAt = 0xB8, VolumeAt = 0xBC, PitchAt = 0xC0, FadeAt = 0xC4;

        // String fields in the order Nintendo's tool added them to the table;
        // named triggers and the sound follow.
        constexpr std::pair<u32, int> RecordStrings[] = {
            { EventAt, record::Event },
            { EmitterSetAt, record::EmitterSet },
            { BoneAt, record::Bone },
            { AnimationsAt, record::Animation1 },
            { AnimationsAt + 4, record::Animation2 },
            { AnimationsAt + 8, record::Animation3 },
            { AnimationsAt + 12, record::Animation4 },
        };

        std::string string_at(const Reader& r, u64 entry, u32 at) {
            return r.cstr_at(entry + static_cast<u32>(r.s32_at(entry + at)), "ELink0 string");
        }

        void read_record(const Reader& r, u64 at, Node& n) {
            const auto word = [&](u32 off) { return static_cast<s64>(r.u32_at(at + off)); };
            const auto sword = [&](u32 off) { return static_cast<s64>(r.s32_at(at + off)); };
            for (int k = 0; k < 5; ++k)
                n.set_i(record::Scale + k, word(FirstFloat + 4 * k));
            n.set_i(record::Follow, sword(FollowAt));
            n.set_i(record::FixedAfterEmit, word(FixedAt));
            for (int k = 0; k < 3; ++k)
                n.set_i(record::PositionX + k, word(PositionAt + 4 * k));
            // A frame below -1 is the negated offset of a named trigger's string;
            // the frame list then goes on past it.
            for (int k = 0; k < record::FrameCount; ++k) {
                const s32 frame = r.s32_at(at + FramesAt + 4 * k);
                if (frame < -1)
                    n.set_s(record::Trigger1 + k, r.cstr_at(at - static_cast<s64>(frame), "ELink0 trigger name"));
                n.set_i(record::Frame1 + k, frame < -1 ? -1 : frame);
            }
            n.set_i(record::StopFrame, sword(StopFrameAt));
            n.set_i(record::PlayType, sword(PlayTypeAt));
            n.set_i(record::Variant, sword(VariantAt));
            for (const auto& [off, f] : RecordStrings)
                n.set_s(f, string_at(r, at, off));
            n.set_s(record::Sound, string_at(r, at, SoundAt));
            n.set_i(record::Unused0x84, r.u8_at(at + FlagsAt));
            n.set_i(record::ControllerParams, r.u8_at(at + FlagsAt + 1));
            n.set_i(record::Unused0x86, r.u16_at(at + FlagsAt + 2));
            n.set_i(record::Group, sword(GroupAt));
            n.set_i(record::CameraShake, sword(ShakeAt));
            n.set_i(record::CameraShakeStrength, word(ShakeStrengthAt));
            n.set_i(record::Volume, word(VolumeAt));
            n.set_i(record::Pitch, word(PitchAt));
            n.set_i(record::SoundFadeFrames, sword(FadeAt));
        }

        // The size field is the one header word whose value is known, so it
        // decides the byte order.
        Endian detect_endian(Bytes data) {
            for (const Endian e : { Endian::Big, Endian::Little }) {
                const Reader r(data, e);
                r.require(0, HeaderSize, "ELink0 header");
                if (r.u32_at(4) == data.size())
                    return e;
            }
            throw Error("ELink0 header size does not match the data in either byte order");
        }

    } // namespace

    Document read(Bytes data) {
        const Reader r(data, detect_endian(data));
        const u32 user_off = r.u32_at(0x08), user_count = r.u32_at(0x0C);
        const u32 rec_off = r.u32_at(0x10), rec_count = r.u32_at(0x14);
        const u32 str_off = r.u32_at(0x18);
        r.require(user_off, u64 { user_count } * UserSize, "ELink0 user table");
        r.require(rec_off, u64 { rec_count } * RecordSize, "ELink0 record table");
        r.require(str_off, 0, "ELink0 string table");

        Document doc;
        doc.format = Format::ELink0;
        doc.endian = r.endian();
        doc.root = std::make_unique<Node>(&schemas().file);
        Node& file = *doc.root;
        const Bytes pool = r.slice(str_off, data.size() - str_off, "ELink0 string table");
        file.set_s(file::StringPool, std::string(pool.begin(), pool.end()));

        // Each listed user owns a run of records. Records no user lists, which
        // Nintendo's tool writes for an empty user name and sorts first, are kept
        // apart.
        std::vector<int> owner(rec_count, -1);
        for (u32 u = 0; u < user_count; ++u) {
            const u64 at = user_off + u64 { u } * UserSize;
            const s32 start = r.s32_at(at + UserFirstAt), count = r.s32_at(at + UserCountAt);
            if (start < 0 || count < 0 || static_cast<u64>(start) + count > rec_count)
                throw Error("ELink0 user " + std::to_string(u) + " lists records outside the record table");
            for (s32 k = start; k < start + count; ++k) {
                if (owner[k] >= 0)
                    throw Error("ELink0 users " + std::to_string(owner[k]) + " and " + std::to_string(u)
                        + " list the same record");
                owner[k] = static_cast<int>(u);
            }
        }
        const auto add_record
            = [&](NodeList& list, u32 k) { read_record(r, rec_off + u64 { k } * RecordSize, list.append_new()); };
        for (u32 k = 0; k < rec_count; ++k)
            if (owner[k] < 0)
                add_record(file.list(file::UnlistedRecords), k);
        for (u32 u = 0; u < user_count; ++u) {
            const u64 at = user_off + u64 { u } * UserSize;
            Node& user = file.list(file::Users).append_new();
            user.set_s(user::Name, string_at(r, at, UserNameEntryAt));
            const u32 start = r.u32_at(at + UserFirstAt);
            for (u32 k = start; k < start + r.u32_at(at + UserCountAt); ++k)
                add_record(user.list(user::Records), k);
        }
        doc.encoding = detect_encoding(file.s(file::StringPool), Encoding::ShiftJis);
        return doc;
    }

    Buffer write(const Document& doc) {
        const Node& file = *doc.root;
        StringPool pool(file.s(file::StringPool));

        // Records in file order with the user name they are written under. A user
        // without a name cannot be listed, so its records join the unlisted ones
        // ahead of every listed user, where they read back as unlisted.
        struct Run {
            const Node* user;
            const NodeList* records;
        };
        std::vector<Run> runs { { nullptr, &file.list(file::UnlistedRecords) } };
        for (const auto& u : file.list(file::Users))
            if (u->s(user::Name).empty())
                runs.push_back({ nullptr, &u->list(user::Records) });
        for (const auto& u : file.list(file::Users))
            if (!u->s(user::Name).empty())
                runs.push_back({ u.get(), &u->list(user::Records) });

        u32 rec_count = 0, user_count = 0;
        for (const Run& run : runs) {
            rec_count += static_cast<u32>(run.records->size());
            user_count += run.user != nullptr;
        }
        const u32 rec_off = HeaderSize + user_count * UserSize;
        const u32 str_off = rec_off + rec_count * RecordSize;

        Writer w(doc.endian);
        w.zeros(str_off);
        const auto put = [&](std::size_t at, u32 v) { w.patch_u32(at, v); };
        const auto string_ref = [&](std::size_t entry, u32 at, const std::string& s) {
            put(entry + at, str_off + pool.offset(s) - static_cast<u32>(entry));
        };

        u32 next = 0;
        for (const Run& run : runs) {
            const std::string owner = run.user ? run.user->s(user::Name) : std::string();
            for (const auto& rec : *run.records) {
                const Node& n = *rec;
                const std::size_t at = rec_off + std::size_t { next++ } * RecordSize;
                put(at + UserHash, crc32(owner));
                put(at + EventHash, crc32(n.s(record::Event)));
                put(at + EmitterSetHash, crc32(n.s(record::EmitterSet)));
                put(at + BoneHash, crc32(n.s(record::Bone)));
                for (int k = 0; k < 5; ++k)
                    put(at + FirstFloat + 4 * k, static_cast<u32>(n.i(record::Scale + k)));
                put(at + FollowAt, static_cast<u32>(n.i(record::Follow)));
                put(at + FixedAt, static_cast<u32>(n.i(record::FixedAfterEmit)));
                for (int k = 0; k < 3; ++k)
                    put(at + PositionAt + 4 * k, static_cast<u32>(n.i(record::PositionX + k)));
                for (int k = 0; k < record::FrameCount; ++k)
                    if (n.s(record::Trigger1 + k).empty())
                        put(at + FramesAt + 4 * k, static_cast<u32>(n.i(record::Frame1 + k)));
                put(at + StopFrameAt, static_cast<u32>(n.i(record::StopFrame)));
                put(at + PlayTypeAt, static_cast<u32>(n.i(record::PlayType)));
                put(at + VariantAt, static_cast<u32>(n.i(record::Variant)));
                string_ref(at, UserNameAt, owner);
                for (const auto& [off, f] : RecordStrings)
                    string_ref(at, off, n.s(f));
                for (int k = 0; k < record::FrameCount; ++k)
                    if (const std::string& name = n.s(record::Trigger1 + k); !name.empty())
                        put(at + FramesAt + 4 * k,
                            static_cast<u32>(-static_cast<s64>(str_off + pool.offset(name) - at)));
                w.buffer()[at + FlagsAt] = static_cast<u8>(n.i(record::Unused0x84));
                w.buffer()[at + FlagsAt + 1] = static_cast<u8>(n.i(record::ControllerParams));
                w.patch_u16(at + FlagsAt + 2, static_cast<u16>(n.i(record::Unused0x86)));
                put(at + GroupAt, static_cast<u32>(n.i(record::Group)));
                put(at + ShakeAt, static_cast<u32>(n.i(record::CameraShake)));
                put(at + ShakeStrengthAt, static_cast<u32>(n.i(record::CameraShakeStrength)));
                string_ref(at, SoundAt, n.s(record::Sound));
                put(at + VolumeAt, static_cast<u32>(n.i(record::Volume)));
                put(at + PitchAt, static_cast<u32>(n.i(record::Pitch)));
                put(at + FadeAt, static_cast<u32>(n.i(record::SoundFadeFrames)));
            }
        }

        // User names go in after every record string, as the tool's tables have them.
        u32 first = 0, listed = 0;
        for (const Run& run : runs) {
            const u32 count = static_cast<u32>(run.records->size());
            if (run.user) {
                const std::string& name = run.user->s(user::Name);
                const std::size_t at = HeaderSize + std::size_t { listed++ } * UserSize;
                put(at, crc32(name));
                put(at + UserFirstAt, first);
                put(at + UserCountAt, count);
                string_ref(at, UserNameEntryAt, name);
            }
            first += count;
        }

        w.bytes(pool.bytes());
        std::copy_n("BLNK", 4, w.buffer().begin());
        put(0x04, static_cast<u32>(w.pos()));
        put(0x08, HeaderSize);
        put(0x0C, user_count);
        put(0x10, rec_off);
        put(0x14, rec_count);
        put(0x18, str_off);
        return std::move(w.buffer());
    }

} // namespace xl::elink0_codec
