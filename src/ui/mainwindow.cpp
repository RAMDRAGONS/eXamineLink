#include "ui/mainwindow.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "core/crc32.hpp"
#include "model/bridge.hpp"
#include "model/convert.hpp"
#include "model/eslink_schema.hpp"
#include "model/elink0_schema.hpp"
#include "model/factory.hpp"
#include "model/xlink_schema.hpp"
#include "model/xml_io.hpp"
#include "ui/fieldtable.hpp"
#include "ui/importusersdialog.hpp"
#include "ui/saveasdialog.hpp"
#include "ui/treemodel.hpp"

using namespace xl;

namespace {

    constexpr int NodeRole = Qt::UserRole + 1;

    QString qs(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

    std::string ss(const QString& s) {
        const QByteArray b = s.toUtf8();
        return std::string(b.constData(), static_cast<std::size_t>(b.size()));
    }

    // A database from any file the editor opens: XML or a binary, packed or not.
    Document load_any(const QString& path, std::vector<std::string>& warnings) {
        if (path.endsWith(".xml", Qt::CaseInsensitive))
            return xml::load_database(ss(path), warnings);
        return load_file(ss(path));
    }

    // The first warnings, enough for a message box.
    QString warning_text(const std::vector<std::string>& warnings) {
        QString text;
        for (std::size_t k = 0; k < warnings.size() && k < 30; ++k)
            text += qs(warnings[k]) + "\n";
        if (warnings.size() > 30)
            text += QObject::tr("and %1 more").arg(warnings.size() - 30);
        return text;
    }

    template <typename T> T* node_ptr(const QVariant& v) { return reinterpret_cast<T*>(v.value<quintptr>()); }

    struct Group {
        QString title;
        std::vector<int> lists;
    };

    // How a scope's lists are arranged into tabs: a user's assets, actions and
    // properties side by side, as the link editors grouped them.
    std::vector<Group> groups_for(const Document& doc, const Node& scope) {
        const bool root = !scope.parent();
        if (is_xlink(doc.format)) {
            using namespace xlink;
            if (root)
                return { { QObject::tr("Definitions"),
                             { file::UserParamDefs, file::AssetParamDefs, file::TriggerParamDefs } },
                    { QObject::tr("Asset Parameters"), { file::AssetParamSets } },
                    { QObject::tr("Trigger Overwrites"), { file::TriggerParamSets } },
                    { QObject::tr("Conditions"), { file::Conditions } },
                    { QObject::tr("Curves && Random"), { file::Curves, file::Randoms } },
                    { QObject::tr("Arrange Groups"), { file::ArrangeGroups } },
                    { QObject::tr("Local Properties"), { file::LocalPropertyNames, file::LocalPropertyEnumNames } } };
            return { { QObject::tr("Assets"), { user::Calls, user::Containers } },
                { QObject::tr("Actions"), { user::ActionSlots, user::Actions, user::ActionTriggers } },
                { QObject::tr("Properties"), { user::Properties, user::PropertyTriggers } },
                { QObject::tr("Always"), { user::AlwaysTriggers } },
                { QObject::tr("User"), { user::UserParams, user::LocalProperties } } };
        }
        if (doc.format == Format::ELink0) {
            using namespace elink0;
            if (root)
                return { { QObject::tr("Unlisted Records"), { file::UnlistedRecords } } };
            return { { QObject::tr("Records"), { user::Records } } };
        }
        using namespace eslink;
        if (root)
            return { { QObject::tr("Shared Assets"), { file::SharedAssets } },
                { QObject::tr("Shared Triggers"), { file::SharedTriggers } } };
        const bool pa = scope.i(user::PooledAssets), pt = scope.i(user::PooledTriggers);
        return { { QObject::tr("Call Tables"), { user::CallTables, pa ? user::AssetRefs : user::Assets } },
            { QObject::tr("Actions"), { user::Actions, pt ? user::TriggerRefs : user::Triggers } },
            { QObject::tr("Own Records"), { user::Assets, user::Triggers } },
            { QObject::tr("Pool References"), { user::AssetRefs, user::TriggerRefs } } };
    }

    NodeList& users_of(Document& doc) { return doc.root->list(doc.root->list_count() - 1); }

    QIcon severity_icon(Severity s) {
        QStyle* st = QApplication::style();
        switch (s) {
        case Severity::Information:
            return st->standardIcon(QStyle::SP_MessageBoxInformation);
        case Severity::Warning:
            return st->standardIcon(QStyle::SP_MessageBoxWarning);
        default:
            return st->standardIcon(QStyle::SP_MessageBoxCritical);
        }
    }

    constexpr int OrderRole = Qt::UserRole + 2;
    constexpr int RankRole = Qt::UserRole + 3;

    // Sorts by severity rank in the category column, by text otherwise, and by
    // the order validation reported in when the list's "neutral" flag is set.
    class IssueItem : public QTreeWidgetItem {
    public:
        using QTreeWidgetItem::QTreeWidgetItem;
        bool operator<(const QTreeWidgetItem& other) const override {
            const QTreeWidget* tree = treeWidget();
            if (!tree || tree->property("neutral").toBool())
                return data(0, OrderRole).toInt() < other.data(0, OrderRole).toInt();
            if (tree->sortColumn() == 0 && data(0, RankRole) != other.data(0, RankRole))
                return data(0, RankRole).toInt() < other.data(0, RankRole).toInt();
            if (tree->sortColumn() == 0 || text(1) == other.text(1))
                return data(0, OrderRole).toInt() < other.data(0, OrderRole).toInt();
            return QString::localeAwareCompare(text(1), other.text(1)) < 0;
        }
    };

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    resize(1400, 860);
    setAcceptDrops(true);

    // Users on the left, as the editors' "target user" selection.
    auto* left = new QWidget;
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    m_userFilter = new QLineEdit;
    m_userFilter->setPlaceholderText(tr("Filter users"));
    m_userFilter->setClearButtonEnabled(true);
    m_users = new QListWidget;
    m_users->setUniformItemSizes(true);
    m_users->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_users, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) { userMenu(p); });
    m_users->setDefaultDropAction(Qt::MoveAction);
    connect(m_users->model(), &QAbstractItemModel::rowsMoved, this,
        [this](const QModelIndex&, int start, int, const QModelIndex&, int row) {
            if (!m_refreshing)
                userDropped(row > start ? row - 1 : row);
        });
    ll->addWidget(m_userFilter);
    ll->addWidget(m_users, 1);

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);

    m_fields = new FieldTable;
    m_fields->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_fields, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QTreeWidgetItem* it = m_fields->itemAt(p);
        if (!it)
            return;
        QMenu menu;
        menu.addAction(tr("Copy Value"), this, [it] { QApplication::clipboard()->setText(it->text(1)); });
        menu.addAction(tr("Copy Row"), this,
            [it] { QApplication::clipboard()->setText(it->text(0) + "\t" + it->text(1) + "\t" + it->text(2)); });
        if (Node* t = FieldTable::targetOf(it))
            menu.addAction(tr("Go to %1").arg(it->text(2).isEmpty() ? it->text(1) : it->text(2)), this,
                [this, t] { navigateTo(t); });
        menu.exec(m_fields->viewport()->mapToGlobal(p));
    });
    auto* right = new QWidget;
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(new QLabel(tr("Selected entry")));
    rl->addWidget(m_fields, 1);

    auto* top = new QSplitter;
    top->addWidget(left);
    top->addWidget(m_tabs);
    top->addWidget(right);
    top->setStretchFactor(0, 1);
    top->setStretchFactor(1, 3);
    top->setStretchFactor(2, 2);
    top->setSizes({ 260, 640, 500 });

    auto* bottom = new QWidget;
    auto* bl = new QVBoxLayout(bottom);
    bl->setContentsMargins(0, 0, 0, 0);
    auto* bar = new QHBoxLayout;
    m_issueSummary = new QLabel;
    auto* validate = new QPushButton(tr("Validate (with test save)"));
    bar->addWidget(m_issueSummary, 1);
    bar->addWidget(validate);
    bl->addLayout(bar);
    m_issues = new QTreeWidget;
    m_issues->setHeaderLabels({ tr("Category"), tr("Problem") });
    m_issues->setRootIsDecorated(false);
    m_issues->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_issues->setContextMenuPolicy(Qt::CustomContextMenu);
    m_issues->header()->setSectionsClickable(true);
    m_issues->header()->setSortIndicatorShown(false);
    connect(m_issues->header(), &QHeaderView::sectionClicked, this, [this](int c) { sortIssues(c); });
    connect(m_issues, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) { issuesMenu(p); });
    auto* copy = new QAction(m_issues);
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutContext(Qt::WidgetShortcut);
    m_issues->addAction(copy);
    connect(copy, &QAction::triggered, this, [this] { copyIssues(false); });
    m_issues->setUniformRowHeights(true);
    m_issues->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    bl->addWidget(m_issues, 1);

    auto* main = new QSplitter(Qt::Vertical);
    main->addWidget(top);
    main->addWidget(bottom);
    main->setStretchFactor(0, 4);
    main->setStretchFactor(1, 1);
    setCentralWidget(main);

    m_validateTimer = new QTimer(this);
    m_validateTimer->setSingleShot(true);
    m_validateTimer->setInterval(700);
    connect(m_validateTimer, &QTimer::timeout, this, [this] { runValidation(false); });

    connect(m_userFilter, &QLineEdit::textChanged, this, [this] { rebuildUsers(); });
    connect(m_users, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* it) {
        if (!m_refreshing && it)
            showScope(node_ptr<Node>(it->data(NodeRole)));
    });
    connect(m_fields, &FieldTable::navigate, this, [this](Node* n) { navigateTo(n); });
    connect(m_fields, &FieldTable::editFailed, this, [this](const QString& m) { statusBar()->showMessage(m, 6000); });
    connect(m_issues, &QTreeWidget::itemActivated, this,
        [this](QTreeWidgetItem* it) { navigateTo(node_ptr<const Node>(it->data(0, NodeRole))); });
    connect(m_issues, &QTreeWidget::itemClicked, this,
        [this](QTreeWidgetItem* it) { navigateTo(node_ptr<const Node>(it->data(0, NodeRole))); });
    connect(validate, &QPushButton::clicked, this, [this] { runValidation(true); });

    buildMenus();
    m_edits = std::make_unique<EditStack>();
    statusBar()->showMessage(tr("Open an XLink, ELink, or SLink database."));
    setDirty();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open..."), QKeySequence::Open, this, [this] {
        if (!confirmDiscard())
            return;
        const QString p = QFileDialog::getOpenFileName(this, tr("Open link database"), QFileInfo(m_path).absolutePath(),
            tr("Link databases (*.szs *.zs *.belnk *.bslnk *.bin *.xml);;All files (*)"));
        if (!p.isEmpty())
            openPath(p);
    });
    file->addAction(tr("&Save"), QKeySequence::Save, this, [this] { save(); });
    file->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, [this] { saveAs(); });
    file->addSeparator();
    file->addAction(tr("Export Database as &XML..."), this, [this] { onExportXml(); });
    file->addAction(tr("Export &User as XML..."), this, [this] { onExportUser(); });
    file->addAction(tr("&Import User from XML..."), this, [this] { onImportUser(); });
    file->addAction(tr("Import Users from &Database..."), this, [this] { onImportUsers(); });
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    m_undo = edit->addAction(tr("&Undo"), QKeySequence::Undo, this, [this] { m_edits->undo(); });
    m_redo = edit->addAction(tr("&Redo"), QKeySequence::Redo, this, [this] { m_edits->redo(); });
    edit->addSeparator();
    edit->addAction(tr("&Add Entry"), QKeySequence(Qt::CTRL | Qt::Key_N), this, [this] { onAdd(); });
    edit->addAction(tr("&Duplicate Entry"), QKeySequence(Qt::CTRL | Qt::Key_D), this, [this] { onDuplicate(); });
    edit->addAction(tr("&Remove Entry"), QKeySequence::Delete, this, [this] { onRemove(); });
    edit->addAction(tr("Move &Up"), QKeySequence(Qt::ALT | Qt::Key_Up), this, [this] { onMove(-1); });
    edit->addAction(tr("Move Do&wn"), QKeySequence(Qt::ALT | Qt::Key_Down), this, [this] { onMove(1); });
    edit->addSeparator();
    edit->addAction(tr("&Find User"), QKeySequence::Find, this, [this] {
        m_userFilter->setFocus();
        m_userFilter->selectAll();
    });

    QMenu* tools = menuBar()->addMenu(tr("&Tools"));
    tools->addAction(tr("&Validate (with test save)"), QKeySequence(Qt::Key_F7), this, [this] { runValidation(true); });
    tools->addAction(tr("&Name Users from a List..."), this, [this] { onLoadNames(); });

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About eXamineLink"), this, [this] { onAbout(); });

    QToolBar* tb = addToolBar(tr("Main"));
    tb->setMovable(false);
    QStyle* st = style();
    tb->addAction(st->standardIcon(QStyle::SP_DialogOpenButton), tr("Open"), this, [this] {
        if (!confirmDiscard())
            return;
        const QString p = QFileDialog::getOpenFileName(this, tr("Open link database"), QFileInfo(m_path).absolutePath(),
            tr("Link databases (*.szs *.zs *.belnk *.bslnk *.bin *.xml);;All files (*)"));
        if (!p.isEmpty())
            openPath(p);
    });
    tb->addAction(st->standardIcon(QStyle::SP_DialogSaveButton), tr("Save"), this, [this] { save(); });
    tb->addSeparator();
    tb->addAction(m_undo);
    tb->addAction(m_redo);
    tb->addSeparator();
    tb->addAction(st->standardIcon(QStyle::SP_FileDialogNewFolder), tr("Add"), this, [this] { onAdd(); });
    tb->addAction(
        st->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Duplicate"), this, [this] { onDuplicate(); });
    tb->addAction(st->standardIcon(QStyle::SP_TrashIcon), tr("Remove"), this, [this] { onRemove(); });
    tb->addAction(st->standardIcon(QStyle::SP_ArrowUp), tr("Move Up"), this, [this] { onMove(-1); });
    tb->addAction(st->standardIcon(QStyle::SP_ArrowDown), tr("Move Down"), this, [this] { onMove(1); });
}

