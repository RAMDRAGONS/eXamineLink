#include "model/validate.hpp"

#include <map>
#include <set>

#include "formats/codecs.hpp"
#include "model/dslink_schema.hpp"
#include "model/value_text.hpp"
#include "model/xlink_schema.hpp"

namespace xl {

    namespace {

        class Checker {
        public:
            explicit Checker(const Document& d)
                : doc(d) { }

            void add(Severity s, const Node* n, std::string text) { issues.push_back({ s, std::move(text), n }); }
            std::string where(const Node& n) const {
                std::string path;
                for (const Node* p = &n; p && p->parent(); p = p->parent())
                    path = to_utf8(p->label(), doc.encoding) + (path.empty() ? "" : " / " + path);
                return path;
            }

            void references();
            void xlink();
            void dslink();

            const Document& doc;
            std::vector<Issue> issues;
        };

        void Checker::references() {
            walk_const(*doc.root, [&](const Node& n) {
                const Schema& sc = n.schema();
                for (int f = 0; f < static_cast<int>(sc.fields.size()); ++f) {
                    const FieldDef& fd = sc.fields[f];
                    if (!fd.target || field_kind(n, f) != DynKind::Ref)
                        continue;
                    const NodeList* list = fd.target(n);
                    if (!list)
                        continue;
                    const s64 v = n.i(f), size = list->size();
                    if (fd.type == FieldType::RangeStart) {
                        const s64 end = n.i(f + 1);
                        // [-1, -1] is how shipped databases spell "no children".
                        if (end == v - 1 || (v < 0 && end < 0))
                            continue;
                        if (end < v - 1)
                            add(Severity::Error, &n, where(n) + ": " + fd.name + " range ends before it starts");
                        else if (v < 0 || end >= size)
                            add(Severity::Error, &n,
                                where(n) + ": " + fd.name + " range " + std::to_string(v) + ".." + std::to_string(end)
                                    + " runs outside " + list->def().name + " (" + std::to_string(size) + ")");
                        continue;
                    }
                    if (fd.type == FieldType::RangeEnd)
                        continue;
                    if (v == xlink::RefTableEnd)
                        add(Severity::Information, &n,
                            where(n) + ": " + fd.name + " points just past the end of " + list->def().name
                                + ", as the shipped database does");
                    else if (v < -1 || v >= size)
                        add(Severity::Error, &n,
                            where(n) + ": " + fd.name + " " + std::to_string(v) + " is outside " + list->def().name
                                + " (" + std::to_string(size) + ")");
                }
            });
        }

