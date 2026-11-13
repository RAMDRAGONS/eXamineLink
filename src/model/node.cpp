#include "model/node.hpp"

namespace xl {

    int Schema::field_index(std::string_view n) const {
        for (std::size_t i = 0; i < fields.size(); ++i)
            if (fields[i].name == n)
                return static_cast<int>(i);
        return -1;
    }

    int Schema::list_index(std::string_view n) const {
        for (std::size_t i = 0; i < lists.size(); ++i)
            if (lists[i].name == n)
                return static_cast<int>(i);
        return -1;
    }

    Node::Node(const Schema* s)
        : m_schema(s)
        , m_values(s->fields.size()) {
        for (std::size_t i = 0; i < s->fields.size(); ++i) {
            const FieldType t = s->fields[i].type;
            if (t == FieldType::Ref)
                m_values[i].i = -1;
        }
        for (std::size_t i = 0; i < s->lists.size(); ++i)
            m_lists.push_back(std::make_unique<NodeList>(this, static_cast<int>(i)));
    }

    Node* Node::parent() const { return m_parent ? m_parent->owner() : nullptr; }

    std::unique_ptr<Node> Node::clone() const {
        auto n = std::make_unique<Node>(m_schema);
        n->m_values = m_values;
        for (std::size_t l = 0; l < m_lists.size(); ++l)
            for (const auto& item : *m_lists[l])
                n->m_lists[l]->append(item->clone());
        return n;
    }

    std::string Node::label() const { return m_schema->label ? m_schema->label(*this) : m_schema->name; }

    const Node* Node::ancestor(const Schema* s) const {
        for (const Node* n = this; n; n = n->parent())
            if (n->m_schema == s)
                return n;
        return nullptr;
    }

    Node* Node::ancestor(const Schema* s) {
        for (Node* n = this; n; n = n->parent())
            if (n->m_schema == s)
                return n;
        return nullptr;
    }

    const Node* Node::root() const {
        const Node* n = this;
        while (n->parent())
            n = n->parent();
        return n;
    }

    NodeList::NodeList(Node* owner, int index)
        : m_owner(owner)
        , m_index(index) { }

    const ListDef& NodeList::def() const { return m_owner->schema().lists[m_index]; }

    Node& NodeList::append(std::unique_ptr<Node> n) { return insert(size(), std::move(n)); }

    Node& NodeList::append_new() { return append(std::make_unique<Node>(def().element)); }

    Node& NodeList::insert(int at, std::unique_ptr<Node> n) {
        n->m_parent = this;
        Node& ref = *n;
        m_items.insert(m_items.begin() + at, std::move(n));
        renumber(at);
        return ref;
    }

    std::unique_ptr<Node> NodeList::take(int at) {
        auto n = std::move(m_items[at]);
        m_items.erase(m_items.begin() + at);
        n->m_parent = nullptr;
        renumber(at);
        return n;
    }

    void NodeList::move(int from, int to) {
        auto n = std::move(m_items[from]);
        m_items.erase(m_items.begin() + from);
        m_items.insert(m_items.begin() + to, std::move(n));
        renumber(std::min(from, to));
    }

    void NodeList::clear() { m_items.clear(); }

    void NodeList::renumber(int from) {
        for (int i = from; i < size(); ++i)
            m_items[i]->m_row = i;
    }

    void walk(Node& n, const std::function<void(Node&)>& fn) {
        fn(n);
        for (int l = 0; l < n.list_count(); ++l)
            for (auto& c : n.list(l))
                walk(*c, fn);
    }

    void walk_const(const Node& n, const std::function<void(const Node&)>& fn) {
        fn(n);
        for (int l = 0; l < n.list_count(); ++l)
            for (const auto& c : n.list(l))
                walk_const(*c, fn);
    }

    FieldDef field(std::string name, FieldType t, std::string doc) {
        FieldDef f;
        f.name = std::move(name);
        f.type = t;
        f.doc = std::move(doc);
        return f;
    }

    FieldDef enum_field(std::string name, const std::vector<std::string>* names, std::string doc) {
        FieldDef f = field(std::move(name), FieldType::Enum, std::move(doc));
        f.enum_names = names;
        return f;
    }

    FieldDef ref_field(std::string name, FieldType t, std::function<NodeList*(const Node&)> target, std::string doc) {
        FieldDef f = field(std::move(name), t, std::move(doc));
        f.target = std::move(target);
        return f;
    }

    FieldDef hidden_field(std::string name, FieldType t) {
        FieldDef f = field(std::move(name), t);
        f.hidden = true;
        return f;
    }

    std::function<NodeList*(const Node&)> ancestor_list(const Schema* owner, int list) {
        return [owner, list](const Node& n) -> NodeList* {
            const Node* a = n.ancestor(owner);
            return a ? &const_cast<Node*>(a)->list(list) : nullptr;
        };
    }

} // namespace xl