bool MainWindow::openPath(const QString& path) {
    try {
        std::vector<std::string> warnings;
        Document doc = load_any(path, warnings);
        if (!warnings.empty())
            QMessageBox::warning(this, tr("Opened with warnings"), warning_text(warnings));
        setDocument(std::move(doc), path);
        return true;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Open failed"), QString("%1\n\n%2").arg(path, qs(e.what())));
        return false;
    }
}

// Removed tab pages must be destroyed: a page left alive keeps a model on the
// previous scope's nodes and crashes the next time it is laid out.
void MainWindow::clearViews() {
    m_active = nullptr;
    m_views.clear();
    while (m_tabs->count()) {
        QWidget* page = m_tabs->widget(0);
        m_tabs->removeTab(0);
        delete page;
    }
}

void MainWindow::setDocument(Document doc, const QString& path) {
    // Everything pointing into the old document goes before the document does.
    m_validateTimer->stop();
    clearViews();
    m_fields->setDocument(nullptr, nullptr);
    m_issues->clear();
    m_users->clear();
    m_scope = nullptr;
    m_doc = std::move(doc);
    m_path = path;
    m_edits = std::make_unique<EditStack>(m_doc.root.get());
    m_edits->on_change = [this] {
        // Deferred: edits arrive from inside item signals of the views being rebuilt.
        QTimer::singleShot(0, this, [this] { refreshAfterEdit(); });
    };
    m_savedState = m_edits->state();
    m_fields->setDocument(&m_doc, m_edits.get());
    m_scope = nullptr;
    rebuildUsers();
    showScope(m_doc.root.get());
    setDirty();
    runValidation(false);
    statusBar()->showMessage(tr("%1: %2 v%3, %4-endian, %5, %6, %7 users")
            .arg(QFileInfo(path).fileName(), qs(format_name(m_doc.format)))
            .arg(m_doc.version())
            .arg(m_doc.endian == Endian::Big ? "big" : "little",
                m_doc.encoding == Encoding::ShiftJis ? "Shift-JIS" : "UTF-8", qs(describe(m_doc.layers)))
            .arg(users_of(m_doc).size()));
}

