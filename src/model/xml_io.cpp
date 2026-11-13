#include "model/xml_io.hpp"

#include <QFile>
#include <QSaveFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <unordered_map>

#include "model/convert.hpp"
#include "model/dslink_schema.hpp"
#include "model/value_text.hpp"
#include "model/xlink_schema.hpp"

namespace xl::xml {

    namespace {

        constexpr const char* ToolAttr = "eXamineLink";
        constexpr const char* ToolVersion = "1";
        // Shared entries name the index they had in the exporting database.
        constexpr const char* SharedIdAttr = "_SharedId";

        QString q(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

        std::string s(QStringView v) {
            const QByteArray b = v.toUtf8();
            return std::string(b.constData(), static_cast<std::size_t>(b.size()));
        }

        bool is_xlink_param_def(const Node& n, int f) {
            return &n.schema() == &xlink::schemas().param && f == xlink::param::Def;
        }

        // Whether a raw string survives as an XML attribute: it must decode to
        // UTF-8 that encodes back to the same bytes, hold no lone surrogates (the
        // escape for undecodable Shift-JIS bytes) and no characters XML attributes
        // normalize or cannot hold.
        bool attribute_safe(const std::string& raw, Encoding enc) {
            const std::string utf8 = to_utf8(raw, enc);
            if (from_utf8(utf8, enc) != raw)
                return false;
            for (std::size_t i = 0; i < utf8.size(); ++i) {
                const u8 c = static_cast<u8>(utf8[i]);
                if (c < 0x20 || c == 0x7F)
                    return false;
                if (c == 0xED && i + 1 < utf8.size() && static_cast<u8>(utf8[i + 1]) >= 0xA0)
                    return false;
            }
            return true;
        }

        std::string to_hex_bytes(const std::string& raw) {
            return QByteArray(raw.data(), static_cast<qsizetype>(raw.size())).toHex().toStdString();
        }

        // Defaults a fresh node starts with; attributes equal to them are left out.
        const Node& defaults_of(const Schema* sc) {
            static std::map<const Schema*, std::unique_ptr<Node>> cache;
            auto& slot = cache[sc];
            if (!slot)
                slot = std::make_unique<Node>(sc);
            return *slot;
        }

        void write_node(
            QXmlStreamWriter& x, const Node& n, Encoding enc, const std::function<void(const Node&)>& extra = { }) {
            const Schema& sc = n.schema();
            x.writeStartElement(q(sc.name));
            if (extra)
                extra(n);
            const Node& def = defaults_of(&sc);
            // Layout hints (original tables, orders, padding) carry an underscore so
            // they stand apart from the data; they keep an unedited database
            // byte-identical through the XML form.
            std::vector<int> hidden_blobs;
            for (int f = 0; f < static_cast<int>(sc.fields.size()); ++f) {
                const FieldDef& fd = sc.fields[f];
                if (!fd.hidden || n.value(f) == def.value(f))
                    continue;
                if (fd.type == FieldType::String)
                    hidden_blobs.push_back(f);
                else
                    x.writeAttribute(q("_" + fd.name), q(value_to_text(n, f, enc)));
            }
            for (int f = 0; f < static_cast<int>(sc.fields.size()); ++f) {
                const FieldDef& fd = sc.fields[f];
                if (fd.hidden)
                    continue;
                // Parameters name their definition so a user file survives definition
                // tables that are ordered differently.
                if (is_xlink_param_def(n, f)) {
                    const Node* d = xlink::param_def(n);
                    x.writeAttribute(
                        q(fd.name), d ? q(to_utf8(d->s(xlink::pdef::Name), enc)) : QString::number(n.i(f)));
                    continue;
                }
                if (n.value(f) == def.value(f) && fd.type != FieldType::Dynamic)
                    continue;
                if (field_kind(n, f) == DynKind::String && !attribute_safe(n.s(f), enc)) {
                    x.writeAttribute(q(fd.name + ".hex"), q(to_hex_bytes(n.s(f))));
                    continue;
                }
                x.writeAttribute(q(fd.name), q(value_to_text(n, f, enc)));
            }
            for (const int f : hidden_blobs) {
                const std::string& raw = n.s(f);
                x.writeTextElement(q("_" + sc.fields[f].name),
                    QString::fromLatin1(QByteArray(raw.data(), static_cast<qsizetype>(raw.size())).toBase64()));
            }
            for (int l = 0; l < n.list_count(); ++l) {
                const NodeList& list = n.list(l);
                if (list.empty() || list.def().hidden)
                    continue;
                x.writeStartElement(q(list.def().name));
                for (const auto& c : list)
                    write_node(x, *c, enc);
                x.writeEndElement();
            }
            x.writeEndElement();
        }

        void open_writer(QSaveFile& f, QXmlStreamWriter& x) {
            if (!f.open(QIODevice::WriteOnly))
                throw Error("cannot write " + s(f.fileName()) + ": " + s(f.errorString()));
            x.setDevice(&f);
            x.setAutoFormatting(true);
            x.setAutoFormattingIndent(2);
            x.writeStartDocument();
        }

        void close_writer(QSaveFile& f, QXmlStreamWriter& x) {
            x.writeEndDocument();
            if (x.hasError() || !f.commit())
                throw Error("failed writing " + s(f.fileName()));
        }

        std::string encoding_name(Encoding e) { return e == Encoding::ShiftJis ? "Shift_JIS" : "UTF-8"; }

        const Schema* root_schema_for(Format f, u32 version) {
            switch (f) {
            case Format::ELink2:
            case Format::SLink2:
                return &xlink::schemas().file;
            case Format::ELink:
                return &dslink::effect_link().file;
            case Format::SLink:
                return &dslink::sound_link(version).file;
            }
            return nullptr;
        }

        bool parse_format(const std::string& name, Format& f) {
            for (const Format c : { Format::ELink, Format::SLink, Format::ELink2, Format::SLink2 })
                if (format_name(c) == name) {
                    f = c;
                    return true;
                }
            return false;
        }

        // Reads elements into nodes. Dynamic fields depend on their siblings and,
        // for parameters, on the database's definitions, so their text is held back
        // and parsed once the tree is in place.
        class NodeReader {
        public:
            NodeReader(QXmlStreamReader& x, Encoding enc, std::string file)
                : x(x)
                , m_enc(enc)
                , m_file(std::move(file)) { }

            // With the reader on `node`'s start element, fills it from the element.
            void read(Node& node) {
                const Schema& sc = node.schema();
                for (const QXmlStreamAttribute& a : x.attributes()) {
                    std::string name = s(a.name());
                    const bool hidden = name.size() > 1 && name[0] == '_';
                    if (hidden)
                        name.erase(0, 1);
                    if (name.size() > 4 && name.ends_with(".hex")) {
                        const int f = sc.field_index(name.substr(0, name.size() - 4));
                        if (f < 0) {
                            warn("unknown attribute " + name + " on " + sc.name);
                            continue;
                        }
                        const QByteArray raw = QByteArray::fromHex(a.value().toLatin1());
                        node.set_s(f, std::string(raw.constData(), static_cast<std::size_t>(raw.size())));
                        continue;
                    }
                    const int f = sc.field_index(name);
                    if (f >= 0 && hidden != sc.fields[f].hidden) {
                        warn("attribute " + s(a.name()) + " on " + sc.name + " is spelled for the wrong kind of field");
                        continue;
                    }
                    if (f < 0) {
                        if (!m_meta.contains(s(a.name())))
                            warn("unknown attribute " + name + " on " + sc.name);
                        continue;
                    }
                    const std::string text = s(a.value());
                    if (sc.fields[f].type == FieldType::Dynamic || is_xlink_param_def(node, f)) {
                        deferred[&node].emplace_back(f, text);
                        continue;
                    }
                    Value v;
                    if (const auto err = text_to_value(node, f, text, m_enc, v))
                        fail(sc.name + "." + name + ": " + *err);
                    node.value(f) = v;
                }
                while (x.readNextStartElement()) {
                    const std::string list_name = s(x.name());
                    if (list_name.size() > 1 && list_name[0] == '_') {
                        const int f = sc.field_index(list_name.substr(1));
                        if (f < 0 || !sc.fields[f].hidden || sc.fields[f].type != FieldType::String) {
                            warn("unknown element " + list_name + " in " + sc.name);
                            x.skipCurrentElement();
                            continue;
                        }
                        const QByteArray raw = QByteArray::fromBase64(x.readElementText().toLatin1());
                        node.set_s(f, std::string(raw.constData(), static_cast<std::size_t>(raw.size())));
                        continue;
                    }
                    const int l = sc.list_index(list_name);
                    if (l < 0) {
                        warn("unknown element " + list_name + " in " + sc.name);
                        x.skipCurrentElement();
                        continue;
                    }
                    NodeList& list = node.list(l);
                    const Schema* el = list.def().element;
                    while (x.readNextStartElement()) {
                        if (s(x.name()) != el->name)
                            fail("expected " + el->name + " in " + list_name + ", found " + s(x.name()));
                        read(list.append(std::make_unique<Node>(el)));
                    }
                }
            }

            [[noreturn]] void fail(const std::string& msg) const {
                throw Error(m_file + " line " + std::to_string(x.lineNumber()) + ": " + msg);
            }
            void warn(const std::string& msg) {
                warnings.push_back(m_file + " line " + std::to_string(x.lineNumber()) + ": " + msg);
            }

            QXmlStreamReader& x;
            Encoding m_enc;
            // Attributes describing the file rather than a field.
            std::set<std::string> m_meta { ToolAttr, "Format", "Encoding", "ByteOrder", SharedIdAttr };
            std::string m_file;
            std::map<const Node*, std::vector<std::pair<int, std::string>>> deferred;
            std::vector<std::string> warnings;
        };

        // Parses held-back values: parameter definitions by name first, since a
        // parameter's value is read according to its definition's type.
        void finalize(std::map<const Node*, std::vector<std::pair<int, std::string>>>& deferred, Encoding enc,
            std::vector<std::string>& warnings) {
            for (auto& [cn, items] : deferred) {
                Node& n = const_cast<Node&>(*cn);
                for (auto& [f, text] : items) {
                    if (!is_xlink_param_def(n, f))
                        continue;
                    const NodeList* defs = xlink::param_defs_for(n);
                    s64 index = -1;
                    if (defs)
                        for (int k = 0; k < defs->size(); ++k)
                            if (to_utf8(defs->at(k).s(xlink::pdef::Name), enc) == text) {
                                index = k;
                                break;
                            }
                    if (index < 0) {
                        char* end = nullptr;
                        const long long v = std::strtoll(text.c_str(), &end, 0);
                        if (!text.empty() && !*end)
                            index = v;
                        else
                            warnings.push_back("parameter definition \"" + text + "\" does not exist in this database");
                    }
                    n.set_i(f, index);
                }
            }
            for (auto& [cn, items] : deferred) {
                Node& n = const_cast<Node&>(*cn);
                for (auto& [f, text] : items) {
                    if (is_xlink_param_def(n, f))
                        continue;
                    Value v;
                    if (const auto err = text_to_value(n, f, text, enc, v)) {
                        warnings.push_back(n.schema().name + "." + n.schema().fields[f].name + ": " + *err);
                        continue;
                    }
                    n.value(f) = v;
                }
            }
        }

        std::string user_suffix(Format f) {
            std::string name = format_name(f);
            for (char& c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return "." + name + ".xml";
        }

        // Visits every Ref-like field below `n` whose target is a list of `root`.
        void root_refs(Node& n, const Node& root, const std::function<void(Node&, int, int)>& fn) {
            walk(n, [&](Node& m) {
                const Schema& sc = m.schema();
                for (int f = 0; f < static_cast<int>(sc.fields.size()); ++f) {
                    const FieldDef& fd = sc.fields[f];
                    const bool ref = fd.type == FieldType::Ref || fd.type == FieldType::RangeStart
                        || fd.type == FieldType::RangeEnd
                        || (fd.type == FieldType::Dynamic && fd.dyn_kind && fd.dyn_kind(m) == DynKind::Ref);
                    if (!ref || !fd.target)
                        continue;
                    const NodeList* t = fd.target(m);
                    if (t && t->owner() == &root)
                        fn(m, f, t->index());
                }
            });
        }

    } // namespace

    bool same_content(const Node& a, const Node& b) {
        if (&a.schema() != &b.schema())
            return false;
        const Schema& sc = a.schema();
        for (int f = 0; f < static_cast<int>(sc.fields.size()); ++f)
            if (!sc.fields[f].hidden && !(a.value(f) == b.value(f)))
                return false;
        for (int l = 0; l < a.list_count(); ++l) {
            if (a.list(l).size() != b.list(l).size())
                return false;
            for (int k = 0; k < a.list(l).size(); ++k)
                if (!same_content(a.list(l).at(k), b.list(l).at(k)))
                    return false;
        }
        return true;
    }

    void save_database(const Document& doc, const std::string& path) {
        QSaveFile f(q(path));
        QXmlStreamWriter x;
        open_writer(f, x);
        write_node(x, *doc.root, doc.encoding, [&](const Node&) {
            x.writeAttribute(ToolAttr, ToolVersion);
            x.writeAttribute("Format", q(format_name(doc.format)));
            x.writeAttribute("Encoding", q(encoding_name(doc.encoding)));
            x.writeAttribute("ByteOrder", doc.endian == Endian::Big ? "Big" : "Little");
        });
        close_writer(f, x);
    }

    Document load_database(const std::string& path, std::vector<std::string>& warnings_out) {
        QFile f(q(path));
        if (!f.open(QIODevice::ReadOnly))
            throw Error("cannot open " + path);
        QXmlStreamReader x(&f);
        if (!x.readNextStartElement())
            throw Error(path + ": no root element");
        Format fmt;
        if (!parse_format(s(x.attributes().value("Format")), fmt) && !parse_format(s(x.name()), fmt))
            throw Error(path + ": root element names no link database format");
        const u32 version = x.attributes().value("Version").toUInt();
        Document doc;
        doc.format = fmt;
        const Preset* p = preset_for(fmt, version);
        doc.endian = p ? p->endian : Endian::Little;
        doc.encoding = p ? p->encoding : (version >= 31 ? Encoding::Utf8 : Encoding::ShiftJis);
        const auto enc = x.attributes().value("Encoding");
        if (enc == u"Shift_JIS")
            doc.encoding = Encoding::ShiftJis;
        else if (enc == u"UTF-8")
            doc.encoding = Encoding::Utf8;
        const auto order = x.attributes().value("ByteOrder");
        if (order == u"Big")
            doc.endian = Endian::Big;
        else if (order == u"Little")
            doc.endian = Endian::Little;
        const Schema* rs = root_schema_for(fmt, version);
        if (s(x.name()) != rs->name)
            throw Error(path + ": expected root element " + rs->name);
        doc.root = std::make_unique<Node>(rs);
        NodeReader reader(x, doc.encoding, path);
        reader.read(*doc.root);
        if (x.hasError())
            throw Error(path + " line " + std::to_string(x.lineNumber()) + ": " + s(x.errorString()));
        std::vector<std::string> warnings = reader.warnings;
        finalize(reader.deferred, doc.encoding, warnings);
        warnings_out.insert(warnings_out.end(), warnings.begin(), warnings.end());
        return doc;
    }

    std::string user_file_name(const Document& doc, const Node& user) {
        std::string base = to_utf8(user.label(), doc.encoding);
        for (char& c : base)
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>'
                || c == '|')
                c = '_';
        return base + user_suffix(doc.format);
    }

