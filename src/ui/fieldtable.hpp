#pragma once

#include <QTreeWidget>

#include "model/document.hpp"
#include "model/edit.hpp"

// The property panel: one row per field of the selected entry, editable in
// place. A field that points at another entry (a call's parameter set,
// condition or container, a trigger's overwrite set) expands to that entry's
// fields and lists, so shared data can be edited without leaving the call.
class FieldTable : public QTreeWidget {
   Q_OBJECT
public:
   explicit FieldTable(QWidget *parent = nullptr);

   void setDocument(xl::Document *doc, xl::EditStack *edits);
   void showNode(xl::Node *node);
   xl::Node *node() const
   {
      return m_node;
   }
   // The entry a reference row points at, or null.
   static xl::Node *targetOf(const QTreeWidgetItem *item);
   // Rebuilds the rows, keeping which ones were expanded.
   void refresh();

signals:
   void navigate(xl::Node *target);
   void editFailed(const QString &message);

private:
   void build(QTreeWidgetItem *parent, xl::Node &n, int depth);
   void addField(QTreeWidgetItem *parent, xl::Node &n, int f, int depth);
   void startEdit(QTreeWidgetItem *item, int column);
   void commit(QTreeWidgetItem *item, const QString &text);
   QString infoFor(const xl::Node &n, int f) const;

   xl::Document *m_doc = nullptr;
   xl::EditStack *m_edits = nullptr;
   xl::Node *m_node = nullptr;
   bool m_building = false;
};