Node* MainWindow::scopeNode() const { return m_scope; }

void MainWindow::rebuildUsers() {
    if (!m_doc.root)
        return;
    m_refreshing = true;
    const QString filter = m_userFilter->text();
    Node* current = m_scope;
    m_users->clear();
    auto* db = new QListWidgetItem(tr("[Database tables]"));
    db->setData(NodeRole, QVariant::fromValue(reinterpret_cast<quintptr>(m_doc.root.get())));
    QFont f = db->font();
    f.setBold(true);
    db->setFont(f);
    db->setFlags(db->flags() & ~Qt::ItemIsDragEnabled);
    m_users->addItem(db);
    if (current == m_doc.root.get())
        m_users->setCurrentItem(db);
    m_users->setDragDropMode(
        users_reorderable(m_doc) ? QAbstractItemView::InternalMove : QAbstractItemView::NoDragDrop);
    for (auto& u : users_of(m_doc)) {
        const QString label = qs(to_utf8(u->label(), m_doc.encoding));
        if (!filter.isEmpty() && !label.contains(filter, Qt::CaseInsensitive))
            continue;
        auto* it = new QListWidgetItem(label);
        it->setData(NodeRole, QVariant::fromValue(reinterpret_cast<quintptr>(u.get())));
        m_users->addItem(it);
        if (u.get() == current)
            m_users->setCurrentItem(it);
    }
    m_refreshing = false;
}