    namespace {

        // Database-wide entries reachable from the user, followed transitively:
        // parameter sets point at curves, random entries and arrange groups.
        std::map<int, std::set<s64>> shared_entries(const Node& root, const Node& user) {
            std::map<int, std::set<s64>> needed;
            std::vector<const Node*> frontier { &user };
            while (!frontier.empty()) {
                const Node* n = frontier.back();
                frontier.pop_back();
                root_refs(const_cast<Node&>(*n), root, [&](Node& m, int f, int list) {
                    const s64 i = m.i(f);
                    if (i < 0 || i >= root.list(list).size())
                        return;
                    if (m.schema().fields[f].type == FieldType::RangeStart) {
                        for (s64 k = i; k <= m.i(f + 1) && k < root.list(list).size(); ++k)
                            if (needed[list].insert(k).second)
                                frontier.push_back(&root.list(list).at(static_cast<int>(k)));
                        return;
                    }
                    if (needed[list].insert(i).second)
                        frontier.push_back(&root.list(list).at(static_cast<int>(i)));
                });
            }
            return needed;
        }

    } // namespace

    void export_user(const Document& doc, const Node& user, const std::string& path) {
        const Node& root = *doc.root;
        const std::map<int, std::set<s64>> needed = shared_entries(root, user);

        QSaveFile f(q(path));
        QXmlStreamWriter x;
        open_writer(f, x);
        x.writeStartElement(q(root.schema().name + "User"));
        x.writeAttribute(ToolAttr, ToolVersion);
        x.writeAttribute("Format", q(format_name(doc.format)));
        x.writeAttribute("Version", QString::number(doc.version()));
        write_node(x, user, doc.encoding);
        if (!needed.empty()) {
            x.writeStartElement("Shared");
            for (const auto& [list, ids] : needed) {
                x.writeStartElement(q(root.list(list).def().name));
                for (const s64 id : ids)
                    write_node(x, root.list(list).at(static_cast<int>(id)), doc.encoding,
                        [&](const Node&) { x.writeAttribute(SharedIdAttr, QString::number(id)); });
                x.writeEndElement();
            }
            x.writeEndElement();
        }
        x.writeEndElement();
        close_writer(f, x);
    }

