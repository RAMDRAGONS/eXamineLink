#include "ui/fieldtable.hpp"

#include <QComboBox>
#include <QHeaderView>
#include <QSet>

#include "core/crc32.hpp"
#include "model/factory.hpp"
#include "model/value_text.hpp"
#include "model/xlink_schema.hpp"

using namespace xl;

namespace {

constexpr int NodeRole = Qt::UserRole + 1;
constexpr int FieldRole = Qt::UserRole + 2;
constexpr int TargetRole = Qt::UserRole + 3;
// Path key used to restore expansion after a rebuild.
constexpr int KeyRole = Qt::UserRole + 4;
constexpr int MaxDepth = 3;

QString qs(const std::string& s) {
  return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

std::string ss(const QString& s) {
  const QByteArray b = s.toUtf8();
  return std::string(b.constData(), static_cast<std::size_t>(b.size()));
}

Node* item_node(const QTreeWidgetItem* it) {
  return reinterpret_cast<Node*>(it->data(0, NodeRole).value<quintptr>());
}

QString item_key(const QTreeWidgetItem* it) {
  QString k;
  for (; it; it = it->parent())
    k.prepend(it->data(0, KeyRole).toString() + "/");
  return k;
}

}  // namespace

FieldTable::FieldTable(QWidget* parent) : QTreeWidget(parent) {
  setColumnCount(3);
  setHeaderLabels({tr("Field"), tr("Value"), tr("Refers to")});
  header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  header()->setSectionResizeMode(1, QHeaderView::Interactive);
  header()->setStretchLastSection(true);
  setColumnWidth(1, 220);
  setAlternatingRowColors(true);
  setUniformRowHeights(true);
  setEditTriggers(QAbstractItemView::NoEditTriggers);
  connect(this, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int col) {
    if (col == 2) {
      if (Node* t = targetOf(it))
        emit navigate(t);
      return;
    }
    startEdit(it, 1);
  });
  connect(this, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int col) {
    if (!m_building && col == 1)
      commit(it, it->text(1));
  });
}

Node* FieldTable::targetOf(const QTreeWidgetItem* item) {
  return reinterpret_cast<Node*>(item->data(0, TargetRole).value<quintptr>());
}

void FieldTable::setDocument(Document* doc, EditStack* edits) {
  m_doc = doc;
  m_edits = edits;
  m_node = nullptr;
  clear();
}

void FieldTable::showNode(Node* node) {
  m_node = node;
  refresh();
}

void FieldTable::refresh() {
  QSet<QString> expanded;
  for (QTreeWidgetItemIterator it(this); *it; ++it)
    if ((*it)->isExpanded())
      expanded.insert(item_key(*it));
  m_building = true;
  clear();
  if (m_node && m_doc)
    build(invisibleRootItem(), *m_node, 0);
  for (QTreeWidgetItemIterator it(this); *it; ++it)
    if (expanded.contains(item_key(*it)))
      (*it)->setExpanded(true);
  m_building = false;
}

QString FieldTable::infoFor(const Node& n, int f) const {
  const FieldDef& fd = n.schema().fields[f];
  if (m_doc->format == Format::XLink && &n.schema() == &xlink::schemas().user && f == xlink::user::Hash &&
      !n.s(xlink::user::Name).empty())
    return crc32(n.s(xlink::user::Name)) == static_cast<u32>(n.i(f)) ? tr("matches the name")
                                                                      : tr("does not match the name");
  if (!fd.target || field_kind(n, f) != DynKind::Ref)
    return {};
  const NodeList* list = fd.target(n);
  if (!list)
    return {};
  const s64 v = n.i(f);
  if (v == xlink::RefTableEnd)
    return tr("(just past the end of %1)").arg(QString::fromStdString(list->def().name));
  if (v < 0)
    return fd.type == FieldType::RangeEnd ? QString() : tr("(none)");
  if (v >= list->size())
    return tr("(missing: %1 has %2)").arg(QString::fromStdString(list->def().name)).arg(list->size());
  return qs(to_utf8(list->at(static_cast<int>(v)).label(), m_doc->encoding));
}