void MainWindow::showScope(Node* scope) {
    if (!scope)
        return;
    const int tab = m_tabs->currentIndex();
    const bool same = scope == m_scope;
    m_scope = scope;
    clearViews();
    for (const Group& g : groups_for(m_doc, *scope)) {
        auto* split = new QSplitter(Qt::Vertical);
        for (const int l : g.lists) {
            NodeList& list = scope->list(l);
            auto* box = new QWidget;
            auto* vl = new QVBoxLayout(box);
            vl->setContentsMargins(2, 2, 2, 2);
            auto* title = new QLabel(QString("<b>%1</b>").arg(QString::fromStdString(list.def().name)));
            if (!list.def().doc.empty())
                title->setToolTip(QString::fromStdString(list.def().doc));
            vl->addWidget(title);
            auto* view = new QTreeView;
            auto* model = new NodeTreeModel(view);
            model->setList(&list, &m_doc);
            view->setModel(model);
            view->setUniformRowHeights(true);
            view->setAlternatingRowColors(true);
            view->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
            view->header()->setStretchLastSection(true);
            vl->addWidget(view, 1);
            split->addWidget(box);
            m_views.push_back({ view, model });
            connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this, view, model](const QModelIndex& cur) {
                    m_active = view;
                    if (Node* n = model->nodeAt(cur))
                        m_fields->showNode(n);
                });
            connect(view, &QTreeView::clicked, this, [this, view] { m_active = view; });
            view->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(
                view, &QWidget::customContextMenuRequested, this, [this, view](const QPoint& p) { viewMenu(view, p); });
        }
        m_tabs->addTab(split, g.title);
    }
    if (same && tab >= 0 && tab < m_tabs->count())
        m_tabs->setCurrentIndex(tab);
    m_fields->showNode(scope);
}

MainWindow::ListView* MainWindow::activeView() {
    for (ListView& v : m_views)
        if (v.view == m_active.data())
            return &v;
    // Otherwise the first view of the visible tab.
    QWidget* page = m_tabs->currentWidget();
    for (ListView& v : m_views)
        if (page && page->isAncestorOf(v.view))
            return &v;
    return nullptr;
}

void MainWindow::refreshAfterEdit() {
    if (!m_doc.root)
        return;
    // Nodes stay where they are across edits; views only need their rows rebuilt.
    for (ListView& v : m_views) {
        const QModelIndex cur = v.view->currentIndex();
        Node* sel = v.model->nodeAt(cur);
        QList<const void*> expanded;
        for (int r = 0; r < v.model->rowCount(); ++r) {
            const QModelIndex i = v.model->index(r, 0);
            if (v.view->isExpanded(i))
                expanded.append(v.model->nodeAt(i));
        }
        v.model->refresh();
        for (const void* p : expanded)
            v.view->expand(v.model->indexOf(static_cast<const Node*>(p)));
        if (sel && sel->parent_list())
            v.view->setCurrentIndex(v.model->indexOf(sel));
    }
    // The scope may have been removed (undoing a user import, say).
    bool alive = m_scope == m_doc.root.get();
    for (auto& u : users_of(m_doc))
        alive = alive || u.get() == m_scope;
    if (!alive)
        showScope(m_doc.root.get());
    rebuildUsers();
    Node* shown = m_fields->node();
    m_fields->showNode(shown && (shown == m_doc.root.get() || shown->parent_list()) ? shown : m_scope);
    setDirty();
    scheduleValidation();
}

void MainWindow::scheduleValidation() { m_validateTimer->start(); }

