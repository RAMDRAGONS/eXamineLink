#include "model/document.hpp"

#include <cctype>
#include <fstream>
#include <map>

#include <QCoreApplication>
#include <QDir>

#include "codec/zstd.hpp"
#include "formats/codecs.hpp"
#include "model/eslink_schema.hpp"
#include "model/xlink_schema.hpp"

namespace xl {

    bool is_xlink(Format f) { return f == Format::ELink2 || f == Format::SLink2; }

    bool is_effect(Format f) { return f == Format::ELink0 || f == Format::ELink || f == Format::ELink2; }

    int xlink_system(Format f) { return static_cast<int>(is_effect(f) ? xlink::System::ELink : xlink::System::SLink); }

    Format xlink_format(int system) {
        return system == static_cast<int>(xlink::System::SLink) ? Format::SLink2 : Format::ELink2;
    }

    Format counterpart(Format f) {
        switch (f) {
        case Format::ELink:
            return Format::ELink2;
        case Format::SLink:
            return Format::SLink2;
        case Format::ELink2:
            return Format::ELink;
        case Format::SLink2:
            return Format::SLink;
        case Format::ELink0:
            break;
        }
        return f;
    }

    const std::vector<Version>& versions() {
        using F = Format;
        static const std::vector<Version> v = {
            { F::ELink0, 0, "ELink0" },
            { F::ELink, 9, "ELink v9" },
            { F::ELink, 10, "ELink v10 (shared pools)" },
            { F::SLink, 50, "SLink v50" },
            { F::SLink, 69, "SLink v69" },
            { F::SLink, 73, "SLink v73 (shared pools)" },
            { F::ELink2, 19, "ELink2 v19" },
            { F::ELink2, 21, "ELink2 v21" },
            { F::SLink2, 19, "SLink2 v19" },
            { F::SLink2, 21, "SLink2 v21" },
            { F::SLink2, 27, "SLink2 v27" },
            { F::SLink2, 28, "SLink2 v28" },
            { F::ELink2, 29, "ELink2 v29" },
            { F::SLink2, 29, "SLink2 v29" },
            { F::ELink2, 30, "ELink2 v30" },
            { F::ELink2, 31, "ELink2 v31" },
            { F::SLink2, 31, "SLink2 v31 (64-bit)" },
            { F::SLink2, 33, "SLink2 v33 (64-bit)" },
            { F::ELink2, 34, "ELink2 v34 (64-bit)" },
            { F::SLink2, 34, "SLink2 v34 (64-bit)" },
            { F::ELink2, 36, "ELink2 v36 (64-bit)" },
        };
        return v;
    }

    const Version* find_version(Format f, u32 version) {
        for (const Version& v : versions())
            if (v.format == f && v.version == version)
                return &v;
        return nullptr;
    }

