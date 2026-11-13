#include "model/bridge.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "core/crc32.hpp"
#include "core/text.hpp"
#include "formats/codecs.hpp"
#include "model/convert.hpp"
#include "model/eslink_schema.hpp"
#include "model/factory.hpp"
#include "model/xlink_schema.hpp"

namespace xl {

    namespace {

        namespace X = xlink;
        namespace D = eslink;

        constexpr s32 Forever = INT_MAX;

        std::string count_text(std::size_t n, const std::string& what) { return std::to_string(n) + " " + what; }

        int field_of(const Schema& s, std::string_view name) {
            const int f = s.field_index(name);
            if (f < 0)
                throw Error("internal: " + s.name + " has no field " + std::string(name));
            return f;
        }

        // The version of an older format without shared pools or aliases, so every
        // user keeps its records to itself.
        u32 plain_version(Format f) { return f == Format::ELink ? 9 : 69; }

        NodeList& users_of(Node& root) { return root.list(root.list_count() - 1); }

        // Users of `src` in `doc`'s encoding, an older format's in its plain version.
        Document normalized(const Document& src, const Document& doc, std::vector<std::string>& warnings) {
            const u32 v
                = is_xlink(src.format) || src.format == Format::ELink0 ? src.version() : plain_version(src.format);
            std::vector<std::string> losses;
            Document out = convert(src, Target { src.format, v, doc.endian, doc.encoding }, losses);
            for (const auto& l : losses)
                warnings.push_back("reading the source: " + l);
            return out;
        }

        int def_index(const NodeList& defs, std::string_view name) {
            for (int k = 0; k < defs.size(); ++k)
                if (defs.at(k).s(X::pdef::Name) == name)
                    return k;
            return -1;
        }

        // Guids only need to differ within a user; deriving them from names keeps
        // an import reproducible.
        class GuidSource {
        public:
            explicit GuidSource(std::string user)
                : m_user(std::move(user)) { }
            u32 next(const std::string& what) {
                u32 g = crc32(m_user + ":" + what) & 0x7FFFFFFF;
                while (g == 0 || !m_used.insert(g).second)
                    g = (g * 1103515245u + 12345u) & 0x7FFFFFFF;
                return g;
            }

        private:
            std::string m_user;
            std::set<u32> m_used;
        };

        // Values of `field` across `nodes`, most frequent first.
        s64 most_common(const std::vector<const Node*>& nodes, int field, s64 fallback) {
            std::map<s64, int> counts;
            for (const Node* n : nodes)
                ++counts[n->i(field)];
            s64 best = fallback;
            int best_n = 0;
            for (const auto& [v, n] : counts)
                if (n > best_n) {
                    best = v;
                    best_n = n;
                }
            return best;
        }

        // Everything collected about fields a record carries that the other format
        // has no place for, reported once per field.
        class Unmapped {
        public:
            void note(const Node& n, const std::set<int>& used) {
                const Schema& s = n.schema();
                for (int f = 0; f < static_cast<int>(s.fields.size()); ++f) {
                    const FieldDef& fd = s.fields[f];
                    if (fd.hidden || used.contains(f) || fd.type == FieldType::RangeStart
                        || fd.type == FieldType::RangeEnd)
                        continue;
                    if (n.i(f) != 0 || !n.s(f).empty())
                        ++m_counts[s.name + "." + fd.name];
                }
            }
            void report(std::vector<std::string>& warnings, const std::string& target) const {
                for (const auto& [what, n] : m_counts)
                    warnings.push_back(count_text(n, "values of " + what + " have no place in " + target));
            }

        private:
            std::map<std::string, std::size_t> m_counts;
        };

        // ------------------------------------------------------------ old to XLink

        // An EffectLink call table plays one of its effects: the one whose set
        // conditions all match the three values the game passes, preferring the
        // effect with the most conditions set and then the first. A plan states that
        // choice as nested XLink switches on the condition properties.
        struct Plan {
            enum Kind { Empty, Leaf, Switch } kind = Empty;
            int effect = -1;
            int level = -1;
            // Each case's value, "" for the default, and what it plays.
            std::vector<std::string> values;
            std::vector<Plan> plans;
            bool operator==(const Plan&) const = default;
        };

        struct Candidate {
            int effect;
            std::array<std::string, 3> conditions;
            int set_count() const {
                return static_cast<int>(
                    std::count_if(conditions.begin(), conditions.end(), [](auto& c) { return !c.empty(); }));
            }
        };

        Plan plan_for(const std::vector<Candidate>& cands, int level) {
            Plan p;
            if (cands.empty())
                return p;
            int j = level;
            while (j < 3
                && std::none_of(
                    cands.begin(), cands.end(), [j](const Candidate& c) { return !c.conditions[j].empty(); }))
                ++j;
            if (j == 3) {
                const Candidate* best = &cands.front();
                for (const auto& c : cands)
                    if (c.set_count() > best->set_count())
                        best = &c;
                p.kind = Plan::Leaf;
                p.effect = best->effect;
                return p;
            }
            std::vector<std::string> values;
            for (const auto& c : cands)
                if (!c.conditions[j].empty()
                    && std::find(values.begin(), values.end(), c.conditions[j]) == values.end())
                    values.push_back(c.conditions[j]);
            std::vector<Candidate> rest;
            for (const auto& c : cands)
                if (c.conditions[j].empty())
                    rest.push_back(c);
            const Plan fallback = plan_for(rest, j + 1);
            p.kind = Plan::Switch;
            p.level = j;
            for (const auto& v : values) {
                std::vector<Candidate> sub;
                for (const auto& c : cands)
                    if (c.conditions[j].empty() || c.conditions[j] == v)
                        sub.push_back(c);
                Plan child = plan_for(sub, j + 1);
                // A case the default would decide the same way adds nothing.
                if (!(child == fallback)) {
                    p.values.push_back(v);
                    p.plans.push_back(std::move(child));
                }
            }
            if (fallback.kind != Plan::Empty) {
                p.values.emplace_back();
                p.plans.push_back(fallback);
            }
            if (p.plans.size() == 1 && p.values.front().empty())
                return p.plans.front();
            return p;
        }

        class ToXLink {
        public:
            ToXLink(
                const Document& doc, const Document& src, const BridgeOptions& opt, std::vector<std::string>& warnings)
                : m_doc(doc)
                , m_src(src)
                , m_opt(opt)
                , m_warnings(warnings)
                , m_sound(src.format == Format::SLink)
                , m_fs(m_sound ? D::sound_link(plain_version(src.format)) : D::effect_link()) {
                m_x.format = counterpart(src.format);
                m_x.endian = doc.endian;
                m_x.encoding = doc.encoding;
                m_x.root = std::make_unique<Node>(&X::schemas().file);
                Node& f = *m_x.root;
                const Node& t = *doc.root;
                for (int k = 0; k < static_cast<int>(t.schema().fields.size()); ++k)
                    f.value(k) = t.value(k);
                for (const int l : { X::file::UserParamDefs, X::file::AssetParamDefs, X::file::TriggerParamDefs,
                         X::file::LocalPropertyNames, X::file::LocalPropertyEnumNames })
                    for (const auto& n : t.list(l))
                        f.list(l).append(n->clone());
                collect_target_habits();
            }

            Document run(const std::vector<int>& which) {
                const NodeList& users = users_of(*m_src.root);
                for (int k = 0; k < users.size(); ++k)
                    if (which.empty() || std::find(which.begin(), which.end(), k) != which.end())
                        user(users.at(k));
                finish();
                return std::move(m_x);
            }