    UserImport read_user(const Document& doc, const std::string& path) {
        QFile f(q(path));
        if (!f.open(QIODevice::ReadOnly))
            throw Error("cannot open " + path);
        QXmlStreamReader x(&f);
        if (!x.readNextStartElement())
            throw Error(path + ": no root element");
        const Node& root = *doc.root;
        if (s(x.name()) != root.schema().name + "User")
            throw Error(path + " holds a " + s(x.name()) + ", not a " + root.schema().name + " user");
        Format user_format;
        if (!parse_format(s(x.attributes().value("Format")), user_format))
            throw Error(path + " names no link database format");
        if (user_format != doc.format)
            throw Error(
                path + " is an " + format_name(user_format) + " user; this database is " + format_name(doc.format));
        bool has_version = false;
        const u32 version = x.attributes().value("Version").toUInt(&has_version);
        const bool foreign = has_version && version != doc.version();

        // A user of another version is read into a database of its own version,
        // which is then converted the way a whole database would be.
        Document src;
        src.format = doc.format;
        src.endian = doc.endian;
        src.encoding = doc.encoding;
        if (foreign) {
            src.root = std::make_unique<Node>(root_schema_for(doc.format, version));
            src.root->set_i(0, version);
            if (is_xlink(doc.format)) {
                // Parameters name their definitions; the target's are the ones that count.
                for (const int l :
                    { xlink::file::UserParamDefs, xlink::file::AssetParamDefs, xlink::file::TriggerParamDefs })
                    for (const auto& d : root.list(l))
                        src.root->list(l).append(d->clone());
            }
        }
        const Node& schema_root = foreign ? *src.root : root;

        UserImport imp;
        NodeReader reader(x, doc.encoding, path);
        const Schema* user_schema = schema_root.list(schema_root.list_count() - 1).def().element;
        while (x.readNextStartElement()) {
            const std::string name = s(x.name());
            if (name == user_schema->name && !imp.user) {
                imp.user = std::make_unique<Node>(user_schema);
                reader.read(*imp.user);
            } else if (name == "Shared") {
                while (x.readNextStartElement()) {
                    const int l = schema_root.schema().list_index(s(x.name()));
                    if (l < 0) {
                        reader.warn("unknown shared table " + s(x.name()));
                        x.skipCurrentElement();
                        continue;
                    }
                    const Schema* el = schema_root.list(l).def().element;
                    while (x.readNextStartElement()) {
                        bool ok = false;
                        const s64 id = x.attributes().value(SharedIdAttr).toLongLong(&ok);
                        if (!ok)
                            reader.fail("shared entry without an Id");
                        auto node = std::make_unique<Node>(el);
                        reader.read(*node);
                        imp.shared.push_back({ l, id, std::move(node) });
                    }
                }
            } else {
                reader.warn("unexpected element " + name);
                x.skipCurrentElement();
            }
        }
        if (x.hasError())
            throw Error(path + " line " + std::to_string(x.lineNumber()) + ": " + s(x.errorString()));
        if (!imp.user)
            throw Error(path + " holds no user");
        imp.warnings = std::move(reader.warnings);
        if (!foreign) {
            imp.deferred = std::move(reader.deferred);
            return imp;
        }

        // Shared entries go back to the positions the file gave them, so the
        // user's references hold while the copy is finalized and converted.
        Node& sr = *src.root;
        for (auto& sh : imp.shared) {
            if (sh.id < 0 || sh.id > 0xFFFFFF)
                throw Error(path + ": shared entry id " + std::to_string(sh.id) + " is out of range");
            NodeList& list = sr.list(sh.list);
            while (list.size() <= sh.id)
                list.append_new();
            list.take(static_cast<int>(sh.id));
            list.insert(static_cast<int>(sh.id), std::move(sh.node));
        }
        imp.shared.clear();
        NodeList& src_users = sr.list(sr.list_count() - 1);
        src_users.append(std::move(imp.user));
        finalize(reader.deferred, doc.encoding, imp.warnings);

        std::vector<std::string> losses;
        const Document conv = convert(src, Target { doc.format, doc.version(), doc.endian, doc.encoding }, losses);
        for (const auto& l : losses)
            imp.warnings.push_back("converting from version " + std::to_string(version) + ": " + l);
        const Node& cr = *conv.root;
        const NodeList& conv_users = cr.list(cr.list_count() - 1);
        if (conv_users.size() != 1)
            throw Error(path + ": the user does not survive conversion from version " + std::to_string(version)
                + " (an alias without its base user, or an empty user)");
        UserImport out = user_import_from(conv, conv_users.at(0));
        out.warnings = std::move(imp.warnings);
        return out;
    }

