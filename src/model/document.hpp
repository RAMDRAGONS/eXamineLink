#pragma once

#include <memory>
#include <string>
#include <vector>

#include "codec/layers.hpp"
#include "core/text.hpp"
#include "model/node.hpp"

namespace xl {

enum class Format { XLink, EffectLink, SoundLink };

// A version eXamineLink can write.
struct Version {
  Format format;
  u32 version;
  std::string label;
  // XLink games check each system's version separately (Splatoon 2's effect
  // system wants 30, its sound system 28): 0 effects, 1 sounds, -1 either.
  int system = -1;
};

const std::vector<Version>& versions();
// `system` as in Version; -1 matches either.
const Version* find_version(Format f, u32 version, int system);

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

// A game's settings: the versions its link systems read, with its byte
// order and encoding. Only settings seen in that game's own databases.
struct Preset {
  std::string key;
  std::string name;
  Endian endian;
  Encoding encoding;
  struct Entry {
    Format format;
    u32 version;
    int system;
  };
  std::vector<Entry> entries;
};

const std::vector<Preset>& presets();
// The version `preset` uses for databases of this format and XLink system
// (-1 for formats without one), or 0 when it has none.
u32 preset_version(const Preset& preset, Format f, int system);
// The first preset using `version` for this format and system, or null.
const Preset* preset_for(Format f, u32 version, int system);
const Preset* find_preset(const std::string& key);

std::string format_name(Format f);

struct Document {
  Format format = Format::XLink;
  Endian endian = Endian::Little;
  Encoding encoding = Encoding::Utf8;
  std::unique_ptr<Node> root;
  // Containers the payload came in, reapplied on save.
  std::vector<Layer> layers;

  u32 version() const;
  // The XLink system (0 effects, 1 sounds), or -1 for other formats.
  int system() const;
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

}  // namespace xl
