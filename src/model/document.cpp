#include "model/document.hpp"

#include <cctype>
#include <fstream>
#include <map>

#include <QCoreApplication>
#include <QDir>

#include "codec/zstd.hpp"
#include "formats/codecs.hpp"
#include "model/dslink_schema.hpp"
#include "model/xlink_schema.hpp"

namespace xl {

const std::vector<Version>& versions() {
  static const std::vector<Version> v = {
      {Format::XLink, 19, "XLink v19", -1},
      {Format::XLink, 27, "SLink2 v27", 1},
      {Format::XLink, 28, "SLink2 v28", 1},
      {Format::XLink, 29, "ELink2 v29", 0},
      {Format::XLink, 29, "SLink2 v29", 1},
      {Format::XLink, 30, "ELink2 v30", 0},
      {Format::XLink, 31, "ELink2 v31", 0},
      {Format::XLink, 31, "SLink2 v31 (64-bit)", 1},
      {Format::XLink, 33, "SLink2 v33 (64-bit)", 1},
      {Format::XLink, 34, "ELink2 v34 (64-bit)", 0},
      {Format::XLink, 36, "ELink2 v36 (64-bit)", 0},
      {Format::EffectLink, 9, "EffectLink v9"},
      {Format::EffectLink, 10, "EffectLink v10 (shared pools)"},
      {Format::SoundLink, 69, "SoundLink v69"},
      {Format::SoundLink, 73, "SoundLink v73 (shared pools)"},
  };
  return v;
}

const Version* find_version(Format f, u32 version, int system) {
  for (const Version& v : versions())
    if (v.format == f && v.version == version && (v.system < 0 || system < 0 || v.system == system))
      return &v;
  return nullptr;
}

const std::vector<Preset>& presets() {
  using F = Format;
  static const std::vector<Preset> p = {
      {"turbo", "Mario Kart 8 (Wii U)", Endian::Big, Encoding::ShiftJis,
       {{F::EffectLink, 9, -1}, {F::SoundLink, 69, -1}}},
      {"alice", "The Legend of Zelda: Tri Force Heroes (3DS)", Endian::Little, Encoding::ShiftJis,
       {{F::SoundLink, 69, -1}}},
      {"takumi", "Animal Crossing: Happy Home Designer (3DS)", Endian::Little, Encoding::ShiftJis,
       {{F::EffectLink, 10, -1}, {F::SoundLink, 73, -1}}},
      {"gambit", "Splatoon (Wii U)", Endian::Big, Encoding::ShiftJis, {{F::XLink, 19, -1}}},
      {"turbo-s-proto", "Mario Kart 8 Deluxe prototype (Switch)", Endian::Little, Encoding::ShiftJis,
       {{F::XLink, 29, 0}, {F::XLink, 27, 1}}},
      {"u-king", "The Legend of Zelda: Breath of the Wild (Wii U)", Endian::Big, Encoding::Utf8,
       {{F::XLink, 30, 0}, {F::XLink, 28, 1}}},
      {"u-king-s", "The Legend of Zelda: Breath of the Wild, Game Builder Garage (Switch)", Endian::Little, Encoding::Utf8,
       {{F::XLink, 30, 0}, {F::XLink, 28, 1}}},
      {"blitz", "Splatoon 2, ARMS, Mario Kart 8 Deluxe (Switch)", Endian::Little, Encoding::ShiftJis,
       {{F::XLink, 30, 0}, {F::XLink, 28, 1}}},
      {"park", "Animal Crossing: New Horizons (Switch)", Endian::Little, Encoding::Utf8,
       {{F::XLink, 31, 0}, {F::XLink, 29, 1}}},
      {"thunder", "Splatoon 3 (Switch)", Endian::Little, Encoding::Utf8, {{F::XLink, 34, 0}, {F::XLink, 31, 1}}},
      {"ex-king", "The Legend of Zelda: Tears of the Kingdom (Switch)", Endian::Little, Encoding::Utf8,
       {{F::XLink, 36, 0}, {F::XLink, 33, 1}}},
  };
  return p;
}

u32 preset_version(const Preset& preset, Format f, int system) {
  for (const Preset::Entry& e : preset.entries)
    if (e.format == f && (e.system < 0 || system < 0 || e.system == system))
      return e.version;
  return 0;
}

const Preset* preset_for(Format f, u32 version, int system) {
  for (const Preset& p : presets())
    if (preset_version(p, f, system) == version)
      return &p;
  return nullptr;
}

const Preset* find_preset(const std::string& key) {
  for (const Preset& p : presets())
    if (p.key == key)
      return &p;
  return nullptr;
}

std::string format_name(Format f) {
  switch (f) {
    case Format::XLink:
      return "XLink";
    case Format::EffectLink:
      return "EffectLink";
    case Format::SoundLink:
      return "SoundLink";
  }
  return {};
}

u32 Document::version() const {
  // Version is the first field of every root schema.
  return root ? static_cast<u32>(root->i(0)) : 0;
}

namespace {

// Grid containers and the action flag word exist only in the 64-bit layout.
bool narrow_xlink(const Document& doc) {
  return doc.format == Format::XLink && !xlink_modern::is_wide(doc.version(), doc.system());
}

}  // namespace

bool stores_field(const Document& doc, const Schema& s, int field) {
  using namespace xlink;
  // SoundLink 73 users have no distance settings.
  if (doc.format == Format::SoundLink && doc.version() == 73 && &s == &dslink::sound_link(73).user)
    return field < dslink::user::Header1 || field > dslink::user::Header4;
  if (!narrow_xlink(doc))
    return true;
  if (&s == &schemas().ctn)
    return field != ctn::WatchPropertyName2 && field != ctn::LocalPropertyNameIdx2 && field != ctn::GridFlags1 &&
           field != ctn::GridFlags2 && field != ctn::BlendByProperty;
  if (&s == &schemas().action)
    return field != action::Flag;
  return true;
}

bool users_reorderable(const Document& doc) {
  return doc.format != Format::XLink || doc.version() == 19;
}

bool stores_list(const Document& doc, const Schema& s, int list) {
  if (doc.format == Format::XLink && &s == &xlink::schemas().user && list == xlink::user::LocalProperties)
    return doc.version() != 19 && xlink_modern::has_user_locals(doc.version(), doc.system());
  return !narrow_xlink(doc) || &s != &xlink::schemas().ctn;
}

bool stores_enum_value(const Document& doc, const Schema& s, int field, s64 value) {
  if (doc.format != Format::XLink || &s != &xlink::schemas().ctn || field != xlink::ctn::Type)
    return true;
  if (value == xlink::CT_Jump)
    return doc.version() != 19 && xlink_modern::has_global_count(doc.version(), doc.system());
  return !narrow_xlink(doc) || value != xlink::CT_Grid;
}

int Document::system() const {
  return format == Format::XLink && root ? static_cast<int>(root->i(xlink::file::System)) : -1;
}

Document Document::clone() const {
  Document d;
  d.format = format;
  d.endian = endian;
  d.encoding = encoding;
  d.root = root ? root->clone() : nullptr;
  d.layers = layers;
  return d;
}

Encoding detect_encoding(std::string_view table, Encoding fallback) {
  bool any_high = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= table.size(); ++i) {
    if (i < table.size() && table[i] != 0)
      continue;
    const std::string_view s = table.substr(start, i - start);
    for (const char c : s)
      if (static_cast<u8>(c) >= 0x80) {
        any_high = true;
        if (!is_valid_utf8(s))
          return Encoding::ShiftJis;
        break;
      }
    start = i + 1;
  }
  return any_high ? Encoding::Utf8 : fallback;
}