        private:
            void collect_target_habits() {
                const Node& t = *m_doc.root;
                std::map<std::string, int> slots;
                std::vector<const Node*> plain, overwritten;
                for (const auto& u : t.list(X::file::Users)) {
                    for (const auto& s : u->list(X::user::ActionSlots))
                        ++slots[s->s(X::slot::Name)];
                    for (const int l : { X::user::ActionTriggers, X::user::PropertyTriggers, X::user::AlwaysTriggers })
                        for (const auto& tr : u->list(l)) {
                            const int ow = l == X::user::ActionTriggers ? int { X::atrig::Overwrite }
                                : l == X::user::PropertyTriggers        ? int { X::ptrig::Overwrite }
                                                                        : int { X::awtrig::Overwrite };
                            const int hash = l == X::user::ActionTriggers ? int { X::atrig::OverwriteHash }
                                : l == X::user::PropertyTriggers          ? int { X::ptrig::OverwriteHash }
                                                                          : int { X::awtrig::OverwriteHash };
                            (tr->i(ow) < 0 ? plain : overwritten).push_back(tr.get());
                            if (tr->i(ow) >= 0 && tr->i(ow) < t.list(X::file::TriggerParamSets).size())
                                m_overwrite_hashes.emplace_back(
                                    &t.list(X::file::TriggerParamSets).at(static_cast<int>(tr->i(ow))), tr->i(hash));
                        }
                }
                // The hash a trigger stores beside its overwrite set follows the set;
                // its scheme is unknown, so new triggers reuse what the database pairs
                // with an identical set, or its most common value.
                m_plain_hash = 0;
                if (!plain.empty()) {
                    std::map<s64, int> c;
                    for (const Node* n : plain) {
                        const int f = &n->schema() == &X::schemas().atrig ? int { X::atrig::OverwriteHash }
                            : &n->schema() == &X::schemas().ptrig         ? int { X::ptrig::OverwriteHash }
                                                                          : int { X::awtrig::OverwriteHash };
                        ++c[n->i(f)];
                    }
                    m_plain_hash = std::max_element(c.begin(), c.end(), [](auto& a, auto& b) {
                        return a.second < b.second;
                    })->first;
                }
                m_slot = m_opt.action_slot;
            }

            const NodeList& defs(int list) const { return m_x.root->list(list); }

            // A parameter for definition `name`, or null when the target has none.
            std::unique_ptr<Node> param(
                int def_list, std::string_view name, const Value& v, bool is_string, bool random, f32 lo, f32 hi) {
                const int d = def_index(defs(def_list), name);
                if (d < 0) {
                    ++m_missing_defs[std::string(name)];
                    return nullptr;
                }
                auto p = std::make_unique<Node>(&X::schemas().param);
                p->set_i(X::param::Def, d);
                p->set_i(X::param::DirectHint, -1);
                const s64 type = defs(def_list).at(d).i(X::pdef::Type);
                if (is_string || type == X::PT_String) {
                    p->set_i(X::param::RefType, X::RT_String);
                    p->set_s(X::param::Value, v.s);
                } else if (random) {
                    Node& r = m_x.root->list(X::file::Randoms).append_new();
                    r.set_i(X::random::Min, f32_bits(lo));
                    r.set_i(X::random::Max, f32_bits(hi));
                    p->set_i(X::param::RefType, X::RT_Random);
                    p->set_i(X::param::Value, r.row());
                } else {
                    p->set_i(
                        X::param::RefType, name == "BitFlag" || type == X::PT_Bitfield ? X::RT_Bitflag : X::RT_Direct);
                    p->set_i(X::param::Value, v.i);
                }
                return p;
            }

            // Whether a value equals the definition's default, which a set leaves out.
            bool is_default(int def_list, std::string_view name, const Value& v) const {
                const int d = def_index(defs(def_list), name);
                if (d < 0)
                    return false;
                const Node& def = defs(def_list).at(d);
                switch (def.i(X::pdef::Type)) {
                case X::PT_String:
                    return def.s(X::pdef::Default) == v.s;
                case X::PT_Float:
                    return f32_bits(static_cast<f32>(def.f64v(X::pdef::Default))) == static_cast<u32>(v.i);
                default:
                    return def.i(X::pdef::Default) == v.i;
                }
            }

            struct Item {
                std::string def;
                Value value;
                bool string = false;
                bool always = false;
                bool random = false;
                f32 lo = 0, hi = 0;
            };

            static Item num(std::string def, s64 v) { return { std::move(def), Value { v, { } } }; }
            static Item flt(std::string def, f32 v) { return { std::move(def), Value { f32_bits(v), { } } }; }
            static Item range(std::string def, f32 v, f32 max) {
                Item it = flt(std::move(def), v);
                if (max > v) {
                    it.random = true;
                    it.lo = v;
                    it.hi = max;
                }
                return it;
            }
            static Item str(std::string def, std::string v, bool always = false) {
                Item it { std::move(def), Value { 0, std::move(v) } };
                it.string = true;
                it.always = always;
                return it;
            }

            // A parameter set holding the items that differ from their defaults, in
            // definition order; returns its index or -1 when empty.
            int param_set(int def_list, int set_list, const std::vector<Item>& items) {
                std::vector<std::unique_ptr<Node>> ps;
                for (const Item& it : items) {
                    if (!it.always && !it.random && is_default(def_list, it.def, it.value))
                        continue;
                    if (auto p = param(def_list, it.def, it.value, it.string, it.random, it.lo, it.hi))
                        ps.push_back(std::move(p));
                }
                if (ps.empty())
                    return -1;
                std::sort(
                    ps.begin(), ps.end(), [](auto& a, auto& b) { return a->i(X::param::Def) < b->i(X::param::Def); });
                Node& set = m_x.root->list(set_list).append_new();
                for (auto& p : ps)
                    set.list(X::pset::Params).append(std::move(p));
                return set.row();
            }

            int enum_name(const std::string& v) { return local_name(X::file::LocalPropertyEnumNames, v); }
            int prop_name(const std::string& v) { return local_name(X::file::LocalPropertyNames, v); }
            int local_name(int list, const std::string& v) {
                NodeList& l = m_x.root->list(list);
                for (int k = 0; k < l.size(); ++k)
                    if (l.at(k).s(X::name::Name) == v)
                        return k;
                l.append_new().set_s(X::name::Name, v);
                return l.size() - 1;
            }

            // ---------------------------------------------------------------- users

