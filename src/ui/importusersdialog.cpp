#include "ui/importusersdialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/text.hpp"
#include "model/xlink_schema.hpp"

using namespace xl;

namespace {

QString qs(const std::string& s) {
  return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

std::string ss(const QString& s) {
  const QByteArray b = s.toUtf8();
  return std::string(b.constData(), static_cast<std::size_t>(b.size()));
}

QComboBox* editable_combo(const QStringList& items, const QString& current) {
  auto* c = new QComboBox;
  c->setEditable(true);
  c->addItems(items);
  c->setCurrentText(current);
  return c;
}

}  // namespace

ImportUsersDialog::ImportUsersDialog(const Document& doc, const Document& src, const QString& src_path,
                                     QWidget* parent)
    : QDialog(parent), m_doc(doc) {
  setWindowTitle(tr("Import Users"));
  resize(560, 620);
  auto* layout = new QVBoxLayout(this);
  layout->addWidget(new QLabel(tr("From %1 (%2 v%3)")
                                   .arg(QFileInfo(src_path).fileName(), qs(format_name(src.format)))
                                   .arg(src.version())));

  m_filter = new QLineEdit;
  m_filter->setObjectName("filter");
  m_filter->setPlaceholderText(tr("Filter users"));
  m_filter->setClearButtonEnabled(true);
  layout->addWidget(m_filter);
  m_users = new QListWidget;
  m_users->setObjectName("users");
  const NodeList& users = src.root->list(src.root->list_count() - 1);
  for (int k = 0; k < users.size(); ++k) {
    auto* it = new QListWidgetItem(qs(to_utf8(users.at(k).label(), src.encoding)), m_users);
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
    it->setCheckState(Qt::Checked);
    it->setData(Qt::UserRole, k);
  }
  layout->addWidget(m_users, 1);
  auto* row = new QHBoxLayout;
  auto* all = new QPushButton(tr("Select All"));
  auto* none = new QPushButton(tr("Select None"));
  row->addWidget(all);
  row->addWidget(none);
  row->addStretch(1);
  layout->addLayout(row);

  if (bridge_crosses_formats(doc, src)) {
    const BridgeOptions def = default_bridge_options(doc, src);
    auto* box = new QGroupBox(tr("Between %1 and %2").arg(qs(format_name(src.format)), qs(format_name(doc.format))));
    auto* form = new QFormLayout(box);
    const Document& xl_side = doc.format == Format::XLink ? doc : src;
    if (doc.format == Format::XLink) {
      QStringList slot_names;
      for (const auto& u : xl_side.root->list(xlink::file::Users))
        for (const auto& s : u->list(xlink::user::ActionSlots)) {
          const QString n = qs(to_utf8(s->s(xlink::slot::Name), doc.encoding));
          if (!slot_names.contains(n))
            slot_names << n;
        }
      m_slot = editable_combo(slot_names, qs(to_utf8(def.action_slot, doc.encoding)));
      m_slot->setObjectName("slot");
      m_slot->setToolTip(tr("XLink action slot that holds the actions of each imported user"));
      form->addRow(tr("Action slot"), m_slot);
    }
    if (doc.format == Format::EffectLink || src.format == Format::EffectLink) {
      QStringList props;
      for (const auto& n : xl_side.root->list(xlink::file::LocalPropertyNames))
        props << qs(to_utf8(n->s(xlink::name::Name), xl_side.encoding));
      for (int k = 0; k < 3; ++k) {
        m_conditions[static_cast<std::size_t>(k)] =
            editable_combo(props, qs(to_utf8(def.conditions[static_cast<std::size_t>(k)], doc.encoding)));
        m_conditions[static_cast<std::size_t>(k)]->setObjectName(QString("condition%1").arg(k + 1));
        m_conditions[static_cast<std::size_t>(k)]->setToolTip(
            tr("XLink property the game compares EffectLink's Condition%1 with").arg(k + 1));
        form->addRow(tr("Condition%1 property").arg(k + 1), m_conditions[static_cast<std::size_t>(k)]);
      }
    }
    layout->addWidget(box);
  }

  m_replace = new QCheckBox(tr("Replace users this database already has"));
  m_replace->setObjectName("replace");
  m_replace->setChecked(true);
  m_replace->setToolTip(tr("Users of the same name, or for XLink the same name hash; otherwise those are skipped"));
  layout->addWidget(m_replace);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  m_ok = buttons->button(QDialogButtonBox::Ok);
  m_ok->setText(tr("Import"));
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(all, &QPushButton::clicked, this, [this] { setAll(true); });
  connect(none, &QPushButton::clicked, this, [this] { setAll(false); });
  connect(m_filter, &QLineEdit::textChanged, this, &ImportUsersDialog::filter);
  connect(m_users, &QListWidget::itemChanged, this, &ImportUsersDialog::updateCount);
  updateCount();
}

void ImportUsersDialog::filter(const QString& text) {
  for (int k = 0; k < m_users->count(); ++k)
    m_users->item(k)->setHidden(!m_users->item(k)->text().contains(text, Qt::CaseInsensitive));
}

// Selecting applies to the users the filter shows.
void ImportUsersDialog::setAll(bool on) {
  for (int k = 0; k < m_users->count(); ++k)
    if (!m_users->item(k)->isHidden())
      m_users->item(k)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
}

void ImportUsersDialog::updateCount() {
  const int n = static_cast<int>(users().size());
  m_ok->setEnabled(n > 0);
  m_ok->setText(n == 1 ? tr("Import 1 User") : tr("Import %1 Users").arg(n));
}

std::vector<int> ImportUsersDialog::users() const {
  std::vector<int> out;
  for (int k = 0; k < m_users->count(); ++k)
    if (m_users->item(k)->checkState() == Qt::Checked)
      out.push_back(m_users->item(k)->data(Qt::UserRole).toInt());
  return out;
}

BridgeOptions ImportUsersDialog::options() const {
  BridgeOptions o;
  if (m_slot)
    o.action_slot = from_utf8(ss(m_slot->currentText().trimmed()), m_doc.encoding);
  for (int k = 0; k < 3; ++k)
    if (QComboBox* c = m_conditions[static_cast<std::size_t>(k)])
      o.conditions[static_cast<std::size_t>(k)] = from_utf8(ss(c->currentText().trimmed()), m_doc.encoding);
  return o;
}

bool ImportUsersDialog::replace() const {
  return m_replace->isChecked();
}