void MainWindow::runValidation(bool full) {
    m_issues->clear();
    if (!m_doc.root)
        return;
    const std::vector<Issue> issues = validate(m_doc, full);
    int counts[4] = { };
    for (std::size_t k = 0; k < issues.size(); ++k) {
        const Issue& i = issues[k];
        ++counts[static_cast<int>(i.severity)];
        if (k >= 2000)
            continue;
        auto* it = new IssueItem(m_issues);
        it->setData(0, OrderRole, static_cast<int>(k));
        it->setData(0, RankRole, 3 - static_cast<int>(i.severity));
        it->setIcon(0, severity_icon(i.severity));
        it->setText(0, qs(severity_name(i.severity)));
        it->setText(1, qs(i.text));
        it->setToolTip(1, qs(i.text));
        it->setData(0, NodeRole, QVariant::fromValue(reinterpret_cast<quintptr>(i.node)));
    }
    if (m_issueSortState)
        m_issues->sortItems(m_issueSortColumn, m_issueSortState == 1 ? Qt::AscendingOrder : Qt::DescendingOrder);
    m_issueSummary->setText(tr("%1 fatal, %2 errors, %3 warnings, %4 notes%5")
            .arg(counts[3])
            .arg(counts[2])
            .arg(counts[1])
            .arg(counts[0])
            .arg(full ? tr(" (test save included)") : QString()));
}

void MainWindow::setDirty() {
    const bool dirty = m_edits && m_edits->state() != m_savedState;
    const QString base = m_path.isEmpty() ? tr("(untitled)") : QFileInfo(m_path).fileName();
    setWindowTitle(QString("%1%2 - eXamineLink v" XL_VERSION).arg(base, dirty ? "*" : ""));
    if (m_undo) {
        m_undo->setEnabled(m_edits && m_edits->can_undo());
        m_redo->setEnabled(m_edits && m_edits->can_redo());
        m_undo->setText(m_edits && m_edits->can_undo() ? tr("&Undo %1").arg(qs(m_edits->undo_text())) : tr("&Undo"));
        m_redo->setText(m_edits && m_edits->can_redo() ? tr("&Redo %1").arg(qs(m_edits->redo_text())) : tr("&Redo"));
    }
}