            void user(const Node& u) {
                const s64 type = u.i(D::user::Type);
                if (type == D::UT_Empty) {
                    ++m_skipped_empty;
                    return;
                }
                Node& x = users_of(*m_x.root).append(make_element(m_x, users_of(*m_x.root)));
                const std::string name = u.s(D::user::Name);
                x.set_s(X::user::Name, name);
                x.set_i(X::user::Hash, crc32(name));
                GuidSource guids(name);
                m_user_locals.clear();

                if (m_sound) {
                    const f32 interior = u.f32v(D::user::Header3);
                    const int d = def_index(defs(X::file::UserParamDefs), "InteriorSize");
                    if (interior != 0 && d >= 0)
                        for (auto& p : x.list(X::user::UserParams))
                            if (p->i(X::param::Def) == d)
                                p->set_i(X::param::Value, f32_bits(interior));
                    if (u.i(D::user::Header1) || u.i(D::user::Header2) || u.i(D::user::Header4))
                        ++m_distance_users;
                }
                Unmapped& um = m_unmapped;
                um.note(u,
                    { D::user::Name, D::user::BaseName, D::user::Type, D::user::PooledAssets, D::user::PooledTriggers,
                        D::user::ParamsPerAsset, D::user::Header3, D::user::Header1, D::user::Header2,
                        D::user::Header4 });

                // Calls: one per call table, in order, then container children.
                const NodeList& cts = u.list(D::user::CallTables);
                const NodeList& assets = u.list(D::user::Assets);
                const int ct_name = field_of(m_fs.call.schema, "Name");
                const int ct_start = field_of(m_fs.call.schema, m_sound ? "AssetStart" : "EffectStart");
                const int ct_end = ct_start + 1;
                const int ct_random = field_of(m_fs.call.schema, "Flags2");
                NodeList& calls = x.list(X::user::Calls);
                struct Pending {
                    int call;
                    std::string key;
                    // The call table's name; leaves are numbered after it.
                    std::string top;
                    Plan plan;
                    bool random;
                    std::vector<int> random_effects;
                };
                std::map<std::string, int> leaves;
                const auto leaf_key = [&](const std::string& top) { return top + "_" + two_digits(leaves[top]++); };
                std::vector<Pending> queue;
                for (const auto& ct : cts) {
                    Node& c = calls.append(make_element(m_x, calls));
                    const std::string key = ct->s(ct_name);
                    c.set_s(X::call::KeyName, key);
                    um.note(*ct, { ct_name, ct_start, ct_end, m_sound ? ct_random : -1 });
                    const s64 s = ct->i(ct_start), e = ct->i(ct_end);
                    std::vector<int> range;
                    for (s64 k = s; k <= e && k < assets.size(); ++k)
                        if (k >= 0)
                            range.push_back(static_cast<int>(k));
                    Pending pd { c.row(), key, key, { }, false, { } };
                    if (m_sound) {
                        if ((ct->i(ct_random) & 1) && range.size() > 1) {
                            pd.random = true;
                            pd.random_effects = range;
                        } else if (!range.empty()) {
                            pd.plan.kind = Plan::Leaf;
                            pd.plan.effect = range.front();
                            m_dropped_assets += range.size() - 1;
                        }
                    } else {
                        std::vector<Candidate> cands;
                        for (const int k : range) {
                            const Node& a = assets.at(k);
                            cands.push_back({ k, { a.s(m_c1), a.s(m_c2), a.s(m_c3) } });
                        }
                        pd.plan = plan_for(cands, 0);
                    }
                    queue.push_back(std::move(pd));
                }
                for (std::size_t q = 0; q < queue.size(); ++q) {
                    Pending pd = queue[q];
                    Node& c = calls.at(pd.call);
                    if (pd.random) {
                        make_container(x, c, X::CT_Random);
                        const int first = calls.size();
                        for (const int e : pd.random_effects) {
                            Node& child = calls.append(make_element(m_x, calls));
                            child.set_s(X::call::KeyName, leaf_key(pd.top));
                            child.set_i(X::call::ParentIndex, pd.call);
                            Node& cond = m_x.root->list(X::file::Conditions)
                                             .append(make_element(m_x, m_x.root->list(X::file::Conditions)));
                            cond.set_i(X::cond::ParentType, X::CT_Random);
                            cond.set_i(X::cond::Weight, f32_bits(1.0f));
                            child.set_i(X::call::Condition, cond.row());
                            asset_call(child, assets.at(e));
                        }
                        set_children(x, c, first, calls.size() - 1);
                        continue;
                    }
                    switch (pd.plan.kind) {
                    case Plan::Empty:
                        ++m_empty_calls;
                        c.set_i(X::call::ParamSet,
                            param_set(
                                X::file::AssetParamDefs, X::file::AssetParamSets, { str("AssetName", pd.key, true) }));
                        break;
                    case Plan::Leaf:
                        asset_call(c, assets.at(pd.plan.effect));
                        break;
                    case Plan::Switch: {
                        const std::string& prop = m_opt.conditions[static_cast<std::size_t>(pd.plan.level)];
                        make_container(x, c, X::CT_Switch);
                        Node& ctn = x.list(X::user::Containers).at(static_cast<int>(c.i(X::call::Container)));
                        ctn.set_s(X::ctn::WatchPropertyName, prop);
                        ctn.set_i(X::ctn::LocalPropertyNameIdx, prop_name(prop));
                        if (std::find(m_user_locals.begin(), m_user_locals.end(), prop) == m_user_locals.end())
                            m_user_locals.push_back(prop);
                        const int first = calls.size();
                        for (std::size_t ci = 0; ci < pd.plan.plans.size(); ++ci) {
                            const std::string& value = pd.plan.values[ci];
                            const Plan& sub = pd.plan.plans[ci];
                            Node& child = calls.append(make_element(m_x, calls));
                            child.set_i(X::call::ParentIndex, pd.call);
                            const bool leaf = sub.kind == Plan::Leaf;
                            const std::string key = value.empty() ? pd.key + "_Default"
                                : leaf                            ? leaf_key(pd.top)
                                                                  : pd.key + "_" + value;
                            child.set_s(X::call::KeyName, key);
                            if (!value.empty()) {
                                Node& cond = m_x.root->list(X::file::Conditions)
                                                 .append(make_element(m_x, m_x.root->list(X::file::Conditions)));
                                cond.set_i(X::cond::ParentType, X::CT_Switch);
                                cond.set_i(X::cond::PropertyType, X::PR_Enum);
                                cond.set_s(X::cond::Value, value);
                                cond.set_i(X::cond::LocalPropertyEnumNameIdx, enum_name(value));
                                child.set_i(X::call::Condition, cond.row());
                            }
                            if (leaf)
                                asset_call(child, assets.at(sub.effect));
                            else
                                queue.push_back({ child.row(), key, pd.top, sub, false, { } });
                        }
                        set_children(x, c, first, calls.size() - 1);
                        break;
                    }
                    }
                }
                s64 asset_id = 0;
                for (auto& c : calls) {
                    c->set_i(X::call::KeyNameHash, crc32(c->s(X::call::KeyName)));
                    c->set_i(X::call::Guid, guids.next("call:" + c->s(X::call::KeyName)));
                    c->set_i(X::call::AssetId, X::call_is_container(c->i(X::call::Flag)) ? -1 : asset_id++);
                }
                if (xlink_modern::has_user_locals(m_doc.version(), xlink_system(m_doc.format)))
                    for (const auto& l : m_user_locals)
                        x.list(X::user::LocalProperties).append_new().set_s(X::name::Name, l);

                actions(u, x, guids, cts.size());
            }

            static std::string two_digits(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }

            void make_container(Node& user, Node& call, s64 type) {
                NodeList& ctns = user.list(X::user::Containers);
                Node& ctn = ctns.append(make_element(m_x, ctns));
                ctn.set_i(X::ctn::Type, type);
                call.set_i(X::call::Flag, call.i(X::call::Flag) | 1);
                call.set_i(X::call::Container, ctn.row());
                call.set_i(X::call::AssetId, -1);
            }

            static void set_children(Node& user, Node& call, int first, int last) {
                Node& ctn = user.list(X::user::Containers).at(static_cast<int>(call.i(X::call::Container)));
                ctn.set_i(X::ctn::ChildStart, first);
                ctn.set_i(X::ctn::ChildEnd, last);
            }