    const std::vector<Preset>& presets() {
        using F = Format;
        constexpr Endian Big = Endian::Big, Little = Endian::Little;
        constexpr Encoding Sjis = Encoding::ShiftJis, Utf8 = Encoding::Utf8;
        static const std::vector<Preset> p = {
            // Effects.
            { "lunch", "Nintendo Land", F::ELink0, 0, Big, Sjis },
            { "turbo", "Mario Kart 8", F::ELink, 9, Big, Sjis },
            { "takumi", "Animal Crossing: Happy Home Designer", F::ELink, 10, Little, Sjis },
            { "gambit", "Splatoon", F::ELink2, 19, Big, Sjis },
            { "block", "Super Mario Maker", F::ELink2, 21, Big, Sjis },
            { "turbo-s-proto", "Mario Kart 8 Deluxe prototype", F::ELink2, 29, Little, Sjis },
            { "u-king", "The Legend of Zelda: Breath of the Wild (Wii U)", F::ELink2, 30, Big, Utf8 },
            { "u-king-s", "The Legend of Zelda: Breath of the Wild (Switch), Game Builder Garage", F::ELink2, 30,
                Little, Utf8 },
            { "blitz", "Splatoon 2, ARMS, Mario Kart 8 Deluxe", F::ELink2, 30, Little, Sjis },
            { "park", "Animal Crossing: New Horizons", F::ELink2, 31, Little, Utf8 },
            { "thunder", "Splatoon 3", F::ELink2, 34, Little, Utf8 },
            { "ex-king",
                "The Legend of Zelda: Tears of the Kingdom, Splatoon Raiders, "
                "Nintendo Switch Sports Resort, Mario Kart World, Donkey Kong Bananza",
                F::ELink2, 36, Little, Utf8 },
            // Sounds.
            { "lounge", "Animal Crossing Plaza", F::SLink, 50, Big, Sjis },
            { "turbo", "Mario Kart 8", F::SLink, 69, Big, Sjis },
            { "alice", "The Legend of Zelda: Tri Force Heroes", F::SLink, 69, Little, Sjis },
            { "takumi", "Animal Crossing: Happy Home Designer", F::SLink, 73, Little, Sjis },
            { "gambit", "Splatoon", F::SLink2, 19, Big, Sjis },
            { "block", "Super Mario Maker", F::SLink2, 21, Big, Sjis },
            { "turbo-s-proto", "Mario Kart 8 Deluxe prototype", F::SLink2, 27, Little, Sjis },
            { "u-king", "The Legend of Zelda: Breath of the Wild (Wii U)", F::SLink2, 28, Big, Utf8 },
            { "u-king-s", "The Legend of Zelda: Breath of the Wild (Switch), Game Builder Garage", F::SLink2, 28,
                Little, Utf8 },
            { "blitz", "Splatoon 2, ARMS, Mario Kart 8 Deluxe", F::SLink2, 28, Little, Sjis },
            { "park", "Animal Crossing: New Horizons", F::SLink2, 29, Little, Utf8 },
            { "thunder", "Splatoon 3", F::SLink2, 31, Little, Utf8 },
            { "ex-king", "The Legend of Zelda: Tears of the Kingdom", F::SLink2, 33, Little, Utf8 },
            { "spiral", "Splatoon Raiders, Nintendo Switch Sports Resort, Mario Kart World, Donkey Kong Bananza",
                F::SLink2, 34, Little, Utf8 },
        };
        return p;
    }

    const Preset* preset_for(Format f, u32 version) {
        for (const Preset& p : presets())
            if (p.format == f && p.version == version)
                return &p;
        return nullptr;
    }

    const Preset* find_preset(const std::string& key, Format f) {
        for (const Preset& p : presets())
            if (p.key == key && p.format == f)
                return &p;
        return nullptr;
    }

    std::string format_name(Format f) {
        switch (f) {
        case Format::ELink:
            return "ELink";
        case Format::SLink:
            return "SLink";
        case Format::ELink2:
            return "ELink2";
        case Format::SLink2:
            return "SLink2";
        case Format::ELink0:
            return "ELink0";
        }
        return { };
    }

    std::string version_name(Format f, u32 version) {
        if (f == Format::ELink0)
            return format_name(f);
        return format_name(f) + " v" + std::to_string(version);
    }

    u32 Document::version() const {
        // Version is the first field of every versioned root schema.
        return root && format != Format::ELink0 ? static_cast<u32>(root->i(0)) : 0;
    }

    namespace {

        // Grid containers and the action flag word exist only in the 64-bit layout.
        bool narrow_xlink(const Document& doc) {
            return is_xlink(doc.format) && !xlink_modern::is_wide(doc.version(), xlink_system(doc.format));
        }

    } // namespace

    bool stores_field(const Document& doc, const Schema& s, int field) {
        using namespace xlink;
        // SoundLink 73 users have no distance settings.
        if (doc.format == Format::SLink && doc.version() == 73 && &s == &eslink::sound_link(73).user)
            return field < eslink::user::Header1 || field > eslink::user::Header7;
        if (is_xlink(doc.format) && &s == &schemas().user && field == user::GlobalPropertyCount)
            return doc.version() != 19 && xlink_modern::has_global_count(doc.version(), xlink_system(doc.format));
        // Versions 19 and 21 know users and calls by name alone; 21 keeps no base
        // user and no call guid.
        if (is_xlink(doc.format) && xlink_early::is_early(doc.version())) {
            if (&s == &schemas().user)
                return field != user::Hash && (field != user::BaseName || doc.version() == 19);
            if (&s == &schemas().call)
                return field != call::KeyNameHash && (field != call::Guid || xlink_early::has_call_guid(doc.version()));
        }
        if (!narrow_xlink(doc))
            return true;
        if (&s == &schemas().ctn)
            return field != ctn::WatchPropertyName2 && field != ctn::LocalPropertyNameIdx2 && field != ctn::GridFlags1
                && field != ctn::GridFlags2 && field != ctn::BlendByProperty;
        if (&s == &schemas().action)
            return field != action::Flag;
        return true;
    }