bool is_database(Bytes payload) {
  const Reader r(payload, Endian::Big);
  return r.magic_at(0, "XLNK") || r.magic_at(0, "eflk") || r.magic_at(0, "SLNK");
}

Document read_database(Bytes payload) {
  const Reader r(payload, Endian::Big);
  if (r.magic_at(0, "XLNK")) {
    r.require(0, 0xC, "XLink header");
    Reader le(payload, Endian::Little);
    u32 version = le.u32_at(8);
    if (version > 0xFFFF)
      version = r.u32_at(8);
    if (version == 19)
      return xlink_v19::read(payload);
    if (version >= 27 && version <= 36)
      return xlink_modern::read(payload);
    throw Error("XLink resource version " + std::to_string(version) + " is not supported");
  }
  if (r.magic_at(0, "eflk") || r.magic_at(0, "SLNK"))
    return dslink_codec::read(payload);
  if (r.magic_at(0, "BLNK"))
    throw Error("BLNK EffectLink databases are not supported");
  throw Error("not a link database (XLNK, eflk or SLNK)");
}

Buffer write_database(const Document& doc) {
  const u32 v = doc.version();
  switch (doc.format) {
    case Format::XLink:
      if (v == 19)
        return xlink_v19::write(doc);
      if (v >= 27 && v <= 36)
        return xlink_modern::write(doc);
      throw Error("XLink resource version " + std::to_string(v) + " cannot be written");
    case Format::EffectLink:
    case Format::SoundLink:
      return dslink_codec::write(doc);
  }
  throw Error("unknown format");
}