bool MainWindow::confirmDiscard() {
    if (!m_edits || m_edits->state() == m_savedState)
        return true;
    const auto r = QMessageBox::warning(this, tr("Unsaved changes"), tr("Save the changes first?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Save)
        return save();
    return r == QMessageBox::Discard;
}

bool MainWindow::writeTo(const QString& path, Document& doc, bool as_xml) {
    try {
        if (as_xml)
            xml::save_database(doc, ss(path));
        else
            save_file(ss(path), doc);
        return true;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Save failed"), qs(e.what()));
        runValidation(true);
        return false;
    }
}

bool MainWindow::save() {
    if (!m_doc.root)
        return false;
    if (m_path.isEmpty())
        return saveAs();
    const bool as_xml = m_path.endsWith(".xml", Qt::CaseInsensitive);
    if (!as_xml)
        m_doc.layers = layers_for_path(ss(m_path), m_doc);
    if (!writeTo(m_path, m_doc, as_xml))
        return false;
    m_savedState = m_edits->state();
    setDirty();
    statusBar()->showMessage(tr("Saved %1").arg(m_path), 5000);
    return true;
}

bool MainWindow::saveAs() {
    if (!m_doc.root)
        return false;
    SaveAsDialog dlg(m_doc, m_path, this);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    const Target t = dlg.target();
    Document out;
    std::vector<std::string> losses;
    try {
        out = convert(m_doc, t, losses);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Conversion failed"), qs(e.what()));
        return false;
    }
    out.layers = dlg.layers(out);
    if (!writeTo(dlg.path(), out, dlg.asXml()))
        return false;
    // A conversion changes what is being edited; keep editing what was written.
    if (t.version != m_doc.version() || t.endian != m_doc.endian || t.encoding != m_doc.encoding) {
        setDocument(std::move(out), dlg.path());
    } else {
        m_doc.layers = out.layers;
        m_path = dlg.path();
        m_savedState = m_edits->state();
        setDirty();
    }
    statusBar()->showMessage(tr("Saved %1").arg(dlg.path()), 5000);
    return true;
}

void MainWindow::navigateTo(const Node* node) {
    if (!node || !m_doc.root)
        return;
    // The scope is the user holding the entry, or the database for its tables.
    const Node* scope = node;
    while (scope->parent() && scope->parent() != m_doc.root.get())
        scope = scope->parent();
    if (scope == node && node->parent_list() && node->parent_list()->owner() == m_doc.root.get()
        && node->parent_list()->index() != m_doc.root->list_count() - 1)
        scope = m_doc.root.get();
    if (!scope->parent() || scope->parent_list()->index() != m_doc.root->list_count() - 1)
        scope = m_doc.root.get();
    if (scope != m_scope) {
        m_userFilter->clear();
        showScope(const_cast<Node*>(scope));
        rebuildUsers();
    }
    for (ListView& v : m_views) {
        const QModelIndex i = v.model->indexOf(node);
        if (!i.isValid())
            continue;
        for (int t = 0; t < m_tabs->count(); ++t)
            if (m_tabs->widget(t)->isAncestorOf(v.view))
                m_tabs->setCurrentIndex(t);
        for (QModelIndex p = i.parent(); p.isValid(); p = p.parent())
            v.view->expand(p);
        v.view->setCurrentIndex(i);
        v.view->scrollTo(i);
        m_active = v.view;
        m_fields->showNode(const_cast<Node*>(node));
        return;
    }
    m_fields->showNode(const_cast<Node*>(node));
}

void MainWindow::onAdd() {
    if (!m_doc.root)
        return;
    ListView* v = activeView();
    NodeList* list = nullptr;
    int at = -1;
    if (v) {
        const QModelIndex cur = v->view->currentIndex();
        list = v->model->listAt(cur);
        if (Node* n = v->model->nodeAt(cur))
            at = n->row() + 1;
    }
    // With the database or a user list chosen, a new user.
    if (!list || m_users->hasFocus())
        list = &users_of(m_doc);
    addTo(*list, at);
}

void MainWindow::addTo(NodeList& list, int at) {
    if (at < 0 || at > list.size())
        at = list.size();
    Node& added = m_edits->insert(list, at, make_element(m_doc, list));
    QTimer::singleShot(0, this, [this, &added] { navigateTo(&added); });
}

Node* MainWindow::selectedNode() {
    if (m_users->hasFocus() && m_users->currentItem())
        return node_ptr<Node>(m_users->currentItem()->data(NodeRole));
    if (ListView* v = activeView())
        return v->model->nodeAt(v->view->currentIndex());
    return nullptr;
}

void MainWindow::onDuplicate() { duplicateNode(selectedNode()); }

void MainWindow::duplicateNode(Node* n) {
    if (!n || !n->parent_list())
        return;
    auto copy = n->clone();
    // XLink users are keyed by their name's hash and must stay distinct.
    if (is_xlink(m_doc.format) && &n->schema() == &xlink::schemas().user) {
        const std::string name = n->s(xlink::user::Name).empty() ? "User" : n->s(xlink::user::Name);
        copy->set_s(xlink::user::Name, name + "_Copy");
        copy->set_i(xlink::user::Hash, crc32(name + "_Copy"));
    }
    Node& added = m_edits->insert(*n->parent_list(), n->row() + 1, std::move(copy));
    QTimer::singleShot(0, this, [this, &added] { navigateTo(&added); });
}

void MainWindow::onRemove() { removeNode(selectedNode()); }

void MainWindow::removeNode(Node* n) {
    if (!n || !n->parent_list())
        return;
    NodeList& list = *n->parent_list();
    const int refs = count_references(*m_doc.root, list, n->row());
    QString question = tr("Remove %1?").arg(qs(to_utf8(n->label(), m_doc.encoding)));
    if (refs)
        question += "\n\n" + tr("%n reference(s) point at it; they will point at nothing afterwards.", nullptr, refs);
    if (QMessageBox::question(this, tr("Remove entry"), question) != QMessageBox::Yes)
        return;
    m_edits->remove(list, n->row());
}

void MainWindow::moveUser(Node* user, int to) {
    NodeList& users = users_of(m_doc);
    if (!user || !users_reorderable(m_doc) || user->parent_list() != &users || to < 0 || to >= users.size()
        || to == user->row())
        return;
    m_edits->move(users, user->row(), to);
}

// The list has already moved the item; the edit is applied once the drop has
// finished, and the rebuilt list then reflects the database.
void MainWindow::userDropped(int item) {
    QListWidgetItem* moved = m_users->item(item);
    Node* user = moved ? node_ptr<Node>(moved->data(NodeRole)) : nullptr;
    Node* before = nullptr;
    Node* after = nullptr;
    if (QListWidgetItem* next = m_users->item(item + 1))
        before = node_ptr<Node>(next->data(NodeRole));
    if (QListWidgetItem* prev = m_users->item(item - 1))
        after = node_ptr<Node>(prev->data(NodeRole));
    QTimer::singleShot(0, this, [this, user, before, after] {
        if (!user || user == m_doc.root.get() || !user->parent_list()) {
            rebuildUsers();
            return;
        }
        const int from = user->row();
        int to = from;
        if (before && before != m_doc.root.get())
            to = before->row() > from ? before->row() - 1 : before->row();
        else if (after && after != m_doc.root.get())
            to = after->row() < from ? after->row() + 1 : after->row();
        if (to == from)
            rebuildUsers();
        else
            moveUser(user, to);
    });
}

void MainWindow::onMove(int delta) {
    if (m_users->hasFocus() && m_users->currentItem()) {
        Node* u = node_ptr<Node>(m_users->currentItem()->data(NodeRole));
        if (u && u != m_doc.root.get())
            moveUser(u, u->row() + delta);
        return;
    }
    ListView* v = activeView();
    Node* n = v ? v->model->nodeAt(v->view->currentIndex()) : nullptr;
    if (!n || !n->parent_list())
        return;
    const int to = n->row() + delta;
    if (to < 0 || to >= n->parent_list()->size())
        return;
    m_edits->move(*n->parent_list(), n->row(), to);
}

void MainWindow::onExportXml() {
    if (!m_doc.root)
        return;
    const QString p = QFileDialog::getSaveFileName(
        this, tr("Export database as XML"), QFileInfo(m_path).completeBaseName() + ".xml", tr("XML (*.xml)"));
    if (p.isEmpty())
        return;
    try {
        xml::save_database(m_doc, ss(p));
        statusBar()->showMessage(tr("Exported %1").arg(p), 5000);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Export failed"), qs(e.what()));
    }
}

void MainWindow::onExportUser() {
    if (!m_doc.root || !m_scope || m_scope == m_doc.root.get()) {
        QMessageBox::information(this, tr("Export user"), tr("Select a user first."));
        return;
    }
    const QString name = qs(xml::user_file_name(m_doc, *m_scope));
    const QString p = QFileDialog::getSaveFileName(
        this, tr("Export user as XML"), QFileInfo(m_path).absoluteDir().filePath(name), tr("XML (*.xml)"));
    if (p.isEmpty())
        return;
    try {
        xml::export_user(m_doc, *m_scope, ss(p));
        statusBar()->showMessage(tr("Exported %1").arg(p), 5000);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Export failed"), qs(e.what()));
    }
}

void MainWindow::onImportUser() {
    if (!m_doc.root)
        return;
    const QString p = QFileDialog::getOpenFileName(
        this, tr("Import user from XML"), QFileInfo(m_path).absolutePath(), tr("XML (*.xml)"));
    if (p.isEmpty())
        return;
    try {
        xml::UserImport imp = xml::read_user(m_doc, ss(p));
        int replace = xml::find_matching_user(m_doc, imp);
        if (replace >= 0) {
            const auto r = QMessageBox::question(this, tr("Import user"),
                tr("%1 already exists. Replace it?\n\nNo adds the imported user alongside it.")
                    .arg(qs(to_utf8(users_of(m_doc).at(replace).label(), m_doc.encoding))),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
            if (r == QMessageBox::Cancel)
                return;
            if (r == QMessageBox::No)
                replace = -1;
        }
        const std::vector<std::string> warnings = imp.warnings;
        const int at = xml::apply_user_import(m_doc, *m_edits, std::move(imp), replace);
        Node* user = &users_of(m_doc).at(at);
        QTimer::singleShot(0, this, [this, user] {
            showScope(user);
            rebuildUsers();
        });
        if (!warnings.empty()) {
            QString text;
            for (const auto& w : warnings)
                text += qs(w) + "\n";
            QMessageBox::warning(this, tr("Imported with warnings"), text);
        }
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Import failed"), qs(e.what()));
    }
}

void MainWindow::onImportUsers() {
    if (!m_doc.root)
        return;
    const QString p = QFileDialog::getOpenFileName(this, tr("Import users from a database"),
        QFileInfo(m_path).absolutePath(), tr("Link databases (*.szs *.zs *.belnk *.bslnk *.bin *.xml);;All files (*)"));
    if (p.isEmpty())
        return;
    try {
        std::vector<std::string> warnings;
        const Document src = load_any(p, warnings);
        if (const std::string problem = bridge_problem(m_doc, src); !problem.empty()) {
            QMessageBox::information(this, tr("Import users"), qs(problem));
            return;
        }
        ImportUsersDialog dlg(m_doc, src, p, this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        std::vector<xml::UserImport> imports;
        int n = 0;
        try {
            imports = bridge_users(m_doc, src, dlg.users(), dlg.options(), warnings);
            // The users shown may be replaced; the database view outlives them.
            showScope(m_doc.root.get());
            n = apply_bridged_users(m_doc, *m_edits, std::move(imports), dlg.replace(), warnings);
        } catch (...) {
            QApplication::restoreOverrideCursor();
            throw;
        }
        QApplication::restoreOverrideCursor();
        QTimer::singleShot(0, this, [this] { rebuildUsers(); });
        statusBar()->showMessage(tr("Imported %n user(s)", nullptr, n), 5000);
        if (!warnings.empty())
            QMessageBox::warning(this, tr("Imported with warnings"), warning_text(warnings));
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Import failed"), qs(e.what()));
    }
}

void MainWindow::onLoadNames() {
    if (!m_doc.root || !is_xlink(m_doc.format)) {
        QMessageBox::information(this, tr("Name users"), tr("Only XLink databases key users by name hash."));
        return;
    }
    const QString p = QFileDialog::getOpenFileName(
        this, tr("Names, one per line"), QString(), tr("Text (*.txt *.csv *.yaml *.yml);;All files (*)"));
    if (p.isEmpty())
        return;
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    // Any identifier-like token is tried, so actor lists in other formats work too.
    std::map<u32, std::string> by_hash;
    QTextStream in(&f);
    static const QRegularExpression token(R"([A-Za-z0-9_\-\.]+|[^\s,:;"'\[\]{}]+)");
    while (!in.atEnd()) {
        const QString line = in.readLine();
        auto it = token.globalMatch(line);
        while (it.hasNext()) {
            const std::string raw = from_utf8(ss(it.next().captured()), m_doc.encoding);
            by_hash.emplace(crc32(raw), raw);
        }
    }
    m_edits->begin_group("Name users");
    int named = 0;
    for (auto& u : users_of(m_doc)) {
        if (!u->s(xlink::user::Name).empty())
            continue;
        const auto hit = by_hash.find(static_cast<u32>(u->i(xlink::user::Hash)));
        if (hit == by_hash.end())
            continue;
        Value v;
        v.s = hit->second;
        m_edits->set_value(*u, xlink::user::Name, v);
        ++named;
    }
    m_edits->end_group();
    statusBar()->showMessage(tr("Named %1 of %2 users").arg(named).arg(users_of(m_doc).size()), 6000);
}

void MainWindow::onAbout() {
    QMessageBox::about(this, tr("About eXamineLink"),
        tr("<h2>eXamineLink</h2><p>A Nintendo EPD XLink database editor</p>"
           "<p><a href=\"https://github.com/RAMDRAGONS/eXamineLink\">"
           "https://github.com/RAMDRAGONS/eXamineLink</a></p>"));
}

void MainWindow::sortIssues(int column) {
    // Each click on a column steps ascending, descending, then file order.
    m_issueSortState = column == m_issueSortColumn ? (m_issueSortState + 1) % 3 : 1;
    m_issueSortColumn = column;
    QHeaderView* h = m_issues->header();
    if (m_issueSortState == 0) {
        m_issues->setProperty("neutral", true);
        m_issues->sortItems(0, Qt::AscendingOrder);
        m_issues->setProperty("neutral", false);
        h->setSortIndicatorShown(false);
        return;
    }
    const Qt::SortOrder order = m_issueSortState == 1 ? Qt::AscendingOrder : Qt::DescendingOrder;
    m_issues->sortItems(column, order);
    h->setSortIndicatorShown(true);
    h->setSortIndicator(column, order);
}

void MainWindow::copyIssues(bool all) {
    QStringList lines;
    for (int k = 0; k < m_issues->topLevelItemCount(); ++k) {
        QTreeWidgetItem* it = m_issues->topLevelItem(k);
        if (all || it->isSelected())
            lines << it->text(0) + "\t" + it->text(1);
    }
    if (!lines.isEmpty())
        QApplication::clipboard()->setText(lines.join("\n"));
}

void MainWindow::issuesMenu(const QPoint& pos) {
    QMenu menu;
    QTreeWidgetItem* it = m_issues->itemAt(pos);
    if (it)
        menu.addAction(
            tr("Go to Entry"), this, [this, it] { navigateTo(node_ptr<const Node>(it->data(0, NodeRole))); });
    menu.addAction(tr("Copy"), QKeySequence::Copy, this, [this] { copyIssues(false); })
        ->setEnabled(!m_issues->selectedItems().isEmpty());
    menu.addAction(tr("Copy All"), this, [this] { copyIssues(true); });
    menu.addSeparator();
    menu.addAction(tr("Validate (with test save)"), this, [this] { runValidation(true); });
    menu.exec(m_issues->viewport()->mapToGlobal(pos));
}

void MainWindow::userMenu(const QPoint& pos) {
    if (!m_doc.root)
        return;
    QListWidgetItem* it = m_users->itemAt(pos);
    QMenu menu;
    Node* user = it ? node_ptr<Node>(it->data(NodeRole)) : nullptr;
    if (user && user != m_doc.root.get()) {
        showScope(user);
        m_users->setCurrentItem(it);
        menu.addAction(tr("Export User as XML..."), this, [this] { onExportUser(); });
        menu.addAction(tr("Replace from XML..."), this, [this] { onImportUser(); });
        menu.addSeparator();
        menu.addAction(tr("Duplicate User"), this, [this, user] { duplicateNode(user); });
        menu.addAction(tr("Remove User"), this, [this, user] { removeNode(user); });
        if (users_reorderable(m_doc)) {
            menu.addAction(tr("Move Up"), this, [this, user] { moveUser(user, user->row() - 1); })
                ->setEnabled(user->row() > 0);
            menu.addAction(tr("Move Down"), this, [this, user] { moveUser(user, user->row() + 1); })
                ->setEnabled(user->row() + 1 < user->parent_list()->size());
        }
        menu.addSeparator();
        const QString name = qs(to_utf8(user->label(), m_doc.encoding));
        menu.addAction(tr("Copy Name"), this, [name] { QApplication::clipboard()->setText(name); });
        if (is_xlink(m_doc.format))
            menu.addAction(tr("Copy Hash"), this, [user] {
                QApplication::clipboard()->setText(qs(hex(static_cast<u32>(user->i(xlink::user::Hash)), 8)));
            });
    } else {
        menu.addAction(tr("Export Database as XML..."), this, [this] { onExportXml(); });
    }
    menu.addSeparator();
    menu.addAction(tr("Add User"), this, [this] { addTo(users_of(m_doc), -1); });
    menu.addAction(tr("Import User from XML..."), this, [this] { onImportUser(); });
    menu.addAction(tr("Import Users from Database..."), this, [this] { onImportUsers(); });
    menu.exec(m_users->viewport()->mapToGlobal(pos));
}

void MainWindow::viewMenu(QTreeView* view, const QPoint& pos) {
    m_active = view;
    const QModelIndex idx = view->indexAt(pos);
    if (idx.isValid())
        view->setCurrentIndex(idx);
    auto* model = static_cast<NodeTreeModel*>(view->model());
    Node* n = model->nodeAt(idx);
    NodeList* list = model->listAt(idx);
    QMenu menu;
    if (list)
        menu.addAction(tr("Add Entry"), this, [this, list, n] { addTo(*list, n ? n->row() + 1 : -1); });
    if (n) {
        menu.addAction(tr("Duplicate"), this, [this, n] { duplicateNode(n); });
        menu.addAction(tr("Remove"), this, [this, n] { removeNode(n); });
        menu.addSeparator();
        menu.addAction(tr("Move Up"), this, [this] { onMove(-1); })->setEnabled(n->row() > 0);
        menu.addAction(tr("Move Down"), this, [this] { onMove(1); })
            ->setEnabled(n->row() + 1 < n->parent_list()->size());
        menu.addSeparator();
        const QString label = qs(to_utf8(n->label(), m_doc.encoding));
        menu.addAction(tr("Copy Label"), this, [label] { QApplication::clipboard()->setText(label); });
        const int refs = count_references(*m_doc.root, *n->parent_list(), n->row());
        menu.addAction(tr("%n reference(s) point here", nullptr, refs))->setEnabled(false);
    }
    menu.exec(view->viewport()->mapToGlobal(pos));
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (confirmDiscard())
        e->accept();
    else
        e->ignore();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls() && e->mimeData()->urls().size() == 1 && e->mimeData()->urls().first().isLocalFile())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    const QString p = e->mimeData()->urls().first().toLocalFile();
    e->acceptProposedAction();
    if (confirmDiscard())
        openPath(p);
}
