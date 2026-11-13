#include "model/edit.hpp"

namespace xl {

    namespace {

        bool is_ref_field(const Node& n, int f) {
            const FieldDef& d = n.schema().fields[f];
            switch (d.type) {
            case FieldType::Ref:
            case FieldType::RangeStart:
            case FieldType::RangeEnd:
                return true;
            case FieldType::Dynamic:
                return d.dyn_kind && d.dyn_kind(n) == DynKind::Ref;
            default:
                return false;
            }
        }

        s64 remap(FieldType t, s64 v, ListOp op, int a, int b) {
            switch (op) {
            case ListOp::Insert:
                return v >= a ? v + 1 : v;
            case ListOp::Remove:
                if (t == FieldType::RangeStart)
                    return v > a ? v - 1 : v;
                if (t == FieldType::RangeEnd)
                    return v >= a ? v - 1 : v;
                if (v == a)
                    return -1;
                return v > a ? v - 1 : v;
            case ListOp::Move:
                if (v == a)
                    return b;
                if (a < b && v > a && v <= b)
                    return v - 1;
                if (b < a && v >= b && v < a)
                    return v + 1;
                return v;
            }
            return v;
        }

    } // namespace

    void fix_references(Node& root, const NodeList& list, ListOp op, int a, int b, std::vector<FieldChange>& changes) {
        (void)root;
        // Appending moves no existing element, so nothing can need adjusting.
        if (op == ListOp::Insert && a >= list.size())
            return;
        // References resolve through ancestors, so only the list owner's subtree
        // can point into the list.
        walk(*list.owner(), [&](Node& n) {
            const Schema& s = n.schema();
            for (int f = 0; f < static_cast<int>(s.fields.size()); ++f) {
                if (!is_ref_field(n, f) || !s.fields[f].target)
                    continue;
                if (s.fields[f].target(n) != &list)
                    continue;
                const s64 v = n.i(f);
                FieldType t = s.fields[f].type;
                if (t == FieldType::Dynamic)
                    t = FieldType::Ref;
                // An unset reference stays unset; an empty range (end before start)
                // still moves with its anchor.
                if (t == FieldType::Ref && v < 0)
                    continue;
                const s64 nv = remap(t, v, op, a, b);
                if (nv != v) {
                    changes.push_back({ &n, f, n.value(f) });
                    n.set_i(f, nv);
                }
            }
        });
    }

    void fix_inserted(Node& inserted, const NodeList& list, int at, std::vector<FieldChange>& changes) {
        walk(inserted, [&](Node& n) {
            const Schema& s = n.schema();
            for (int f = 0; f < static_cast<int>(s.fields.size()); ++f) {
                if (!is_ref_field(n, f) || !s.fields[f].target || s.fields[f].target(n) != &list)
                    continue;
                const s64 v = n.i(f);
                FieldType t = s.fields[f].type;
                if (t == FieldType::Dynamic)
                    t = FieldType::Ref;
                if (t == FieldType::Ref && v < 0)
                    continue;
                const s64 nv = remap(t, v, ListOp::Insert, at, 0);
                if (nv != v) {
                    changes.push_back({ &n, f, n.value(f) });
                    n.set_i(f, nv);
                }
            }
        });
    }

    int count_references(const Node& root, const NodeList& list, int index) {
        int count = 0;
        walk_const(root, [&](const Node& n) {
            const Schema& s = n.schema();
            for (int f = 0; f < static_cast<int>(s.fields.size()); ++f) {
                if (!is_ref_field(n, f) || !s.fields[f].target || s.fields[f].target(n) != &list)
                    continue;
                const FieldType t = s.fields[f].type;
                if (t == FieldType::RangeStart) {
                    const s64 end = n.i(f + 1);
                    if (n.i(f) <= index && index <= end)
                        ++count;
                } else if (t != FieldType::RangeEnd && n.i(f) == index) {
                    ++count;
                }
            }
        });
        return count;
    }

    struct EditStack::Command {
        std::string text;
        unsigned long long serial = 0;
        virtual ~Command() = default;
        virtual void undo() = 0;
        virtual void redo() = 0;
    };

    namespace {

        void restore(std::vector<FieldChange>& changes) {
            for (auto it = changes.rbegin(); it != changes.rend(); ++it)
                it->node->value(it->field) = it->before;
        }

    } // namespace

    struct SetValueCmd : EditStack::Command {
        Node* node;
        int field;
        Value before, after;
        void undo() override { node->value(field) = before; }
        void redo() override { node->value(field) = after; }
    };

    struct InsertCmd : EditStack::Command {
        Node* root;
        NodeList* list;
        int at;
        std::unique_ptr<Node> held;
        std::vector<FieldChange> changes;
        void undo() override {
            restore(changes);
            held = list->take(at);
        }
        void redo() override {
            changes.clear();
            fix_references(*root, *list, ListOp::Insert, at, 0, changes);
            Node& placed = list->insert(at, std::move(held));
            // Undo restores these after taking the node back out, so its own fields
            // return to the numbering it arrived with.
            fix_inserted(placed, *list, at, changes);
        }
    };