        void Checker::xlink() {
            using namespace xlink;
            const Node& file = *doc.root;
            const u32 v = doc.version();
            const bool v19 = v == 19, wide = !v19 && xlink_modern::is_wide(v, xlink_system(doc.format));
            const bool jumps = !v19 && xlink_modern::has_global_count(v, xlink_system(doc.format));

            const auto check_params = [&](const Node& holder, int list, const NodeList& defs, bool full) {
                std::set<s64> seen;
                for (const auto& p : holder.list(list)) {
                    const s64 d = p->i(param::Def);
                    if (d < 0 || d >= defs.size())
                        add(Severity::Error, p.get(),
                            where(*p) + ": parameter definition " + std::to_string(d) + " does not exist");
                    else if (!seen.insert(d).second)
                        add(Severity::Error, p.get(),
                            where(*p) + ": parameter "
                                + to_utf8(defs.at(static_cast<int>(d)).s(pdef::Name), doc.encoding) + " is set twice");
                    else if (p->i(param::RefType) == RT_Direct
                        && defs.at(static_cast<int>(d)).i(pdef::Type) == PT_String)
                        add(Severity::Warning, p.get(), where(*p) + ": string parameter holds a direct value");
                }
                if (full && static_cast<int>(seen.size()) != defs.size())
                    add(Severity::Warning, &holder,
                        where(holder) + ": " + std::to_string(defs.size() - static_cast<int>(seen.size()))
                            + " user parameters are missing; they are written with their defaults");
            };
            for (const auto& set : file.list(file::AssetParamSets))
                check_params(*set, pset::Params, file.list(file::AssetParamDefs), false);
            for (const auto& set : file.list(file::TriggerParamSets))
                check_params(*set, pset::Params, file.list(file::TriggerParamDefs), false);
            if (file.list(file::AssetParamDefs).size() > 64)
                add(Severity::Error, &file, "more than 64 asset parameter definitions do not fit the parameter mask");
            if (file.list(file::TriggerParamDefs).size() > 32)
                add(Severity::Error, &file, "more than 32 trigger parameter definitions do not fit the parameter mask");

            // ELink2 36 and SLink2 33 find local property names by binary search
            // (strcmp) in the database's table and in each user's list.
            const auto check_sorted = [&](const Node& holder, const NodeList& list, const std::string& what) {
                for (int k = 1; k < list.size(); ++k)
                    if (!(list.at(k - 1).s(name::Name) < list.at(k).s(name::Name))) {
                        add(Severity::Error, &list.at(k),
                            where(holder) + ": " + what + " are not in sorted order without repeats");
                        return;
                    }
            };
            if (jumps)
                check_sorted(file, file.list(file::LocalPropertyNames), "local property names");

            std::map<u32, const Node*> hashes;
            std::map<std::string, const Node*> names;
            for (const auto& up : file.list(file::Users)) {
                const Node& u = *up;
                check_params(u, user::UserParams, file.list(file::UserParamDefs), true);
                if (v19 && u.s(user::Name).empty())
                    add(Severity::Error, &u, where(u) + ": version 19 finds users by name, and this one has none");
                if (!u.s(user::Name).empty() && !names.emplace(u.s(user::Name), &u).second)
                    add(Severity::Warning, &u, where(u) + ": another user has the same name");
                if (!v19) {
                    const u32 h = u.s(user::Name).empty() ? static_cast<u32>(u.i(user::Hash)) : 0;
                    if (h && !hashes.emplace(h, &u).second)
                        add(Severity::Error, &u, where(u) + ": another user has the same hash " + hex(h, 8));
                }
                if (jumps) {
                    check_sorted(u, u.list(user::LocalProperties), "local properties");
                    const u16 globals = xlink_modern::global_property_count(file, u);
                    if (u.i(user::GlobalPropertyCount) != globals)
                        add(Severity::Information, &u,
                            where(u) + ": global property count is " + std::to_string(u.i(user::GlobalPropertyCount))
                                + " where its contents read " + std::to_string(globals));
                }
                const NodeList& calls = u.list(user::Calls);
                const NodeList& ctns = u.list(user::Containers);
                if (calls.size() > 0xFFFF)
                    add(Severity::Error, &u, where(u) + ": more than 65535 calls");
                for (const auto& cp : calls) {
                    const Node& c = *cp;
                    if (call_is_container(c.i(call::Flag))) {
                        const s64 ci = c.i(call::Container);
                        if (ci < 0)
                            add(Severity::Warning, &c, where(c) + ": container call without container parameters");
                        else if (ci < ctns.size() && ctns.at(static_cast<int>(ci)).i(ctn::Type) != CT_Jump) {
                            // A jump container points at a call another container holds, so its
                            // child keeps that container's parent index and condition.
                            const Node& ct = ctns.at(static_cast<int>(ci));
                            for (s64 k = ct.i(ctn::ChildStart); k <= ct.i(ctn::ChildEnd) && k < calls.size(); ++k) {
                                if (k < 0)
                                    continue;
                                const Node& child = calls.at(static_cast<int>(k));
                                if (child.i(call::ParentIndex) != c.row())
                                    add(Severity::Warning, &child,
                                        where(child) + ": parent index does not name the container holding it");
                                const s64 cond = child.i(call::Condition);
                                const s64 type = ct.i(ctn::Type);
                                const s64 made_for = cond >= 0 && cond < file.list(file::Conditions).size()
                                    ? file.list(file::Conditions).at(static_cast<int>(cond)).i(cond::ParentType)
                                    : type;
                                const auto random = [](s64 t) { return t == CT_Random || t == CT_Random2; };
                                if (made_for != type)
                                    // Version 19 conditions carry no type and are read as their
                                    // container's, so a mismatch cannot be stored there. Shipped
                                    // databases give blend and sequence children other containers'
                                    // conditions, and mix the two random kinds, whose conditions
                                    // share one layout.
                                    add(v19 ? Severity::Error
                                            : (type == CT_Blend || type == CT_Sequence
                                                  || (random(type) && random(made_for)))
                                            ? Severity::Information
                                            : Severity::Warning,
                                        &child, where(child) + ": condition was made for another container type");
                            }
                        }
                    } else if (c.i(call::ParamSet) < 0) {
                        add(Severity::Warning, &c, where(c) + ": asset call without asset parameters");
                    }
                    if (!v19 && (c.i(call::AssetId) < -0x8000 || c.i(call::AssetId) > 0x7FFF))
                        add(Severity::Error, &c, where(c) + ": asset id does not fit 16 bits");
                }
                for (const auto& ct : ctns) {
                    if (ct->i(ctn::Type) == CT_Jump && !jumps)
                        add(Severity::Error, ct.get(),
                            where(*ct) + ": jump containers exist only from ELink2 36 and SLink2 33");
                    if (ct->i(ctn::Type) == CT_Grid) {
                        if (!wide)
                            add(Severity::Error, ct.get(),
                                where(*ct) + ": grid containers exist only in 64-bit versions");
                        const int a = ct->list(ctn::Axis1).size(), b = ct->list(ctn::Axis2).size();
                        if (ct->list(ctn::Cells).size() != a * b)
                            add(Severity::Error, ct.get(),
                                where(*ct) + ": grid needs " + std::to_string(a * b) + " cells");
                        if (a > 255 || b > 255)
                            add(Severity::Error, ct.get(), where(*ct) + ": grid axes hold at most 255 values");
                    }
                }
                for (const int l : { user::ActionTriggers, user::PropertyTriggers, user::AlwaysTriggers })
                    for (const auto& t : u.list(l))
                        if (t->i(1) < 0)
                            add(Severity::Error, t.get(), where(*t) + ": trigger names no asset call");
            }
        }