            void asset_call(Node& call, const Node& a) {
                const std::string key = call.s(X::call::KeyName);
                std::vector<Item> items { str("AssetName", key, true) };
                std::set<int> used;
                const auto F = [&](std::string_view n) {
                    const int f = field_of(a.schema(), n);
                    used.insert(f);
                    return f;
                };
                if (m_sound) {
                    items.push_back(str("RuntimeAssetName", a.s(F("Label")), true));
                    for (const char* p : { "Volume", "Pitch", "Lpf", "Pan", "SurroundPan" }) {
                        const int f = a.schema().field_index(p);
                        if (f < 0)
                            continue;
                        const int fmax = F(std::string(p) + "Max");
                        used.insert(f);
                        items.push_back(range(p, a.f32v(f), a.f32v(fmax)));
                    }
                    const int dmin = F("DelayMin"), dmax = F("DelayMax");
                    items.push_back(range("Delay", static_cast<f32>(a.i(dmin)), static_cast<f32>(a.i(dmax))));
                    items.push_back(str("Bone", a.s(F("Bone"))));
                    F("Id");
                    F("LabelId");
                } else {
                    items.push_back(str("RuntimeAssetName", a.s(F("EmitterSet")), true));
                    items.push_back(str("Bone", a.s(F("Bone"))));
                    items.push_back(str("EffectGroup", a.s(F("Group"))));
                    const s64 matrix = a.i(F("Field0x30"));
                    const int b31 = field_of(a.schema(), "Field0x31");
                    const s64 follow_off = a.i(b31);
                    // Without a matrix mode the byte changes nothing ELink2 can hold.
                    if (matrix != 0)
                        used.insert(b31);
                    items.push_back(num("Matrix", matrix));
                    items.push_back(num("RotateSource", a.i(F("Field0x32"))));
                    items.push_back(num("Clip", a.i(F("Field0x37")) + 1));
                    items.push_back(num("ForceCalc", a.i(F("Field0x38"))));
                    // As Mario Kart 8 Deluxe ported Mario Kart 8's effects: they follow
                    // their user unless the byte at 0x31 is set together with a matrix mode.
                    items.push_back(num("BitFlag", matrix != 0 && follow_off != 0 ? 0 : 2));
                    for (const char* p : { "Scale", "PositionX", "PositionY", "PositionZ", "RotationX", "RotationY",
                             "RotationZ", "Red", "Green", "Blue", "Alpha" })
                        items.push_back(flt(p, a.f32v(F(p))));
                    F("Id");
                    F(m_c1n);
                    F(m_c2n);
                    F(m_c3n);
                    if (!a.list(D::slot_params::Params).empty())
                        ++m_param_words;
                }
                m_unmapped.note(a, used);
                call.set_i(X::call::ParamSet, param_set(X::file::AssetParamDefs, X::file::AssetParamSets, items));
            }

            void actions(const Node& u, Node& x, GuidSource& guids, int n_calls) {
                const NodeList& acts = u.list(D::user::Actions);
                if (acts.empty())
                    return;
                const NodeList& trigs = u.list(D::user::Triggers);
                const Schema& ts = m_fs.trigger.schema;
                const int t_start = field_of(ts, "StartFrame"), t_end = field_of(ts, "EndFrame");
                const int t_call = m_sound ? field_of(ts, "Call") : field_of(ts, "CallName");
                const int t_flags = field_of(ts, "Flags0");
                const int a_name = field_of(m_fs.action.schema, "Name");
                const int a_start = field_of(m_fs.action.schema, "TriggerStart");
                NodeList& slots = x.list(X::user::ActionSlots);
                NodeList& xa = x.list(X::user::Actions);
                NodeList& xt = x.list(X::user::ActionTriggers);
                Node& slot = slots.append(make_element(m_x, slots));
                slot.set_s(X::slot::Name, m_slot);
                const NodeList& calls = x.list(X::user::Calls);
                for (const auto& a : acts) {
                    Node& na = xa.append(make_element(m_x, xa));
                    na.set_s(X::action::Name, a->s(a_name));
                    m_unmapped.note(*a, { a_name, a_start, a_start + 1 });
                    const int first = xt.size();
                    for (s64 k = a->i(a_start); k <= a->i(a_start + 1) && k < trigs.size(); ++k) {
                        if (k < 0)
                            continue;
                        const Node& t = trigs.at(static_cast<int>(k));
                        std::set<int> used { t_start, t_end, t_call, t_flags, field_of(ts, "Id") };
                        s64 call = -1;
                        if (m_sound) {
                            call = t.i(t_call) >= 0 && t.i(t_call) < n_calls ? t.i(t_call) : -1;
                        } else {
                            for (int c = 0; c < n_calls; ++c)
                                if (calls.at(c).s(X::call::KeyName) == t.s(t_call)) {
                                    call = c;
                                    break;
                                }
                        }
                        if (call < 0) {
                            ++m_unresolved_triggers;
                            continue;
                        }
                        Node& nt = xt.append(make_element(m_x, xt));
                        nt.set_i(X::atrig::Guid, guids.next("trigger:" + std::to_string(xt.size())));
                        nt.set_i(X::atrig::AssetCall, call);
                        s64 start = t.i(t_start), end = t.i(t_end);
                        if (end < 0)
                            end = Forever;
                        nt.set_i(X::atrig::StartFrame, start);
                        nt.set_i(X::atrig::EndFrame, end);
                        if (start == 0 && end == Forever)
                            nt.set_i(X::atrig::Flag, 0x4);
                        std::vector<Item> items;
                        if (m_sound) {
                            for (const char* p : { "Volume", "Pitch", "Lpf" }) {
                                const int f = field_of(ts, p), fmax = field_of(ts, std::string(p) + "Max");
                                used.insert(f);
                                used.insert(fmax);
                                items.push_back(range(p, t.f32v(f), t.f32v(fmax)));
                            }
                            const int dmin = field_of(ts, "DelayMin");
                            used.insert(dmin);
                            used.insert(dmin + 1);
                            items.push_back(
                                range("Delay", static_cast<f32>(t.i(dmin)), static_cast<f32>(t.i(dmin + 1))));
                        } else {
                            // Flags0 selects what the trigger overrides: bit 0 the bone, then
                            // scale, position and rotation.
                            const s64 mask = t.i(t_flags);
                            const char* names[] = { "Scale", "PositionX", "PositionY", "PositionZ", "RotationX",
                                "RotationY", "RotationZ" };
                            const int bone = field_of(ts, "Bone");
                            used.insert(bone);
                            // An overridden value replaces the effect's even when it equals
                            // the default, so it stays in the set.
                            if (mask & 1)
                                items.push_back(str("Bone", t.s(bone), true));
                            for (int b = 0; b < 7; ++b) {
                                const int f = field_of(ts, names[b]);
                                used.insert(f);
                                if (mask >> (b + 1) & 1) {
                                    Item it = flt(names[b], t.f32v(f));
                                    it.always = true;
                                    items.push_back(it);
                                }
                            }
                            if (t.i(field_of(ts, "HandoverId")))
                                ++m_handovers;
                            used.insert(field_of(ts, "HandoverId"));
                        }
                        m_unmapped.note(t, used);
                        const int set = param_set(X::file::TriggerParamDefs, X::file::TriggerParamSets, items);
                        nt.set_i(X::atrig::Overwrite, set);
                        nt.set_i(X::atrig::OverwriteHash, overwrite_hash(set));
                    }
                    na.set_i(X::action::TriggerStart, first);
                    na.set_i(X::action::TriggerEnd, xt.size() - 1);
                }
                slot.set_i(X::slot::ActionStart, 0);
                slot.set_i(X::slot::ActionEnd, xa.size() - 1);
            }

            s64 overwrite_hash(int set) {
                if (set < 0)
                    return m_plain_hash;
                const Node& mine = m_x.root->list(X::file::TriggerParamSets).at(set);
                for (const auto& [n, h] : m_overwrite_hashes)
                    if (xml::same_content(*n, mine))
                        return h;
                ++m_guessed_hashes;
                std::map<s64, int> c;
                for (const auto& [n, h] : m_overwrite_hashes)
                    ++c[h];
                if (c.empty())
                    return m_plain_hash;
                return std::max_element(c.begin(), c.end(), [](auto& a, auto& b) {
                    return a.second < b.second;
                })->first;
            }