    bool users_reorderable(const Document& doc) {
        return !is_xlink(doc.format) || xlink_early::is_early(doc.version());
    }

    bool stores_list(const Document& doc, const Schema& s, int list) {
        if (is_xlink(doc.format) && &s == &xlink::schemas().user && list == xlink::user::LocalProperties)
            return doc.version() != 19 && xlink_modern::has_user_locals(doc.version(), xlink_system(doc.format));
        return !narrow_xlink(doc) || &s != &xlink::schemas().ctn;
    }

    bool stores_enum_value(const Document& doc, const Schema& s, int field, s64 value) {
        if (!is_xlink(doc.format) || &s != &xlink::schemas().ctn || field != xlink::ctn::Type)
            return true;
        if (value == xlink::CT_Jump)
            return doc.version() != 19 && xlink_modern::has_global_count(doc.version(), xlink_system(doc.format));
        return !narrow_xlink(doc) || value != xlink::CT_Grid;
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
        return r.magic_at(0, "XLNK") || r.magic_at(0, "eflk") || r.magic_at(0, "SLNK") || r.magic_at(0, "BLNK");
    }

    Document read_database(Bytes payload) {
        const Reader r(payload, Endian::Big);
        if (r.magic_at(0, "XLNK")) {
            r.require(0, 0xC, "XLink header");
            Reader le(payload, Endian::Little);
            u32 version = le.u32_at(8);
            if (version > 0xFFFF)
                version = r.u32_at(8);
            if (xlink_early::is_early(version))
                return xlink_early::read(payload);
            if (version >= 27 && version <= 36)
                return xlink_modern::read(payload);
            throw Error("XLink resource version " + std::to_string(version) + " is not supported");
        }
        if (r.magic_at(0, "eflk") || r.magic_at(0, "SLNK"))
            return eslink_codec::read(payload);
        if (r.magic_at(0, "BLNK"))
            return elink0_codec::read(payload);
        throw Error("not a link database (XLNK, eflk, SLNK or BLNK)");
    }

    Buffer write_database(const Document& doc) {
        const u32 v = doc.version();
        switch (doc.format) {
        case Format::ELink2:
        case Format::SLink2:
            if (xlink_early::is_early(v))
                return xlink_early::write(doc);
            if (v >= 27 && v <= 36)
                return xlink_modern::write(doc);
            throw Error("XLink resource version " + std::to_string(v) + " cannot be written");
        case Format::ELink:
        case Format::SLink:
            return eslink_codec::write(doc);
        case Format::ELink0:
            return elink0_codec::write(doc);
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

    } // namespace

    std::string default_member_name(const std::string& path, const Document& doc) {
        std::string base = base_name(path);
        for (const char* ext : { ".szs", ".zs", ".sarc" })
            if (ends_with_ci(base, ext))
                base.resize(base.size() - std::string(ext).size());
        if (base.find('.') != std::string::npos)
            return base;
        if (is_xlink(doc.format))
            return base + (is_effect(doc.format) ? ".belnk" : ".bslnk");
        if (doc.format == Format::ELink0)
            return base + ".belink";
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
            if (kinds_match({ LayerKind::Yaz, LayerKind::Sarc }))
                return doc.layers;
            Layer y { LayerKind::Yaz };
            y.yaz_alignment = 0x100;
            Layer a { LayerKind::Sarc };
            a.archive.endian = doc.endian;
            a.archive.files.push_back({ default_member_name(path, doc), { } });
            a.sarc_index = 0;
            return { y, a };
        }
        if (zs) {
            if (kinds_match({ LayerKind::Zstd }))
                return doc.layers;
            return { Layer { LayerKind::Zstd } };
        }
        return { };
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
                    } catch (const Error&) { }
                }
            }
            std::vector<std::shared_ptr<const Buffer>> out;
            for (auto [it, end] = found->by_id.equal_range(id); it != end; ++it)
                out.push_back(it->second);
            if (out.empty())
                throw Error("compressed with Zstandard dictionary " + std::to_string(id)
                    + "; put the game's ZsDic.pack.zs or .zsdic files in " + folder);
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

} // namespace xl