Buffer read_all(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw Error("cannot open " + path);
  return Buffer(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void write_all(const std::string& path, Bytes data) {
  // Written beside the target first so a failure cannot leave a truncated file.
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f)
      throw Error("cannot write " + path);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f)
      throw Error("failed writing " + path);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    throw Error("cannot replace " + path);
  }
}

namespace {

bool ends_with_ci(const std::string& s, const std::string& suffix) {
  if (s.size() < suffix.size())
    return false;
  for (std::size_t i = 0; i < suffix.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != suffix[i])
      return false;
  return true;
}

std::string base_name(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

std::string default_member_name(const std::string& path, const Document& doc) {
  std::string base = base_name(path);
  for (const char* ext : {".szs", ".zs", ".sarc"})
    if (ends_with_ci(base, ext))
      base.resize(base.size() - std::string(ext).size());
  if (base.find('.') != std::string::npos)
    return base;
  if (doc.format == Format::XLink)
    return base + (doc.root->i(xlink::file::System) == static_cast<s64>(xlink::System::ELink) ? ".belnk" : ".bslnk");
  return base + ".bin";
}

std::vector<Layer> layers_for_path(const std::string& path, const Document& doc) {
  const bool szs = ends_with_ci(path, ".szs"), zs = ends_with_ci(path, ".zs");
  const auto kinds_match = [&](std::initializer_list<LayerKind> want) {
    if (doc.layers.size() != want.size())
      return false;
    std::size_t i = 0;
    for (const LayerKind k : want)
      if (doc.layers[i++].kind != k)
        return false;
    return true;
  };
  if (szs) {
    if (kinds_match({LayerKind::Yaz, LayerKind::Sarc}))
      return doc.layers;
    Layer y{LayerKind::Yaz};
    y.yaz_alignment = 0x100;
    Layer a{LayerKind::Sarc};
    a.archive.endian = doc.endian;
    a.archive.files.push_back({default_member_name(path, doc), {}});
    a.sarc_index = 0;
    return {y, a};
  }
  if (zs) {
    if (kinds_match({LayerKind::Zstd}))
      return doc.layers;
    return {Layer{LayerKind::Zstd}};
  }
  return {};
}

std::string dictionary_folder() {
  const QString dir = QCoreApplication::applicationDirPath() + "/ZsDic";
  QDir().mkpath(dir);
  return QDir::toNativeSeparators(dir).toStdString();
}

// Read once, on first need, so opening files without dictionaries never
// touches the folder.
DictionaryLookup folder_dictionaries() {
  struct Found {
    bool searched = false;
    std::multimap<u32, std::shared_ptr<const Buffer>> by_id;
  };
  auto found = std::make_shared<Found>();
  return [found](u32 id) {
    const std::string folder = dictionary_folder();
    if (!found->searched) {
      found->searched = true;
      const auto add = [&](Buffer&& d) {
        if (const u32 key = zstd::dictionary_id(d))
          found->by_id.emplace(key, std::make_shared<const Buffer>(std::move(d)));
      };
      const QDir dir(QString::fromStdString(folder));
      for (const QString& f : dir.entryList(QDir::Files, QDir::Name)) {
        // Anything else in the folder is not a dictionary and is passed over.
        try {
          Buffer data = read_all(dir.filePath(f).toStdString());
          if (zstd::probe(data))
            data = zstd::decompress(data);
          if (sarc::probe(data)) {
            for (sarc::File& m : sarc::read(data).files)
              add(std::move(m.data));
          } else {
            add(std::move(data));
          }
        } catch (const Error&) {
        }
      }
    }
    std::vector<std::shared_ptr<const Buffer>> out;
    for (auto [it, end] = found->by_id.equal_range(id); it != end; ++it)
      out.push_back(it->second);
    if (out.empty())
      throw Error("compressed with Zstandard dictionary " + std::to_string(id) +
                  "; put the game's ZsDic.pack.zs or .zsdic files in " + folder);
    return out;
  };
}

Document load_file(const std::string& path) {
  const Buffer data = read_all(path);
  Unwrapped u = unwrap(data, [](Bytes b) { return is_database(b); }, folder_dictionaries());
  Document doc = read_database(u.payload);
  doc.layers = std::move(u.layers);
  return doc;
}

void save_file(const std::string& path, const Document& doc) {
  write_all(path, wrap(doc.layers, write_database(doc)));
}

}  // namespace xl