        void Checker::dslink() {
            using namespace dslink;
            const Node& file = *doc.root;
            const u32 v = doc.version();
            const bool pooled_version = v == 10 || v == 73;
            std::set<std::string> names;
            for (const auto& u : file.list(file::Users))
                if (u->i(user::Type) == UT_User)
                    names.insert(u->s(user::Name));
            for (const auto& up : file.list(file::Users)) {
                const Node& u = *up;
                const s64 type = u.i(user::Type);
                if (type == UT_Alias && !names.contains(u.s(user::BaseName)))
                    add(Severity::Warning, &u, where(u) + ": alias of a user that does not exist");
                if (!pooled_version && type != UT_User)
                    add(Severity::Error, &u, where(u) + ": this version has no aliases or empty users");
                if (!pooled_version && (u.i(user::PooledAssets) || u.i(user::PooledTriggers)))
                    add(Severity::Error, &u, where(u) + ": this version has no shared pools");
                if (type != UT_User)
                    continue;
                for (int l = 0; l < u.list_count(); ++l)
                    if (pooled_version && u.list(l).size() > 0xFFFF)
                        add(Severity::Error, &u, where(u) + ": " + u.list(l).def().name + " exceeds 65535 entries");
                if (doc.format == Format::ELink)
                    for (const auto& slot : *asset_list_of(u))
                        if (slot->list(slot_params::Params).size() != u.i(user::ParamsPerAsset))
                            add(Severity::Error, slot.get(),
                                where(*slot) + ": has " + std::to_string(slot->list(slot_params::Params).size())
                                    + " parameter words; the user gives every effect "
                                    + std::to_string(u.i(user::ParamsPerAsset)));
                if (u.i(user::PooledAssets) && !u.list(user::Assets).empty())
                    add(Severity::Warning, &u,
                        where(u) + ": own effects are ignored while assets come from the shared pool");
            }
        }

    } // namespace

    std::string severity_name(Severity s) {
        switch (s) {
        case Severity::Information:
            return "Information";
        case Severity::Warning:
            return "Warning";
        case Severity::Error:
            return "Error";
        case Severity::Fatal:
            return "Fatal";
        }
        return { };
    }

    std::vector<Issue> validate(const Document& doc, bool try_write) {
        Checker c(doc);
        if (!doc.root)
            return { };
        c.references();
        if (is_xlink(doc.format))
            c.xlink();
        else
            c.dslink();
        if (try_write) {
            try {
                write_database(doc);
            } catch (const std::exception& e) {
                c.add(Severity::Fatal, doc.root.get(), std::string("saving fails: ") + e.what());
            }
        }
        return c.issues;
    }

} // namespace xl
