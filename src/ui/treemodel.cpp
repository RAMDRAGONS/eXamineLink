#include "ui/treemodel.hpp"

#include <QBrush>
#include <QFont>

#include "model/xlink_schema.hpp"

using namespace xl;

namespace {

    // Element rows carry the node pointer; list rows the list pointer with the
    // low bit set. Both are at least 8-byte aligned.
    quintptr tag(const Node* n) { return reinterpret_cast<quintptr>(n); }
    quintptr tag(const NodeList* l) { return reinterpret_cast<quintptr>(l) | 1; }

    QString qs(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

    // Nesting depth of an XLink call inside containers, from its parent chain.
    int call_depth(const Node& n) {
        const NodeList* calls = n.parent_list();
        int depth = 0;
        s64 p = n.i(xlink::call::ParentIndex);
        while (p >= 0 && p < calls->size() && depth < 16) {
            ++depth;
            p = calls->at(static_cast<int>(p)).i(xlink::call::ParentIndex);
        }
        return depth;
    }

} // namespace

NodeTreeModel::NodeTreeModel(QObject* parent)
    : QAbstractItemModel(parent) { }

void NodeTreeModel::setList(NodeList* list, const Document* doc) {
    beginResetModel();
    m_list = list;
    m_doc = doc;
    endResetModel();
}

void NodeTreeModel::refresh() {
    beginResetModel();
    endResetModel();
}

Node* NodeTreeModel::nodeAt(const QModelIndex& index) const {
    if (!index.isValid() || isList(index))
        return nullptr;
    return reinterpret_cast<Node*>(index.internalId());
}

NodeList* NodeTreeModel::listAt(const QModelIndex& index) const {
    if (!index.isValid())
        return m_list;
    if (isList(index))
        return reinterpret_cast<NodeList*>(index.internalId() & ~quintptr { 1 });
    return reinterpret_cast<Node*>(index.internalId())->parent_list();
}

QModelIndex NodeTreeModel::indexOf(const Node* node) const {
    if (!node || !node->parent_list())
        return { };
    const NodeList* l = node->parent_list();
    if (l != m_list && !indexOfList(l).isValid())
        return { };
    return createIndex(node->row(), 0, tag(node));
}

QModelIndex NodeTreeModel::indexOfList(const NodeList* list) const {
    if (!list || list == m_list)
        return { };
    const Node* owner = list->owner();
    if (!indexOf(owner).isValid())
        return { };
    return createIndex(list->index(), 0, tag(list));
}

QModelIndex NodeTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (!m_list || row < 0 || column < 0 || column >= 2)
        return { };
    if (!parent.isValid()) {
        if (row >= m_list->size())
            return { };
        return createIndex(row, column, tag(&m_list->at(row)));
    }
    if (isList(parent)) {
        NodeList* l = listAt(parent);
        if (row >= l->size())
            return { };
        return createIndex(row, column, tag(&l->at(row)));
    }
    Node* n = nodeAt(parent);
    if (row >= n->list_count())
        return { };
    return createIndex(row, column, tag(&n->list(row)));
}

QModelIndex NodeTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid())
        return { };
    if (isList(child)) {
        const Node* owner = listAt(child)->owner();
        return createIndex(owner->row(), 0, tag(owner));
    }
    const Node* n = nodeAt(child);
    const NodeList* l = n->parent_list();
    if (l == m_list)
        return { };
    return createIndex(l->index(), 0, tag(l));
}

int NodeTreeModel::rowCount(const QModelIndex& parent) const {
    if (!m_list)
        return 0;
    if (!parent.isValid())
        return m_list->size();
    if (parent.column() != 0)
        return 0;
    if (isList(parent))
        return listAt(parent)->size();
    const Node* n = nodeAt(parent);
    int visible = 0;
    for (int l = 0; l < n->list_count(); ++l)
        visible += !n->list(l).def().hidden && stores_list(*m_doc, n->schema(), l);
    return visible ? n->list_count() : 0;
}

int NodeTreeModel::columnCount(const QModelIndex&) const { return 2; }

QVariant NodeTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return { };
    if (isList(index)) {
        const NodeList* l = listAt(index);
        if (role == Qt::DisplayRole)
            return index.column() == 0 ? QString::fromStdString(l->def().name) : QString("%1 entries").arg(l->size());
        if (role == Qt::FontRole) {
            QFont f;
            f.setItalic(true);
            return f;
        }
        if (role == Qt::ToolTipRole && !l->def().doc.empty())
            return QString::fromStdString(l->def().doc);
        return { };
    }
    const Node* n = nodeAt(index);
    if (role == Qt::DisplayRole) {
        if (index.column() == 0)
            return QString::number(n->row());
        QString label = qs(to_utf8(n->label(), m_doc->encoding));
        if (&n->schema() == &xlink::schemas().call)
            label.prepend(QString(call_depth(*n) * 4, ' '));
        return label;
    }
    if (role == Qt::ForegroundRole && index.column() == 0)
        return QBrush(Qt::gray);
    if (role == Qt::ToolTipRole)
        return QString::fromStdString(n->schema().name);
    return { };
}

QVariant NodeTreeModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return { };
    return section == 0 ? QStringLiteral("#") : QStringLiteral("Entry");
}
