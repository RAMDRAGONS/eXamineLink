// Offscreen smoke test of the editor window: open, browse, edit, undo.

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>

#include <algorithm>
#include <cstdio>
#include <functional>

#include "model/bridge.hpp"
#include "model/convert.hpp"
#include "ui/fieldtable.hpp"
#include "ui/importusersdialog.hpp"
#include "ui/mainwindow.hpp"
#include "ui/saveasdialog.hpp"
#include "ui/treemodel.hpp"

namespace {

void
settle()
{
   for (int k = 0; k < 5; ++k)
      QApplication::processEvents();
}

int
fail(const char *what)
{
   std::fprintf(stderr, "ui test failed: %s\n", what);
   return 1;
}

// Opens a widget's context menu at pos, then runs pick on it while it is
// shown; pick may trigger an action. Returns the menu's action texts.
QStringList
context_menu(QWidget *w, const QPoint &pos, const std::function<void(QMenu *)> &pick = {})
{
   QStringList texts;
   QTimer::singleShot(0, [&] {
      auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      if (!menu)
         return;
      for (QAction *a : menu->actions())
         if (!a->isSeparator())
            texts << a->text();
      if (pick)
         pick(menu);
      menu->close();
   });
   emit w->customContextMenuRequested(pos);
   return texts;
}

void
trigger(QMenu *menu, const QString &text)
{
   for (QAction *a : menu->actions())
      if (a->text() == text) {
         a->trigger();
         return;
      }
}

QStringList
column(const QTreeWidget *t, int c)
{
   QStringList out;
   for (int k = 0; k < t->topLevelItemCount(); ++k)
      out << t->topLevelItem(k)->text(c);
   return out;
}

} // namespace

