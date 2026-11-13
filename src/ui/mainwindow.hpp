#pragma once

#include <QMainWindow>
#include <QPointer>

#include <memory>
#include <vector>

#include "model/document.hpp"
#include "model/edit.hpp"
#include "model/validate.hpp"

class FieldTable;
class NodeTreeModel;
class QLabel;
class QLineEdit;
class QListWidget;
class QTabWidget;
class QTimer;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

// Laid out the way the link editors' own tooling was: pick a user (or the
// database-wide tables), browse that user's assets, actions and properties
// in tabs, edit the selected entry on the right, and read problems, graded
// Information to Fatal, along the bottom.
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  bool openPath(const QString& path);

protected:
  void closeEvent(QCloseEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

private:
  struct ListView {
    QTreeView* view;
    NodeTreeModel* model;
  };

  void buildMenus();
  void clearViews();
  void setDocument(xl::Document doc, const QString& path);
  void rebuildUsers();
  void showScope(xl::Node* scope);
  void refreshAfterEdit();
  void scheduleValidation();
  void runValidation(bool full);
  void setDirty();
  bool confirmDiscard();
  bool save();
  bool saveAs();
  bool writeTo(const QString& path, xl::Document& doc, bool xml);
  void navigateTo(const xl::Node* node);
  ListView* activeView();
  xl::Node* scopeNode() const;

  void onAdd();
  void onDuplicate();
  void onRemove();
  xl::Node* selectedNode();
  void addTo(xl::NodeList& list, int at);
  void duplicateNode(xl::Node* n);
  void removeNode(xl::Node* n);
  void moveUser(xl::Node* user, int to);
  void userDropped(int item);
  void onMove(int delta);
  void onExportXml();
  void onExportUser();
  void onImportUser();
  void onImportUsers();
  void onLoadNames();
  void onAbout();
  void userMenu(const QPoint& pos);
  void viewMenu(QTreeView* view, const QPoint& pos);
  void issuesMenu(const QPoint& pos);
  void copyIssues(bool all);
  void sortIssues(int column);

  xl::Document m_doc;
  std::unique_ptr<xl::EditStack> m_edits;
  QString m_path;
  unsigned long long m_savedState = 0;

  QLineEdit* m_userFilter = nullptr;
  QListWidget* m_users = nullptr;
  QTabWidget* m_tabs = nullptr;
  FieldTable* m_fields = nullptr;
  QTreeWidget* m_issues = nullptr;
  QLabel* m_issueSummary = nullptr;
  QTimer* m_validateTimer = nullptr;
  std::vector<ListView> m_views;
  QPointer<QTreeView> m_active;
  xl::Node* m_scope = nullptr;
  bool m_refreshing = false;
  // Issue list sorting: column and 0 for file order, 1 ascending, 2 descending.
  int m_issueSortColumn = 0;
  int m_issueSortState = 0;

  QAction* m_undo = nullptr;
  QAction* m_redo = nullptr;
};