    struct RemoveCmd : EditStack::Command {
        Node* root;
        NodeList* list;
        int at;
        std::unique_ptr<Node> held;
        std::vector<FieldChange> changes;
        void undo() override {
            list->insert(at, std::move(held));
            restore(changes);
        }
        void redo() override {
            changes.clear();
            held = list->take(at);
            fix_references(*root, *list, ListOp::Remove, at, 0, changes);
        }
    };

    struct MoveCmd : EditStack::Command {
        Node* root;
        NodeList* list;
        int from, to;
        std::vector<FieldChange> changes;
        void undo() override {
            list->move(to, from);
            restore(changes);
        }
        void redo() override {
            changes.clear();
            fix_references(*root, *list, ListOp::Move, from, to, changes);
            list->move(from, to);
        }
    };

    struct GroupCmd : EditStack::Command {
        std::vector<std::unique_ptr<Command>> parts;
        void undo() override {
            for (auto it = parts.rbegin(); it != parts.rend(); ++it)
                (*it)->undo();
        }
        void redo() override {
            for (auto& p : parts)
                p->redo();
        }
    };

    EditStack::EditStack(Node* root)
        : m_root(root) { }

    EditStack::~EditStack() = default;

    void EditStack::reset(Node* root) {
        m_root = root;
        m_done.clear();
        m_undone.clear();
        m_group.reset();
        m_group_depth = 0;
        ++m_revision;
    }

    unsigned long long EditStack::state() const { return m_done.empty() ? 0 : m_done.back()->serial; }

    void EditStack::push(std::unique_ptr<Command> c) {
        c->serial = ++m_serial;
        if (m_group) {
            static_cast<GroupCmd&>(*m_group).parts.push_back(std::move(c));
        } else {
            m_done.push_back(std::move(c));
            m_undone.clear();
        }
        changed();
    }

    void EditStack::changed() {
        ++m_revision;
        if (on_change && !m_group)
            on_change();
    }

    void EditStack::set_value(Node& n, int field, Value v) {
        if (n.value(field) == v)
            return;
        auto c = std::make_unique<SetValueCmd>();
        c->text = "Edit " + n.schema().fields[field].name;
        c->node = &n;
        c->field = field;
        c->before = n.value(field);
        c->after = std::move(v);
        c->redo();
        push(std::move(c));
    }

    Node& EditStack::insert(NodeList& list, int at, std::unique_ptr<Node> n) {
        auto c = std::make_unique<InsertCmd>();
        c->text = "Add " + n->schema().name;
        c->root = m_root;
        c->list = &list;
        c->at = at;
        c->held = std::move(n);
        c->redo();
        Node& ref = list.at(at);
        push(std::move(c));
        return ref;
    }

    void EditStack::remove(NodeList& list, int at) {
        auto c = std::make_unique<RemoveCmd>();
        c->text = "Remove " + list.at(at).schema().name;
        c->root = m_root;
        c->list = &list;
        c->at = at;
        c->redo();
        push(std::move(c));
    }

    void EditStack::move(NodeList& list, int from, int to) {
        if (from == to)
            return;
        auto c = std::make_unique<MoveCmd>();
        c->text = "Move " + list.at(from).schema().name;
        c->root = m_root;
        c->list = &list;
        c->from = from;
        c->to = to;
        c->redo();
        push(std::move(c));
    }

    void EditStack::begin_group(std::string text) {
        if (m_group_depth++ == 0) {
            m_group = std::make_unique<GroupCmd>();
            m_group->text = std::move(text);
        }
    }

    void EditStack::end_group() {
        if (--m_group_depth > 0)
            return;
        auto g = std::move(m_group);
        g->serial = ++m_serial;
        if (!static_cast<GroupCmd&>(*g).parts.empty()) {
            m_done.push_back(std::move(g));
            m_undone.clear();
        }
        if (on_change)
            on_change();
    }

    std::string EditStack::undo_text() const { return m_done.empty() ? std::string() : m_done.back()->text; }

    std::string EditStack::redo_text() const { return m_undone.empty() ? std::string() : m_undone.back()->text; }

    void EditStack::undo() {
        if (m_done.empty())
            return;
        auto c = std::move(m_done.back());
        m_done.pop_back();
        c->undo();
        m_undone.push_back(std::move(c));
        changed();
    }

    void EditStack::redo() {
        if (m_undone.empty())
            return;
        auto c = std::move(m_undone.back());
        m_undone.pop_back();
        c->redo();
        m_done.push_back(std::move(c));
        changed();
    }

} // namespace xl