    UserImport user_import_from(const Document& doc, const Node& user) {
        const Node& root = *doc.root;
        UserImport imp;
        imp.user = user.clone();
        for (const auto& [list, ids] : shared_entries(root, user))
            for (const s64 id : ids)
                imp.shared.push_back({ list, id, root.list(list).at(static_cast<int>(id)).clone() });
        return imp;
    }

    int find_matching_user(const Document& doc, const UserImport& imp) {
        const NodeList& users = doc.root->list(doc.root->list_count() - 1);
        const std::string& name = imp.user->s(0);
        for (int k = 0; k < users.size(); ++k) {
            const Node& u = users.at(k);
            if (!name.empty() && u.s(0) == name)
                return k;
            if (is_xlink(doc.format) && imp.user->i(xlink::user::Hash) != 0
                && u.i(xlink::user::Hash) == imp.user->i(xlink::user::Hash))
                return k;
        }
        return -1;
    }

    namespace {

        // Hash of a node's visible content, for finding identical table entries.
        std::size_t content_hash(const Node& n) {
            std::size_t h = std::hash<const void*>()(&n.schema());
            const auto mix = [&h](std::size_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
            const Schema& sc = n.schema();
            for (int f = 0; f < static_cast<int>(sc.fields.size()); ++f) {
                if (sc.fields[f].hidden)
                    continue;
                mix(std::hash<s64>()(n.i(f)));
                mix(std::hash<std::string>()(n.s(f)));
            }
            for (int l = 0; l < n.list_count(); ++l) {
                mix(static_cast<std::size_t>(n.list(l).size()));
                for (const auto& c : n.list(l))
                    mix(content_hash(*c));
            }
            return h;
        }

        // A root holding copies of the target database's definition tables, so
        // imported nodes can be finalized (parameter values read by their
        // definition's type) before any of them touches the real database.
        std::unique_ptr<Node> shadow_root(const Document& doc) {
            auto shadow = std::make_unique<Node>(&doc.root->schema());
            const Node& root = *doc.root;
            for (int f = 0; f < static_cast<int>(root.schema().fields.size()); ++f)
                shadow->value(f) = root.value(f);
            if (is_xlink(doc.format))
                for (const int l :
                    { xlink::file::UserParamDefs, xlink::file::AssetParamDefs, xlink::file::TriggerParamDefs })
                    for (const auto& d : root.list(l))
                        shadow->list(l).append(d->clone());
            return shadow;
        }

        // A position in a local property name table, stored next to the name it
        // stands for. Action switches keep an action hash in the same field.
        struct LocalNameRef {
            Node* node;
            int name_field;
            int index_field;
            bool enum_table;
        };

        void local_name_refs(
            Node& user, const std::function<Node*(int list, s64 id)>& shared, std::vector<LocalNameRef>& out) {
            using namespace xlink;
            NodeList& calls = user.list(user::Calls);
            NodeList& ctns = user.list(user::Containers);
            // The container a call sits in, when that container is a switch.
            const auto parent_switch = [&](const Node& call) -> const Node* {
                const s64 parent = call.i(call::ParentIndex);
                if (parent < 0 || parent >= calls.size())
                    return nullptr;
                const s64 c = calls.at(static_cast<int>(parent)).i(call::Container);
                if (c < 0 || c >= ctns.size() || ctns.at(static_cast<int>(c)).i(ctn::Type) != CT_Switch)
                    return nullptr;
                return &ctns.at(static_cast<int>(c));
            };
            for (auto& c : ctns) {
                if (c->i(ctn::SwitchByte) != 0)
                    continue;
                if (!c->s(ctn::WatchPropertyName).empty() && c->i(ctn::LocalPropertyNameIdx) >= 0)
                    out.push_back({ c.get(), ctn::WatchPropertyName, ctn::LocalPropertyNameIdx, false });
                if (!c->s(ctn::WatchPropertyName2).empty() && c->i(ctn::LocalPropertyNameIdx2) >= 0)
                    out.push_back({ c.get(), ctn::WatchPropertyName2, ctn::LocalPropertyNameIdx2, false });
            }
            // Only property switches resolve a condition's enum name index; shipped
            // databases share switch conditions with other containers' children.
            // Conditions are shared, and one that an action switch child uses holds an
            // action hash there for every call sharing it.
            std::set<s64> property_conditions, action_conditions;
            for (auto& c : calls) {
                const s64 id = c->i(call::Condition);
                if (const Node* sw = parent_switch(*c))
                    (sw->i(ctn::SwitchByte) != 0 ? action_conditions : property_conditions).insert(id);
            }
            const auto condition = [&](s64 id) {
                Node* c = id >= 0 && !action_conditions.count(id) ? shared(file::Conditions, id) : nullptr;
                if (c && c->i(cond::ParentType) == CT_Switch && c->i(cond::PropertyType) == PR_Enum
                    && c->i(cond::LocalPropertyEnumNameIdx) >= 0)
                    out.push_back({ c, cond::Value, cond::LocalPropertyEnumNameIdx, true });
            };
            for (const s64 id : property_conditions)
                condition(id);
            for (auto& t : user.list(user::PropertyTriggers))
                condition(t->i(ptrig::Condition));
        }

        // Points the import's local property indices at `doc`'s tables, adding the
        // names they lack. The tables are kept sorted when they are, so entries of
        // the database whose index matched their name move with it.
        void bind_local_names(
            Document& doc, EditStack& edits, Node& user, const std::map<std::pair<int, s64>, Node*>& by_id) {
            using namespace xlink;
            if (!is_xlink(doc.format) || doc.version() == 19)
                return;
            std::vector<LocalNameRef> refs;
            local_name_refs(
                user,
                [&](int list, s64 id) {
                    const auto it = by_id.find({ list, id });
                    return it == by_id.end() ? nullptr : it->second;
                },
                refs);
            for (auto& [key, n] : by_id)
                if (key.first == file::Curves && !n->s(curve::PropName).empty() && n->i(curve::IsPropGlobal) == 0
                    && n->i(curve::LocalPropertyNameIdx) >= 0)
                    refs.push_back({ n, curve::PropName, curve::LocalPropertyNameIdx, false });
            if (refs.empty())
                return;

            Node& root = *doc.root;
            for (const bool enums : { false, true }) {
                NodeList& table = root.list(enums ? file::LocalPropertyEnumNames : file::LocalPropertyNames);
                std::vector<std::string> names;
                for (const auto& n : table)
                    names.push_back(n->s(name::Name));
                std::set<std::string> missing;
                for (const auto& r : refs)
                    if (r.enum_table == enums
                        && std::find(names.begin(), names.end(), r.node->s(r.name_field)) == names.end())
                        missing.insert(r.node->s(r.name_field));
                if (!missing.empty()) {
                    const bool sorted = std::is_sorted(names.begin(), names.end());
                    std::vector<std::string> next = names;
                    next.insert(next.end(), missing.begin(), missing.end());
                    if (sorted)
                        std::sort(next.begin(), next.end());
                    const auto position = [&](const std::string& n) {
                        return static_cast<s64>(std::find(next.begin(), next.end(), n) - next.begin());
                    };
                    // Entries of the database whose index named their own name follow it.
                    const auto follow = [&](Node& n, int name_field, int index_field) {
                        const s64 i = n.i(index_field);
                        if (i < 0 || i >= static_cast<s64>(names.size())
                            || names[static_cast<std::size_t>(i)] != n.s(name_field))
                            return;
                        const s64 to = position(n.s(name_field));
                        if (to != i) {
                            Value v;
                            v.i = to;
                            edits.set_value(n, index_field, v);
                        }
                    };
                    if (enums) {
                        for (auto& c : root.list(file::Conditions))
                            if (c->i(cond::ParentType) == CT_Switch && c->i(cond::PropertyType) == PR_Enum)
                                follow(*c, cond::Value, cond::LocalPropertyEnumNameIdx);
                    } else {
                        for (auto& u : root.list(file::Users))
                            for (auto& c : u->list(user::Containers)) {
                                follow(*c, ctn::WatchPropertyName, ctn::LocalPropertyNameIdx);
                                follow(*c, ctn::WatchPropertyName2, ctn::LocalPropertyNameIdx2);
                            }
                        for (auto& c : root.list(file::Curves))
                            follow(*c, curve::PropName, curve::LocalPropertyNameIdx);
                    }
                    for (int k = 0; k < static_cast<int>(next.size()); ++k)
                        if (k >= table.size() || table.at(k).s(name::Name) != next[static_cast<std::size_t>(k)]) {
                            auto n = std::make_unique<Node>(&schemas().name);
                            n->set_s(name::Name, next[static_cast<std::size_t>(k)]);
                            edits.insert(table, k, std::move(n));
                        }
                    names = std::move(next);
                }
                for (const auto& r : refs)
                    if (r.enum_table == enums)
                        r.node->set_i(r.index_field,
                            static_cast<s64>(
                                std::find(names.begin(), names.end(), r.node->s(r.name_field)) - names.begin()));
            }
        }

    } // namespace