            void finish() {
                const std::string target = "XLink " + std::string(m_sound ? "SLink2" : "ELink2");
                m_unmapped.report(m_warnings, target);
                for (const auto& [name, n] : m_missing_defs)
                    m_warnings.push_back(
                        count_text(n, "values for parameter " + name + ", which this database does not define"));
                if (m_skipped_empty)
                    m_warnings.push_back(count_text(m_skipped_empty, "empty users skipped"));
                if (m_dropped_assets)
                    m_warnings.push_back(count_text(m_dropped_assets,
                        "sound assets after the first of a call table that does not "
                        "pick at random dropped; such a call plays its first asset"));
                if (m_empty_calls)
                    m_warnings.push_back(
                        count_text(m_empty_calls, "call tables without assets become calls naming no asset"));
                if (m_distance_users)
                    m_warnings.push_back(count_text(m_distance_users,
                        "users' MaxVolumeDistance, UnitDistance or DistanceFlags "
                        "dropped; SLink2 games keep distances in distance parameter "
                        "sets the DistanceParamSetName user parameter names"));
                if (m_param_words)
                    m_warnings.push_back(count_text(m_param_words, "effects' game-specific parameter words dropped"));
                if (m_handovers)
                    m_warnings.push_back(count_text(m_handovers,
                        "triggers hand their emitter over to the next action, which XLink "
                        "triggers cannot; the effect starts again instead"));
                if (m_unresolved_triggers)
                    m_warnings.push_back(
                        count_text(m_unresolved_triggers, "triggers name no call table of their user and are dropped"));
                if (m_guessed_hashes)
                    m_warnings.push_back(count_text(m_guessed_hashes,
                        "trigger overwrite sets are new to this database; their "
                        "OverwriteHash is its most common one"));
            }

            const Document& m_doc;
            const Document& m_src;
            const BridgeOptions& m_opt;
            std::vector<std::string>& m_warnings;
            bool m_sound;
            const D::FamilySchemas& m_fs;
            Document m_x;
            std::string m_slot;
            s64 m_plain_hash = 0;
            std::vector<std::pair<const Node*, s64>> m_overwrite_hashes;
            std::vector<std::string> m_user_locals;
            Unmapped m_unmapped;
            std::map<std::string, std::size_t> m_missing_defs;
            std::size_t m_skipped_empty = 0, m_dropped_assets = 0, m_empty_calls = 0, m_distance_users = 0,
                        m_param_words = 0, m_handovers = 0, m_unresolved_triggers = 0, m_guessed_hashes = 0;
            const char* m_c1n = "Condition1";
            const char* m_c2n = "Condition2";
            const char* m_c3n = "Condition3";
            int m_c1 = field_of(D::effect_link().asset.schema, "Condition1");
            int m_c2 = field_of(D::effect_link().asset.schema, "Condition2");
            int m_c3 = field_of(D::effect_link().asset.schema, "Condition3");
        };

        // ------------------------------------------------------------ XLink to old

        // Values of one parameter set read by definition name, defaults filled in.
        class ParamReader {
        public:
            ParamReader(const Node& file, int def_list, s64 set_list, s64 set)
                : m_file(file)
                , m_defs(file.list(def_list)) {
                if (set >= 0 && set < file.list(static_cast<int>(set_list)).size())
                    m_set = &file.list(static_cast<int>(set_list)).at(static_cast<int>(set));
            }

            const Node* find(std::string_view name) const {
                const int d = def_index(m_defs, name);
                if (d < 0 || !m_set)
                    return nullptr;
                for (const auto& p : m_set->list(X::pset::Params))
                    if (p->i(X::param::Def) == d)
                        return p.get();
                return nullptr;
            }
            std::string str(std::string_view name) const {
                if (const Node* p = find(name))
                    return p->s(X::param::Value);
                const int d = def_index(m_defs, name);
                return d < 0 ? std::string() : m_defs.at(d).s(X::pdef::Default);
            }
            // A number's value and, for a random range, its top.
            std::pair<f32, f32> f(std::string_view name) const {
                if (const Node* p = find(name)) {
                    const s64 rt = p->i(X::param::RefType);
                    const s64 v = p->i(X::param::Value);
                    if (X::is_random_ref(rt) && v >= 0 && v < m_file.list(X::file::Randoms).size()) {
                        const Node& r = m_file.list(X::file::Randoms).at(static_cast<int>(v));
                        return { bits_f32(static_cast<u32>(r.i(X::random::Min))),
                            bits_f32(static_cast<u32>(r.i(X::random::Max))) };
                    }
                    if (rt == X::RT_Curve || rt == X::RT_ArrangeGroup) {
                        ++curves;
                        return { def_f(name), 0 };
                    }
                    return { bits_f32(static_cast<u32>(v)), 0 };
                }
                return { def_f(name), 0 };
            }
            s64 i(std::string_view name) const {
                if (const Node* p = find(name)) {
                    const s64 rt = p->i(X::param::RefType);
                    if (rt == X::RT_Direct || rt == X::RT_Bitflag)
                        return static_cast<s32>(p->i(X::param::Value));
                    ++curves;
                }
                const int d = def_index(m_defs, name);
                return d < 0 ? 0 : m_defs.at(d).i(X::pdef::Default);
            }
            bool has(std::string_view name) const { return find(name) != nullptr; }

            mutable std::size_t curves = 0;

        private:
            f32 def_f(std::string_view name) const {
                const int d = def_index(m_defs, name);
                return d < 0 ? 0.0f : static_cast<f32>(m_defs.at(d).f64v(X::pdef::Default));
            }

            const Node& m_file;
            const NodeList& m_defs;
            const Node* m_set = nullptr;
        };

        class FromXLink {
        public:
            FromXLink(
                const Document& doc, const Document& src, const BridgeOptions& opt, std::vector<std::string>& warnings)
                : m_doc(doc)
                , m_src(src)
                , m_opt(opt)
                , m_warnings(warnings)
                , m_sound(doc.format == Format::SLink)
                , m_fs(m_sound ? D::sound_link(plain_version(doc.format)) : D::effect_link()) {
                m_o.format = doc.format;
                m_o.endian = doc.endian;
                m_o.encoding = doc.encoding;
                m_o.root = std::make_unique<Node>(&m_fs.file);
                m_o.root->set_i(D::file::Version, plain_version(doc.format));
                collect_target_habits();
            }

            Document run(const std::vector<int>& which) {
                const NodeList& users = users_of(*m_src.root);
                for (int k = 0; k < users.size(); ++k)
                    if (which.empty() || std::find(which.begin(), which.end(), k) != which.end())
                        user(users.at(k));
                finish();
                return std::move(m_o);
            }

        private:
            // Fields with no XLink counterpart take the value the target's records use most.
            void collect_target_habits() {
                const D::FamilySchemas* tfs = D::family_of(*m_doc.root);
                std::vector<const Node*> users, assets, trigs, calls;
                for (const auto& u : users_of(*m_doc.root)) {
                    users.push_back(u.get());
                    for (const auto& c : u->list(D::user::CallTables))
                        calls.push_back(c.get());
                    for (const auto& a : *D::asset_list_of(*u))
                        if (&a->schema() == &tfs->asset.schema)
                            assets.push_back(a.get());
                    for (const auto& t : *D::trigger_list_of(*u))
                        if (&t->schema() == &tfs->trigger.schema)
                            trigs.push_back(t.get());
                }
                for (const auto& a : m_doc.root->list(D::file::SharedAssets))
                    assets.push_back(a.get());
                for (const auto& t : m_doc.root->list(D::file::SharedTriggers))
                    trigs.push_back(t.get());
                const auto habits = [](const Schema& mine, const Schema& theirs, const std::vector<const Node*>& nodes,
                                        std::map<int, s64>& out) {
                    for (int f = 0; f < static_cast<int>(mine.fields.size()); ++f) {
                        const FieldDef& fd = mine.fields[f];
                        if (fd.hidden || fd.type == FieldType::String || fd.type == FieldType::RangeStart
                            || fd.type == FieldType::RangeEnd || fd.type == FieldType::Ref)
                            continue;
                        const int tf = theirs.field_index(fd.name);
                        if (tf >= 0)
                            out[f] = most_common(nodes, tf, 0);
                    }
                };
                habits(m_fs.asset.schema, tfs->asset.schema, assets, m_asset_habit);
                habits(m_fs.trigger.schema, tfs->trigger.schema, trigs, m_trigger_habit);
                habits(m_fs.call.schema, tfs->call.schema, calls, m_call_habit);
                m_kind = most_common(users, D::user::Kind, 0);
                for (const Node* u : users)
                    m_names.emplace(crc32(u->s(D::user::Name)), u->s(D::user::Name));
                if (!m_sound) {
                    m_words_per_asset = most_common(users, D::user::ParamsPerAsset, 0);
                    // The parameter words effects of that many words carry most often.
                    std::map<std::vector<s64>, int> c;
                    for (const Node* a : assets)
                        if (a->list(D::slot_params::Params).size() == m_words_per_asset) {
                            std::vector<s64> w;
                            for (const auto& p : a->list(D::slot_params::Params))
                                w.push_back(p->i(D::asset_param::Value));
                            ++c[w];
                        }
                    for (const auto& u : users)
                        for (const auto& r : u->list(D::user::AssetRefs))
                            if (r->list(D::slot_params::Params).size() == m_words_per_asset) {
                                std::vector<s64> w;
                                for (const auto& p : r->list(D::slot_params::Params))
                                    w.push_back(p->i(D::asset_param::Value));
                                ++c[w];
                            }
                    if (!c.empty())
                        m_words = std::max_element(c.begin(), c.end(), [](auto& a, auto& b) {
                            return a.second < b.second;
                        })->first;
                    else
                        m_words.assign(static_cast<std::size_t>(m_words_per_asset), 0);
                }
            }

