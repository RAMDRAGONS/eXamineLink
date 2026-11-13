#pragma once

#include <memory>
#include <string>
#include <vector>

#include "codec/layers.hpp"
#include "core/text.hpp"
#include "model/node.hpp"

namespace xl {

    // The link systems. ELink0 links effects and sounds in one record and has no
    // version; ELink and SLink are the separate effect and sound formats XLink
    // replaced; ELink2 and SLink2 are XLink's effect and sound databases, which
    // share one layout and codec but carry their own versions.
    enum class Format { ELink0, ELink, SLink, ELink2, SLink2 };

    bool is_xlink(Format f);
    // ELink0 counts as an effect format: its databases sit with the effects.
    bool is_effect(Format f);
    // The system index XLink files and layouts use: 0 effects, 1 sounds.
    int xlink_system(Format f);
    Format xlink_format(int system);
    // The format that replaced (or was replaced by) `f`: ELink and ELink2,
    // SLink and SLink2. ELink0 has none and is its own.
    Format counterpart(Format f);

    // A version eXamineLink can write.
    struct Version {
        Format format;
        u32 version;
        std::string label;
    };

    const std::vector<Version>& versions();
    const Version* find_version(Format f, u32 version);

    // What to write: a version in a byte order and text encoding. These follow
    // the platform and game, not the version (Breath of the Wild ships ELink2
    // v30 big-endian on Wii U and little-endian on Switch, in UTF-8 where
    // Splatoon 2 uses Shift-JIS), so every version can be written with either.
    struct Target {
        Format format;
        u32 version;
        Endian endian;
        Encoding encoding;
    };

    // The settings of one link system as games ship it: a format and version
    // with a byte order and encoding. Effect and sound systems carry their own
    // versions, so each has its own presets, and games whose systems agree share
    // one. Keys are the games' codenames, unique among effect and among sound
    // presets.
    struct Preset {
        std::string key;
        std::string name;
        Format format;
        u32 version;
        Endian endian;
        Encoding encoding;
    };

    const std::vector<Preset>& presets();
    // The first preset of this format using `version`, or null.
    const Preset* preset_for(Format f, u32 version);
    const Preset* find_preset(const std::string& key, Format f);

    std::string format_name(Format f);
    // "ELink2 v36", or the bare name of a format without versions.
    std::string version_name(Format f, u32 version);

    struct Document {
        Format format = Format::ELink2;
        Endian endian = Endian::Little;
        Encoding encoding = Encoding::Utf8;
        std::unique_ptr<Node> root;
        // Containers the payload came in, reapplied on save.
        std::vector<Layer> layers;

        u32 version() const;
        Document clone() const;
    };

    // Whether the document's version stores a field, list or enum value of a
    // schema. The XLink model holds every version's fields; editors hide those
    // the loaded version would drop on save.
    bool stores_field(const Document& doc, const Schema& s, int field);
    bool stores_list(const Document& doc, const Schema& s, int list);
    bool stores_enum_value(const Document& doc, const Schema& s, int field, s64 value);

    // Whether the order of users is the database's to choose. XLink 27 and later
    // keep users in hash order for the game's binary search; every other version
    // is searched by name from the first user, so order only decides which of two
    // users with the same name is found.
    bool users_reorderable(const Document& doc);

    bool is_database(Bytes payload);
    // Parses an uncompressed database.
    Document read_database(Bytes payload);
    // Serializes the document in the version its root names.
    Buffer write_database(const Document& doc);

    // Containers for saving to `path`: the document's own when the extension
    // still matches them, otherwise Yaz+SARC for .szs, Zstandard for .zs and
    // none for anything else.
    std::vector<Layer> layers_for_path(const std::string& path, const Document& doc);
    // Name a database gets inside a new SARC archive.
    std::string default_member_name(const std::string& path, const Document& doc);

    // Folder beside the executable holding games' Zstandard dictionaries, as
    // packs such as ZsDic.pack.zs or loose .zsdic files; created when missing.
    std::string dictionary_folder();
    DictionaryLookup folder_dictionaries();
    Document load_file(const std::string& path);
    void save_file(const std::string& path, const Document& doc);

    Buffer read_all(const std::string& path);
    void write_all(const std::string& path, Bytes data);

} // namespace xl
