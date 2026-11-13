#pragma once

#include <QAbstractItemModel>

#include "core/text.hpp"
#include "model/document.hpp"
#include "model/node.hpp"

// Shows one list of nodes as a tree: each element is a row, and the lists an
// element holds (a curve's points, a parameter set's parameters) are child
// rows that expand to their own elements.
class NodeTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit NodeTreeModel(QObject* parent = nullptr);

    void setList(xl::NodeList* list, const xl::Document* doc);
    xl::NodeList* list() const { return m_list; }
    // Rebuilds after the underlying nodes changed.
    void refresh();

    // The node a row shows, or nullptr for list rows.
    xl::Node* nodeAt(const QModelIndex& index) const;
    // The list a row stands for (list rows) or holds its node (element rows).
    xl::NodeList* listAt(const QModelIndex& index) const;
    QModelIndex indexOf(const xl::Node* node) const;
    QModelIndex indexOfList(const xl::NodeList* list) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = { }) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = { }) const override;
    int columnCount(const QModelIndex& parent = { }) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

private:
    static bool isList(const QModelIndex& i) { return i.internalId() & 1; }

    xl::NodeList* m_list = nullptr;
    const xl::Document* m_doc = nullptr;
};