            void apply_habits(Node& n, const std::map<int, s64>& habit, const std::set<int>& set) {
                for (const auto& [f, v] : habit)
                    if (!set.contains(f))
                        n.set_i(f, v);
            }

            struct Leaf {
                int call;
                std::array<std::string, 3> conditions;
            };

            // The asset calls a call reaches, with the conditions on the way there.
            void flatten(const Node& user, int call, std::array<std::string, 3> conds, std::vector<Leaf>& out, bool top,
                bool& random) {
                const NodeList& calls = user.list(X::user::Calls);
                const Node& c = calls.at(call);
                if (!X::call_is_container(c.i(X::call::Flag))) {
                    out.push_back({ call, conds });
                    return;
                }
                const s64 ci = c.i(X::call::Container);
                if (ci < 0 || ci >= user.list(X::user::Containers).size())
                    return;
                const Node& ctn = user.list(X::user::Containers).at(static_cast<int>(ci));
                const s64 first = ctn.i(X::ctn::ChildStart), last = ctn.i(X::ctn::ChildEnd);
                std::vector<int> kids;
                for (s64 k = first; k <= last && k < calls.size(); ++k)
                    if (k >= 0)
                        kids.push_back(static_cast<int>(k));
                if (kids.empty())
                    return;
                const s64 type = ctn.i(X::ctn::Type);
                const NodeList& cnds = m_src.root->list(X::file::Conditions);
                const auto cond_of = [&](int k) -> const Node* {
                    const s64 i = calls.at(k).i(X::call::Condition);
                    return i >= 0 && i < cnds.size() ? &cnds.at(static_cast<int>(i)) : nullptr;
                };
                if (type == X::CT_Switch && ctn.i(X::ctn::SwitchByte) == 0) {
                    const std::string& prop = ctn.s(X::ctn::WatchPropertyName);
                    int slot = -1;
                    if (!m_sound)
                        for (int s = 0; s < 3; ++s)
                            if (!m_opt.conditions[static_cast<std::size_t>(s)].empty()
                                && m_opt.conditions[static_cast<std::size_t>(s)] == prop)
                                slot = s;
                    bool plain = slot >= 0 && conds[static_cast<std::size_t>(slot)].empty();
                    for (const int k : kids)
                        if (const Node* cd = cond_of(k))
                            plain = plain && cd->i(X::cond::PropertyType) == X::PR_Enum
                                && cd->i(X::cond::CompareType) == 0;
                    if (plain) {
                        for (const int k : kids) {
                            auto sub = conds;
                            if (const Node* cd = cond_of(k))
                                sub[static_cast<std::size_t>(slot)] = cd->s(X::cond::Value);
                            flatten(user, k, sub, out, false, random);
                        }
                        return;
                    }
                }
                if ((type == X::CT_Random || type == X::CT_Random2) && m_sound && top) {
                    bool leaves = true;
                    for (const int k : kids)
                        leaves = leaves && !X::call_is_container(calls.at(k).i(X::call::Flag));
                    if (leaves) {
                        random = true;
                        for (const int k : kids)
                            out.push_back({ k, conds });
                        return;
                    }
                }
                // Only the first child survives a container the older format cannot state.
                ++m_reduced[X::ContainerTypeNames.at(static_cast<std::size_t>(std::clamp<s64>(type, 0, X::CT_Grid)))];
                flatten(user, kids.front(), conds, out, false, random);
            }

            void user(const Node& u) {
                Node& o = users_of(*m_o.root).append(make_element(m_o, users_of(*m_o.root)));
                std::string name = u.s(X::user::Name);
                if (name.empty()) {
                    // A user the target already has is found by the hash of its name.
                    const auto it = m_names.find(static_cast<u32>(u.i(X::user::Hash)));
                    if (it != m_names.end()) {
                        name = it->second;
                    } else {
                        name = from_utf8(hex(static_cast<u32>(u.i(X::user::Hash)), 8), m_o.encoding);
                        ++m_unnamed;
                    }
                }
                o.set_s(D::user::Name, name);
                o.set_i(D::user::Type, D::UT_User);
                o.set_i(D::user::Kind, m_kind);
                o.set_i(D::user::OwnStrings, 1);
                if (!m_sound)
                    o.set_i(D::user::ParamsPerAsset, m_words_per_asset);
                const Node& file = *m_src.root;
                if (m_sound) {
                    const int d = def_index(file.list(X::file::UserParamDefs), "InteriorSize");
                    for (const auto& p : u.list(X::user::UserParams))
                        if (p->i(X::param::Def) == d)
                            o.set_i(D::user::Header3, p->i(X::param::Value));
                }

                const NodeList& calls = u.list(X::user::Calls);
                std::vector<bool> child(static_cast<std::size_t>(calls.size()), false);
                for (const auto& c : u.list(X::user::Containers))
                    for (s64 k = c->i(X::ctn::ChildStart); k <= c->i(X::ctn::ChildEnd) && k < calls.size(); ++k)
                        if (k >= 0)
                            child[static_cast<std::size_t>(k)] = true;
                std::map<int, int> table_of;
                for (int k = 0; k < calls.size(); ++k)
                    if (!child[static_cast<std::size_t>(k)])
                        call_table(u, o, k, table_of);

                // Actions of every slot, in slot order.
                NodeList& oa = o.list(D::user::Actions);
                NodeList& ot = o.list(D::user::Triggers);
                const Schema& as = m_fs.action.schema;
                const Schema& ts = m_fs.trigger.schema;
                const int a_name = field_of(as, "Name"), a_start = field_of(as, "TriggerStart");
                const NodeList& xa = u.list(X::user::Actions);
                const NodeList& xt = u.list(X::user::ActionTriggers);
                for (const auto& a : xa) {
                    Node& na = oa.append(make_element(m_o, oa));
                    na.set_s(a_name, a->s(X::action::Name));
                    const int first = ot.size();
                    for (s64 k = a->i(X::action::TriggerStart); k <= a->i(X::action::TriggerEnd) && k < xt.size();
                        ++k) {
                        if (k < 0)
                            continue;
                        const Node& t = xt.at(static_cast<int>(k));
                        const s64 flag = t.i(X::atrig::Flag);
                        if (flag & 0x18) {
                            ++m_dropped_triggers["on leaving or after another action"];
                            continue;
                        }
                        const s64 call = t.i(X::atrig::AssetCall);
                        if (call < 0 || call >= calls.size())
                            continue;
                        const int table = ensure_table(u, o, static_cast<int>(call), table_of);
                        Node& nt = ot.append(make_element(m_o, ot));
                        std::set<int> set;
                        const auto put = [&](std::string_view f, s64 v) {
                            const int i = field_of(ts, f);
                            nt.set_i(i, v);
                            set.insert(i);
                        };
                        put("Id", ot.size() - 1);
                        if (m_sound) {
                            put("Call", table);
                        } else {
                            const int f = field_of(ts, "CallName");
                            nt.set_s(f, o.list(D::user::CallTables).at(table).s(field_of(m_fs.call.schema, "Name")));
                            set.insert(f);
                        }
                        const bool always = flag & 0x4;
                        s64 start = always ? 0 : t.i(X::atrig::StartFrame),
                            end = always ? Forever : t.i(X::atrig::EndFrame);
                        if (m_sound && end == Forever)
                            end = -1;
                        put("StartFrame", start);
                        put("EndFrame", end);
                        if (flag & 0x1)
                            ++m_oneshots;
                        ParamReader ow(
                            file, X::file::TriggerParamDefs, X::file::TriggerParamSets, t.i(X::atrig::Overwrite));
                        if (m_sound) {
                            for (const char* p : { "Volume", "Pitch", "Lpf" }) {
                                const auto [v, max] = ow.f(p);
                                nt.set_i(field_of(ts, p), f32_bits(v));
                                nt.set_i(field_of(ts, std::string(p) + "Max"), f32_bits(max));
                                set.insert(field_of(ts, p));
                                set.insert(field_of(ts, std::string(p) + "Max"));
                            }
                            const auto [d, dmax] = ow.f("Delay");
                            put("DelayMin", static_cast<s64>(std::lround(d)));
                            put("DelayMax", static_cast<s64>(std::lround(dmax)));
                        } else {
                            s64 mask = 0;
                            if (ow.has("Bone")) {
                                mask |= 1;
                                const int f = field_of(ts, "Bone");
                                nt.set_s(f, ow.str("Bone"));
                                set.insert(f);
                            }
                            const char* names[] = { "Scale", "PositionX", "PositionY", "PositionZ", "RotationX",
                                "RotationY", "RotationZ" };
                            // Values the mask leaves out stay 0, as the format writes them.
                            for (int b = 0; b < 7; ++b) {
                                if (ow.has(names[b])) {
                                    mask |= s64 { 1 } << (b + 1);
                                    nt.set_i(field_of(ts, names[b]), f32_bits(ow.f(names[b]).first));
                                }
                                set.insert(field_of(ts, names[b]));
                            }
                            put("Flags0", mask);
                            set.insert(field_of(ts, "HandoverId"));
                        }
                        m_curves += ow.curves;
                        apply_habits(nt, m_trigger_habit, set);
                    }
                    na.set_i(a_start, first);
                    na.set_i(a_start + 1, ot.size() - 1);
                }
                if (u.list(X::user::ActionSlots).size() > 1)
                    ++m_merged_slots;
                m_dropped_triggers["property"] += u.list(X::user::PropertyTriggers).size();
                m_dropped_triggers["always"] += u.list(X::user::AlwaysTriggers).size();
            }