void FieldTable::addField(QTreeWidgetItem* parent, Node& n, int f, int depth) {
  const FieldDef& fd = n.schema().fields[f];
  auto* it = new QTreeWidgetItem(parent);
  it->setText(0, QString::fromStdString(fd.name));
  it->setText(1, qs(value_to_text(n, f, m_doc->encoding)));
  it->setData(0, NodeRole, QVariant::fromValue(reinterpret_cast<quintptr>(&n)));
  it->setData(0, FieldRole, f);
  it->setData(0, KeyRole, QString::fromStdString(fd.name));
  it->setText(2, infoFor(n, f));
  if (!fd.doc.empty())
    for (int c = 0; c < 3; ++c)
      it->setToolTip(c, QString::fromStdString(fd.doc));
  if (fd.read_only)
    it->setForeground(1, QBrush(Qt::gray));
  else
    it->setFlags(it->flags() | Qt::ItemIsEditable);

  // Expand into the entry this field points at.
  if (depth >= MaxDepth || !fd.target || field_kind(n, f) != DynKind::Ref || fd.type == FieldType::RangeEnd)
    return;
  NodeList* list = fd.target(n);
  const s64 v = n.i(f);
  if (!list || v < 0 || v >= list->size())
    return;
  Node& target = list->at(static_cast<int>(v));
  it->setData(0, TargetRole, QVariant::fromValue(reinterpret_cast<quintptr>(&target)));
  it->setToolTip(2, tr("Double-click to go to this entry"));
  if (fd.type == FieldType::RangeStart)
    return;
  // Definitions are shown by name already; their fields are clutter here.
  if (m_doc->format == Format::XLink && &n.schema() == &xlink::schemas().param && f == xlink::param::Def)
    return;
  const int users = count_references(*m_doc->root, *list, static_cast<int>(v));
  if (users > 1)
    it->setText(2, it->text(2) + tr("  (shared by %1)").arg(users));
  build(it, target, depth + 1);
}

void FieldTable::build(QTreeWidgetItem* parent, Node& n, int depth) {
  const Schema& sc = n.schema();
  for (int f = 0; f < static_cast<int>(sc.fields.size()); ++f)
    if (!sc.fields[f].hidden && stores_field(*m_doc, sc, f))
      addField(parent, n, f, depth);
  if (depth == 0)
    return;
  // Lists of a referenced entry, such as a parameter set's parameters.
  for (int l = 0; l < n.list_count(); ++l) {
    NodeList& list = n.list(l);
    if (list.def().hidden || !stores_list(*m_doc, sc, l))
      continue;
    auto* li = new QTreeWidgetItem(parent);
    li->setText(0, QString::fromStdString(list.def().name));
    li->setText(1, tr("%1 entries").arg(list.size()));
    li->setData(0, KeyRole, QString::fromStdString(list.def().name));
    QFont font = li->font(0);
    font.setItalic(true);
    li->setFont(0, font);
    for (auto& c : list) {
      auto* ci = new QTreeWidgetItem(li);
      ci->setText(0, QString("#%1").arg(c->row()));
      ci->setText(1, qs(to_utf8(c->label(), m_doc->encoding)));
      ci->setData(0, KeyRole, QString::number(c->row()));
      ci->setData(0, TargetRole, QVariant::fromValue(reinterpret_cast<quintptr>(c.get())));
      if (depth < MaxDepth)
        build(ci, *c, depth + 1);
    }
  }
}

void FieldTable::startEdit(QTreeWidgetItem* it, int column) {
  if (!(it->flags() & Qt::ItemIsEditable) || it->data(0, FieldRole).isNull())
    return;
  Node* n = item_node(it);
  const int f = it->data(0, FieldRole).toInt();
  const FieldDef& fd = n->schema().fields[f];
  const DynKind kind = field_kind(*n, f);
  // Fixed choices get a drop-down; everything else is typed.
  if (fd.type == FieldType::Enum || kind == DynKind::Bool) {
    auto* box = new QComboBox(this);
    if (kind == DynKind::Bool)
      box->addItems({"false", "true"});
    else if (fd.enum_names)
      for (std::size_t k = 0; k < fd.enum_names->size(); ++k)
        if (stores_enum_value(*m_doc, n->schema(), f, static_cast<s64>(k)))
          box->addItem(QString::fromStdString((*fd.enum_names)[k]));
    box->setEditable(fd.type == FieldType::Enum);
    box->setCurrentText(it->text(1));
    setItemWidget(it, 1, box);
    box->showPopup();
    connect(box, &QComboBox::activated, this, [this, it, box](int) {
      const QString text = box->currentText();
      removeItemWidget(it, 1);
      commit(it, text);
    });
    return;
  }
  editItem(it, column);
}

void FieldTable::commit(QTreeWidgetItem* it, const QString& text) {
  Node* n = item_node(it);
  if (!n || !m_edits)
    return;
  const int f = it->data(0, FieldRole).toInt();
  Value v;
  // Parameters choose their definition by name.
  std::string input = ss(text);
  if (m_doc->format == Format::XLink && &n->schema() == &xlink::schemas().param && f == xlink::param::Def) {
    if (const NodeList* defs = xlink::param_defs_for(*n))
      for (int k = 0; k < defs->size(); ++k)
        if (to_utf8(defs->at(k).s(xlink::pdef::Name), m_doc->encoding) == input)
          input = std::to_string(k);
  }
  if (const auto err = text_to_value(*n, f, input, m_doc->encoding, v)) {
    emit editFailed(QString::fromStdString(n->schema().fields[f].name + ": " + *err));
    refresh();
    return;
  }
  const auto extra = follow_up_edits(*m_doc, *n, f, v);
  m_edits->begin_group("Edit " + n->schema().fields[f].name);
  m_edits->set_value(*n, f, v);
  for (const FieldUpdate& u : extra)
    m_edits->set_value(*u.node, u.field, u.value);
  m_edits->end_group();
}