int
main(int argc, char **argv)
{
   QApplication app(argc, argv);
   if (argc < 3)
      return fail("usage: examinelink-uitest DATABASE SCREENSHOT.png");
   MainWindow w;
   w.resize(1500, 900);
   w.show();
   if (!w.openPath(QString::fromLocal8Bit(argv[1])))
      return fail("open");
   settle();
   const QString shot = QString::fromLocal8Bit(argv[2]);
   w.grab().save(QString(shot).replace(".png", "-database.png"));

   // Pick the first user with something in its first tab.
   auto *users = w.findChild<QListWidget *>();
   auto *tabs = w.findChild<QTabWidget *>();
   auto *fields = w.findChild<FieldTable *>();
   if (!users || !tabs || !fields || users->count() < 2)
      return fail("widgets");
   QTreeView *view = nullptr;
   for (int k = 1; k < users->count() && !view; ++k) {
      users->setCurrentRow(k);
      settle();
      for (QTreeView *v : tabs->currentWidget()->findChildren<QTreeView *>())
         if (v->model()->rowCount() > 0) {
            view = v;
            break;
         }
   }
   if (!view)
      return fail("no user with entries");
   view->setCurrentIndex(view->model()->index(0, 0));
   settle();
   if (!fields->node())
      return fail("selection does not reach the field panel");
   // Expand the referenced records so the screenshot shows them.
   fields->expandToDepth(1);
   settle();
   w.grab().save(shot);

   // Edit the first editable field through the panel and undo it.
   const QString before = fields->topLevelItem(0)->text(1);
   // Try each editable field with a value its type accepts; the panel refuses
   // the rest, which the title shows by staying unmodified.
   bool edited = false;
   for (int k = 0; k < fields->topLevelItemCount() && !edited; ++k) {
      QTreeWidgetItem *item = fields->topLevelItem(k);
      if (!(item->flags() & Qt::ItemIsEditable))
         continue;
      const QString original = item->text(1);
      bool number = false;
      original.toLongLong(&number);
      item->setText(1, number ? (original == "1" ? "2" : "1") : original + "X");
      settle();
      edited = w.windowTitle().contains("*");
   }
   if (!edited)
      return fail("no field accepted an edit");
   QMetaObject::invokeMethod(&w, [&w] {
      for (QAction *a : w.findChildren<QAction *>())
         if (a->shortcut() == QKeySequence::Undo) {
            a->trigger();
            break;
         }
   });
   settle();
   if (w.windowTitle().startsWith("*") || w.windowTitle().contains("*"))
      return fail("undo did not clear the modified state");

   // Context menus act on the entry under the cursor, independent of focus.
   const int rows = view->model()->rowCount();
   const QStringList entryMenu = context_menu(view, view->visualRect(view->model()->index(0, 0)).center(),
                                              [](QMenu *m) { trigger(m, QObject::tr("Duplicate")); });
   settle();
   if (!entryMenu.contains("Duplicate") || !entryMenu.contains("Move Down"))
      return fail("entry context menu");
   if (view->model()->rowCount() != rows + 1)
      return fail("Duplicate from the context menu");
   const QStringList userMenu = context_menu(users, users->visualItemRect(users->currentItem()).center());
   if (!userMenu.contains("Export User as XML...") || !userMenu.contains("Remove User"))
      return fail("user context menu");

   // Users move by drag where the format lets the database choose their order.
   const bool movable = users->dragDropMode() == QAbstractItemView::InternalMove;
   if (movable && users->count() > 3) {
      const QString first = users->item(1)->text(), second = users->item(2)->text();
      users->model()->moveRows(QModelIndex(), 1, 1, QModelIndex(), 3);
      settle();
      if (users->item(1)->text() != second || users->item(2)->text() != first)
         return fail("dragging a user does not reorder the database");
      QMetaObject::invokeMethod(&w, [&w] {
         for (QAction *a : w.findChildren<QAction *>())
            if (a->shortcut() == QKeySequence::Undo) {
               a->trigger();
               break;
            }
      });
      settle();
      if (users->item(1)->text() != first)
         return fail("undoing a user move");
   }

   // Issues: header clicks cycle ascending, descending and file order; Ctrl+C copies.
   QTreeWidget *issues = nullptr;
   for (QTreeWidget *t : w.findChildren<QTreeWidget *>())
      if (!qobject_cast<FieldTable *>(t))
         issues = t;
   if (!issues)
      return fail("issues list");
   if (issues->topLevelItemCount() >= 2) {
      const QStringList fileOrder = column(issues, 1);
      QStringList sorted = fileOrder;
      std::stable_sort(sorted.begin(), sorted.end(),
                       [](const QString &a, const QString &b) { return QString::localeAwareCompare(a, b) < 0; });
      emit issues->header()->sectionClicked(1);
      if (column(issues, 1) != sorted)
         return fail("ascending issue sort");
      emit issues->header()->sectionClicked(1);
      QStringList descending = column(issues, 1);
      std::reverse(descending.begin(), descending.end());
      QStringList checked = descending;
      std::stable_sort(checked.begin(), checked.end(),
                       [](const QString &a, const QString &b) { return QString::localeAwareCompare(a, b) < 0; });
      if (descending != checked)
         return fail("descending issue sort");
      emit issues->header()->sectionClicked(1);
      if (column(issues, 1) != fileOrder)
         return fail("issue sort back to file order");
      issues->selectAll();
      for (QAction *a : issues->actions())
         if (a->shortcut() == QKeySequence::Copy)
            a->trigger();
      if (QApplication::clipboard()->text().count('\n') + 1 != issues->topLevelItemCount())
         return fail("copying issues");
   }
   // Save As: another version starts from a preset's byte order, the
   // database's own version keeps the database's, and presets set all three.
   {
      const xl::Document doc = xl::load_file(argv[1]);
      SaveAsDialog dlg(doc, "out.bin");
      auto *version = dlg.findChild<QComboBox *>("version");
      auto *preset = dlg.findChild<QComboBox *>("preset");
      if (!version || !preset)
         return fail("save as dialog");
      if (preset->currentIndex() == 0 && xl::preset_for(doc.format, doc.version()) &&
          xl::preset_for(doc.format, doc.version())->endian == doc.endian &&
          xl::preset_for(doc.format, doc.version())->encoding == doc.encoding)
         return fail("save as does not recognize the database's preset");
      for (int k = 0; k < version->count(); ++k) {
         version->setCurrentIndex(k);
         const xl::u32 v = version->currentData().toUInt();
         const xl::Version *ver = xl::find_version(doc.format, v);
         const xl::Endian want = ver ? xl::default_output(*ver, doc).endian : doc.endian;
         if (dlg.target().endian != want)
            return fail("save as byte order default");
      }
      version->setCurrentIndex(version->findData(doc.version()));
      dlg.resize(620, 460);
      dlg.grab().save(QString(shot).replace(".png", "-saveas.png"));
      for (int k = 1; k < preset->count(); ++k) {
         preset->setCurrentIndex(k);
         emit preset->activated(k);
         const xl::Preset &p = xl::presets()[static_cast<std::size_t>(preset->itemData(k).toInt())];
         const xl::Target t = dlg.target();
         if (t.version != p.version || t.endian != p.endian || t.encoding != p.encoding || preset->currentIndex() != k)
            return fail("save as preset");
      }
   }
   // Import Users: the source's users listed and checked, the filter hiding
   // the others, nothing to import once none is chosen, and an import of one
   // user replacing it in place.
   {
      xl::Document doc = xl::load_file(argv[1]);
      const xl::NodeList &users = doc.root->list(doc.root->list_count() - 1);
      ImportUsersDialog dlg(doc, doc, argv[1]);
      auto *list = dlg.findChild<QListWidget *>("users");
      auto *filter = dlg.findChild<QLineEdit *>("filter");
      if (!list || !filter || list->count() != users.size() ||
          dlg.users().size() != static_cast<std::size_t>(users.size()))
         return fail("import users dialog");
      dlg.resize(560, 620);
      dlg.grab().save(QString(shot).replace(".png", "-import.png"));
      if (users.size() > 0) {
         filter->setText(list->item(0)->text());
         if (list->item(0)->isHidden())
            return fail("import users filter");
         filter->clear();
         for (QPushButton *b : dlg.findChildren<QPushButton *>())
            if (b->text() == "Select None")
               b->click();
         for (QPushButton *b : dlg.findChildren<QPushButton *>())
            if (b->text().startsWith("Import") && b->isEnabled() && dlg.users().empty())
               return fail("import users with none chosen");
         list->item(0)->setCheckState(Qt::Checked);
         std::vector<std::string> warnings;
         const xl::Document src = doc.clone();
         auto imports = xl::bridge_users(doc, src, dlg.users(), dlg.options(), warnings);
         xl::EditStack edits(doc.root.get());
         const int before_n = users.size();
         if (xl::apply_bridged_users(doc, edits, std::move(imports), dlg.replace(), warnings) != 1 ||
             users.size() != before_n)
            return fail("import users replacing one user");
         edits.undo();
         if (users.size() != before_n)
            return fail("undoing the import");
      }
   }
   std::printf("ui test passed (%s, %d issues, users %s)\n", qPrintable(before), issues->topLevelItemCount(),
               movable ? "reorderable" : "in hash order");
   return 0;
}