            int ensure_table(const Node& u, Node& o, int call, std::map<int, int>& table_of) {
                const auto it = table_of.find(call);
                if (it != table_of.end())
                    return it->second;
                return call_table(u, o, call, table_of);
            }

            int call_table(const Node& u, Node& o, int call, std::map<int, int>& table_of) {
                const NodeList& calls = u.list(X::user::Calls);
                std::vector<Leaf> leaves;
                bool random = false;
                flatten(u, call, { }, leaves, true, random);
                NodeList& tables = o.list(D::user::CallTables);
                NodeList& assets = o.list(D::user::Assets);
                Node& ct = tables.append(make_element(m_o, tables));
                const Schema& cs = m_fs.call.schema;
                ct.set_s(field_of(cs, "Name"), calls.at(call).s(X::call::KeyName));
                const int s = field_of(cs, m_sound ? "AssetStart" : "EffectStart");
                std::set<int> set { s, s + 1 };
                if (m_sound) {
                    ct.set_i(field_of(cs, "Flags2"), random ? 1 : 0);
                    set.insert(field_of(cs, "Flags2"));
                }
                apply_habits(ct, m_call_habit, set);
                ct.set_i(s, assets.size());
                for (const Leaf& l : leaves)
                    asset(u, o, calls.at(l.call), l.conditions);
                ct.set_i(s + 1, assets.size() - 1);
                table_of[call] = ct.row();
                return ct.row();
            }

            void asset(const Node& u, Node& o, const Node& call, const std::array<std::string, 3>& conds) {
                (void)u;
                NodeList& assets = o.list(D::user::Assets);
                Node& a = assets.append(make_element(m_o, assets));
                const Schema& s = m_fs.asset.schema;
                std::set<int> set;
                const auto put = [&](std::string_view f, s64 v) {
                    const int i = field_of(s, f);
                    a.set_i(i, v);
                    set.insert(i);
                };
                const auto puts = [&](std::string_view f, std::string v) {
                    const int i = field_of(s, f);
                    a.set_s(i, std::move(v));
                    set.insert(i);
                };
                const auto putf = [&](std::string_view f, f32 v) { put(f, f32_bits(v)); };
                ParamReader p(*m_src.root, X::file::AssetParamDefs, X::file::AssetParamSets, call.i(X::call::ParamSet));
                put("Id", assets.size() - 1);
                if (m_sound) {
                    puts("Label", p.str("RuntimeAssetName"));
                    for (const char* n : { "Volume", "Pitch", "Lpf", "Pan", "SurroundPan" }) {
                        if (s.field_index(n) < 0)
                            continue;
                        const auto [v, max] = p.f(n);
                        putf(n, v);
                        putf(std::string(n) + "Max", max);
                    }
                    const auto [d, dmax] = p.f("Delay");
                    put("DelayMin", static_cast<s64>(std::lround(d)));
                    put("DelayMax", static_cast<s64>(std::lround(dmax)));
                    puts("Bone", p.str("Bone"));
                } else {
                    puts("Condition1", conds[0]);
                    puts("Condition2", conds[1]);
                    puts("Condition3", conds[2]);
                    puts("EmitterSet", p.str("RuntimeAssetName"));
                    puts("Bone", p.str("Bone"));
                    puts("Group", p.str("EffectGroup"));
                    const s64 matrix = p.i("Matrix");
                    put("Field0x30", matrix);
                    put("Field0x32", p.i("RotateSource"));
                    put("Field0x37", std::max<s64>(p.i("Clip") - 1, 0));
                    put("Field0x38", p.i("ForceCalc"));
                    put("Field0x31", matrix != 0 && !(p.i("BitFlag") & 2) ? 1 : 0);
                    for (const char* n : { "Scale", "PositionX", "PositionY", "PositionZ", "RotationX", "RotationY",
                             "RotationZ", "Red", "Green", "Blue", "Alpha" })
                        putf(n, p.f(n).first);
                    a.list(D::slot_params::Params).clear();
                    for (const s64 w : m_words)
                        a.list(D::slot_params::Params).append_new().set_i(D::asset_param::Value, w);
                }
                m_curves += p.curves;
                apply_habits(a, m_asset_habit, set);
            }

            void finish() {
                const std::string target = m_sound ? "SLink" : "ELink";
                for (const auto& [type, n] : m_reduced)
                    m_warnings.push_back(
                        count_text(n, type + " containers " + target + " cannot state keep only their first child"));
                for (const auto& [kind, n] : m_dropped_triggers)
                    if (n)
                        m_warnings.push_back(count_text(n, std::string(kind) + " triggers have no place in " + target));
                if (m_merged_slots)
                    m_warnings.push_back(count_text(m_merged_slots,
                        "users have several action slots, whose actions " + target + " lists together"));
                if (m_unnamed)
                    m_warnings.push_back(count_text(m_unnamed,
                        "users have only a hash that names no user of this database and are "
                        "named after it"));
                if (m_curves)
                    m_warnings.push_back(
                        count_text(m_curves, "parameters follow a curve or arrange group and keep their default"));
                if (m_oneshots)
                    m_warnings.push_back(count_text(m_oneshots, "one-shot triggers become ordinary triggers"));
                if (!m_sound && !m_words.empty())
                    m_warnings.push_back(
                        "effects carry the parameter words this database's effects use most, as XLink has none");
            }