    int apply_user_import(Document& doc, EditStack& edits, UserImport imp, int replace) {
        Node& root = *doc.root;
        NodeList& users = root.list(root.list_count() - 1);
        std::vector<std::string> warnings = imp.warnings;
        edits.begin_group("Import user " + to_utf8(imp.user->label(), doc.encoding));

        // Finalize everything inside the shadow, where ids are still the file's.
        auto shadow = shadow_root(doc);
        std::vector<Node*> shared_nodes;
        for (auto& sh : imp.shared)
            shared_nodes.push_back(&shadow->list(sh.list).append(std::move(sh.node)));
        Node* user = &shadow->list(shadow->list_count() - 1).append(std::move(imp.user));
        finalize(imp.deferred, doc.encoding, warnings);
        {
            std::map<std::pair<int, s64>, Node*> by_id;
            for (std::size_t k = 0; k < imp.shared.size(); ++k)
                by_id[{ imp.shared[k].list, imp.shared[k].id }] = shared_nodes[k];
            bind_local_names(doc, edits, *user, by_id);
        }

        // Resolve shared entries in dependency order: an entry is placed once the
        // entries it points at are, reusing an identical existing entry if any.
        std::map<std::pair<int, s64>, s64> mapping;
        std::map<int, std::unordered_multimap<std::size_t, int>> existing;
        std::map<int, int> appended;
        const auto index_of = [&](int list) -> std::unordered_multimap<std::size_t, int>& {
            auto [it, fresh] = existing.try_emplace(list);
            if (fresh)
                for (int k = 0; k < root.list(list).size(); ++k)
                    it->second.emplace(content_hash(root.list(list).at(k)), k);
            return it->second;
        };
        const auto remap_refs = [&](Node& subtree, bool partial) {
            bool complete = true;
            root_refs(subtree, *shadow, [&](Node& m, int f, int list) {
                const s64 v = m.i(f);
                if (v < 0)
                    return;
                const auto it = mapping.find({ list, v });
                if (it != mapping.end())
                    m.set_i(f, it->second);
                else if (partial)
                    complete = false;
                else if (m.schema().fields[f].type != FieldType::RangeEnd)
                    m.set_i(f, -1);
            });
            return complete;
        };
        std::vector<std::size_t> pending(imp.shared.size());
        for (std::size_t k = 0; k < pending.size(); ++k)
            pending[k] = k;
        std::vector<std::pair<int, std::unique_ptr<Node>>> to_add;
        while (!pending.empty()) {
            std::vector<std::size_t> next;
            for (const std::size_t k : pending) {
                Node& n = *shared_nodes[k];
                // Checking readiness on a copy keeps old ids intact until every
                // target is known.
                auto probe = n.clone();
                shadow->list(imp.shared[k].list).append(std::move(probe));
                Node& placed_probe = shadow->list(imp.shared[k].list).at(shadow->list(imp.shared[k].list).size() - 1);
                const bool ready = remap_refs(placed_probe, true);
                auto owned = shadow->list(imp.shared[k].list).take(placed_probe.row());
                if (!ready) {
                    next.push_back(k);
                    continue;
                }
                const int list = imp.shared[k].list;
                const std::size_t h = content_hash(*owned);
                s64 target = -1;
                const auto range = index_of(list).equal_range(h);
                for (auto it = range.first; it != range.second; ++it)
                    if (same_content(root.list(list).at(it->second), *owned)) {
                        target = it->second;
                        break;
                    }
                if (target < 0) {
                    target = root.list(list).size() + appended[list]++;
                    to_add.emplace_back(list, std::move(owned));
                }
                mapping[{ list, imp.shared[k].id }] = target;
            }
            if (next.size() == pending.size()) {
                // Entries pointing at each other in a cycle: place them as they are.
                for (const std::size_t k : next) {
                    const int list = imp.shared[k].list;
                    auto owned = shared_nodes[k]->clone();
                    mapping[{ list, imp.shared[k].id }] = root.list(list).size() + appended[list]++;
                    to_add.emplace_back(list, std::move(owned));
                }
                for (auto& [list, node] : to_add)
                    remap_refs(*node, false);
                break;
            }
            pending = std::move(next);
        }
        auto user_owned = shadow->list(shadow->list_count() - 1).take(user->row());
        // Root references inside the user resolve against the shadow; reattach
        // it there to remap, then move it into the database.
        Node& again = shadow->list(shadow->list_count() - 1).append(std::move(user_owned));
        remap_refs(again, false);
        user_owned = shadow->list(shadow->list_count() - 1).take(again.row());

        for (auto& [list, node] : to_add)
            edits.insert(root.list(list), root.list(list).size(), std::move(node));
        int at;
        if (replace >= 0) {
            edits.remove(users, replace);
            at = replace;
        } else {
            at = users.size();
        }
        edits.insert(users, at, std::move(user_owned));
        edits.end_group();
        return at;
    }

} // namespace xl::xml