            const Document& m_doc;
            const Document& m_src;
            const BridgeOptions& m_opt;
            std::vector<std::string>& m_warnings;
            bool m_sound;
            const D::FamilySchemas& m_fs;
            Document m_o;
            std::map<int, s64> m_asset_habit, m_trigger_habit, m_call_habit;
            std::map<u32, std::string> m_names;
            s64 m_kind = 0;
            s64 m_words_per_asset = 0;
            std::vector<s64> m_words;
            std::map<std::string, std::size_t> m_reduced;
            std::map<std::string, std::size_t> m_dropped_triggers;
            std::size_t m_merged_slots = 0, m_unnamed = 0, m_curves = 0, m_oneshots = 0;
        };

        // Values each condition slot of an EffectLink database uses.
        std::array<std::set<std::string>, 3> slot_values(const Document& d) {
            std::array<std::set<std::string>, 3> out;
            const Schema& s = D::effect_link().asset.schema;
            const int f[3] = { field_of(s, "Condition1"), field_of(s, "Condition2"), field_of(s, "Condition3") };
            walk_const(*d.root, [&](const Node& n) {
                if (&n.schema() != &s)
                    return;
                for (int k = 0; k < 3; ++k)
                    if (!n.s(f[k]).empty())
                        out[static_cast<std::size_t>(k)].insert(n.s(f[k]));
            });
            return out;
        }

        // Enum values each switch property of an XLink database compares with.
        std::map<std::string, std::set<std::string>> property_values(const Document& d) {
            std::map<std::string, std::set<std::string>> out;
            const Node& file = *d.root;
            const NodeList& cnds = file.list(X::file::Conditions);
            for (const auto& u : file.list(X::file::Users)) {
                const NodeList& calls = u->list(X::user::Calls);
                for (const auto& c : u->list(X::user::Containers)) {
                    if (c->i(X::ctn::Type) != X::CT_Switch || c->s(X::ctn::WatchPropertyName).empty())
                        continue;
                    auto& vals = out[c->s(X::ctn::WatchPropertyName)];
                    for (s64 k = c->i(X::ctn::ChildStart); k <= c->i(X::ctn::ChildEnd) && k < calls.size(); ++k) {
                        const s64 ci = k >= 0 ? calls.at(static_cast<int>(k)).i(X::call::Condition) : -1;
                        if (ci >= 0 && ci < cnds.size()
                            && cnds.at(static_cast<int>(ci)).i(X::cond::PropertyType) == X::PR_Enum)
                            vals.insert(cnds.at(static_cast<int>(ci)).s(X::cond::Value));
                    }
                }
            }
            return out;
        }

    } // namespace

    std::string bridge_problem(const Document& doc, const Document& src) {
        if (!doc.root || !src.root)
            return "no database";
        if (doc.format == src.format)
            return { };
        if (doc.format == Format::ELink0 || src.format == Format::ELink0)
            return "ELink0 users only go into ELink0 databases";
        if (src.format != counterpart(doc.format))
            return "effect and sound databases hold different things";
        if (is_xlink(doc.format) && doc.version() == 19)
            return format_name(src.format) + " users go into " + format_name(doc.format)
                + " databases of version 27 and later";
        if (is_xlink(src.format) && src.version() == 19)
            return format_name(src.format) + " v19 users do not go into " + format_name(doc.format);
        return { };
    }

    bool bridge_crosses_formats(const Document& doc, const Document& src) { return doc.format != src.format; }

    BridgeOptions default_bridge_options(const Document& doc, const Document& src) {
        BridgeOptions o;
        o.action_slot = "State";
        const Document& xl = is_xlink(doc.format) ? doc : src;
        const Document* ef = doc.format == Format::ELink ? &doc : src.format == Format::ELink ? &src : nullptr;
        if (is_xlink(xl.format) && xl.root) {
            std::map<std::string, int> slots;
            for (const auto& u : xl.root->list(X::file::Users))
                for (const auto& s : u->list(X::user::ActionSlots))
                    ++slots[s->s(X::slot::Name)];
            if (!slots.empty())
                o.action_slot = std::max_element(slots.begin(), slots.end(), [](auto& a, auto& b) {
                    return a.second < b.second;
                })->first;
        }
        if (!ef || !is_xlink(xl.format))
            return o;
        // Each condition slot stands for the property sharing most of its values.
        const auto by_slot = slot_values(*ef);
        const auto by_prop = property_values(xl);
        std::set<std::string> taken;
        for (int k = 0; k < 3; ++k) {
            std::size_t best = 0;
            for (const auto& [prop, vals] : by_prop) {
                if (taken.contains(prop))
                    continue;
                std::size_t shared = 0;
                for (const auto& v : by_slot[static_cast<std::size_t>(k)])
                    shared += vals.contains(v);
                if (shared > best) {
                    best = shared;
                    o.conditions[static_cast<std::size_t>(k)] = prop;
                }
            }
            if (best)
                taken.insert(o.conditions[static_cast<std::size_t>(k)]);
            else if (is_xlink(doc.format) && !by_slot[static_cast<std::size_t>(k)].empty())
                o.conditions[static_cast<std::size_t>(k)] = "Condition" + std::to_string(k + 1);
        }
        return o;
    }

    std::vector<xml::UserImport> bridge_users(const Document& doc, const Document& src, const std::vector<int>& which,
        const BridgeOptions& options, std::vector<std::string>& warnings) {
        if (const std::string p = bridge_problem(doc, src); !p.empty())
            throw Error(p);
        const Document s = normalized(src, doc, warnings);
        Document built;
        std::vector<int> picked = which;
        if (doc.format == s.format) {
            built = s.clone();
        } else if (is_xlink(doc.format)) {
            if (options.action_slot.empty())
                throw Error("the action slot older users' actions go in needs a name");
            for (int k = 0; k < 3 && s.format == Format::ELink; ++k)
                if (options.conditions[static_cast<std::size_t>(k)].empty())
                    throw Error("Condition" + std::to_string(k + 1) + " needs an XLink property to stand for");
            built = ToXLink(doc, s, options, warnings).run(which);
            picked.clear();
        } else {
            built = FromXLink(doc, s, options, warnings).run(which);
            picked.clear();
        }
        std::vector<std::string> losses;
        const Document conv = convert(built, Target { doc.format, doc.version(), doc.endian, doc.encoding }, losses);
        for (const auto& l : losses)
            warnings.push_back("converting to version " + std::to_string(doc.version()) + ": " + l);
        std::vector<xml::UserImport> out;
        const NodeList& users = users_of(*conv.root);
        for (int k = 0; k < users.size(); ++k)
            if (picked.empty() || std::find(picked.begin(), picked.end(), k) != picked.end())
                out.push_back(xml::user_import_from(conv, users.at(k)));
        return out;
    }

    int apply_bridged_users(Document& doc, EditStack& edits, std::vector<xml::UserImport> imports, bool replace,
        std::vector<std::string>& warnings) {
        int done = 0;
        std::size_t skipped = 0;
        edits.begin_group("Import " + std::to_string(imports.size()) + " users");
        for (auto& imp : imports) {
            const int match = xml::find_matching_user(doc, imp);
            if (match >= 0 && !replace) {
                ++skipped;
                continue;
            }
            for (auto& w : imp.warnings)
                warnings.push_back(std::move(w));
            imp.warnings.clear();
            xml::apply_user_import(doc, edits, std::move(imp), match);
            ++done;
        }
        edits.end_group();
        if (skipped)
            warnings.push_back(count_text(skipped, "users already in the database were left as they are"));
        return done;
    }

} // namespace xl
