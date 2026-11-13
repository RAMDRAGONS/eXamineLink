#include "ui/saveasdialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "model/convert.hpp"

using namespace xl;

namespace {

    enum Container { ByExtension, Raw, YazSarc, Zstandard, Xml };

    std::string ss(const QString& s) {
        const QByteArray b = s.toUtf8();
        return std::string(b.constData(), static_cast<std::size_t>(b.size()));
    }

} // namespace

SaveAsDialog::SaveAsDialog(const Document& doc, const QString& path, QWidget* parent)
    : QDialog(parent)
    , m_doc(doc) {
    setWindowTitle(tr("Save As"));
    resize(620, 420);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;

    auto* row = new QHBoxLayout;
    m_path = new QLineEdit(path);
    auto* browse = new QPushButton(tr("Browse..."));
    row->addWidget(m_path, 1);
    row->addWidget(browse);
    form->addRow(tr("File"), row);

    // A preset sets the version, byte order and encoding a game uses; changing
    // any of them by hand shows which preset, if any, still matches.
    m_preset = new QComboBox;
    m_preset->setObjectName("preset");
    m_preset->addItem(tr("Custom"), -1);
    for (std::size_t k = 0; k < presets().size(); ++k)
        if (presets()[k].format == doc.format)
            m_preset->addItem(QString::fromStdString(presets()[k].name), static_cast<int>(k));
    form->addRow(tr("Preset"), m_preset);

    m_target = new QComboBox;
    m_target->setObjectName("version");
    for (const Version* v : conversion_targets(doc))
        m_target->addItem(QString::fromStdString(v->label), v->version);
    // A version eXamineLink reads but has no samples to write from is kept as is.
    if (!find_version(doc.format, doc.version()))
        m_target->insertItem(0,
            tr("%1 v%2 (as loaded)").arg(QString::fromStdString(format_name(doc.format))).arg(doc.version()),
            doc.version());
    m_target->setCurrentIndex(m_target->findData(doc.version()));
    form->addRow(tr("Version"), m_target);

    m_endian = new QComboBox;
    m_endian->setObjectName("byteOrder");
    m_endian->addItem(tr("Little-endian"), static_cast<int>(Endian::Little));
    m_endian->addItem(tr("Big-endian"), static_cast<int>(Endian::Big));
    m_endian->setCurrentIndex(m_endian->findData(static_cast<int>(doc.endian)));
    form->addRow(tr("Byte order"), m_endian);
    m_encoding = new QComboBox;
    m_encoding->setObjectName("encoding");
    m_encoding->addItem(tr("Shift-JIS"), static_cast<int>(Encoding::ShiftJis));
    m_encoding->addItem(tr("UTF-8"), static_cast<int>(Encoding::Utf8));
    m_encoding->setCurrentIndex(m_encoding->findData(static_cast<int>(doc.encoding)));
    form->addRow(tr("Text encoding"), m_encoding);

    m_container = new QComboBox;
    m_container->addItem(tr("From the file extension (.szs, .zs, other)"), ByExtension);
    m_container->addItem(tr("Uncompressed"), Raw);
    m_container->addItem(tr("Yaz-compressed SARC (.szs)"), YazSarc);
    m_container->addItem(tr("Zstandard (.zs)"), Zstandard);
    m_container->addItem(tr("XML (eXamineLink text form)"), Xml);
    form->addRow(tr("Container"), m_container);

    m_member = new QLineEdit;
    m_member->setPlaceholderText(tr("Name inside the archive, such as ELink2DB.belnk"));
    for (const Layer& l : doc.layers)
        if (l.kind == LayerKind::Sarc)
            m_member->setText(QString::fromStdString(l.archive.files[l.sarc_index].name));
    form->addRow(tr("Archive member"), m_member);

    m_level = new QSpinBox;
    m_level->setRange(1, 22);
    m_level->setValue(19);
    form->addRow(tr("Zstandard level"), m_level);
    layout->addLayout(form);

    layout->addWidget(new QLabel(tr("What this version cannot keep:")));
    m_losses = new QPlainTextEdit;
    m_losses->setReadOnly(true);
    layout->addWidget(m_losses, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!m_path->text().isEmpty())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, &SaveAsDialog::browse);
    connect(m_preset, &QComboBox::activated, this, [this] {
        const int k = m_preset->currentData().toInt();
        if (k < 0)
            return;
        const Preset& p = presets()[static_cast<std::size_t>(k)];
        m_syncing = true;
        m_target->setCurrentIndex(m_target->findData(p.version));
        m_endian->setCurrentIndex(m_endian->findData(static_cast<int>(p.endian)));
        m_encoding->setCurrentIndex(m_encoding->findData(static_cast<int>(p.encoding)));
        m_syncing = false;
        updateLosses();
    });
    // A new version starts from the byte order and encoding of a preset using it.
    connect(m_target, &QComboBox::currentIndexChanged, this, [this] {
        if (m_syncing)
            return;
        if (const Version* v = find_version(m_doc.format, m_target->currentData().toUInt())) {
            const Target d = default_output(*v, m_doc);
            m_syncing = true;
            m_endian->setCurrentIndex(m_endian->findData(static_cast<int>(d.endian)));
            m_encoding->setCurrentIndex(m_encoding->findData(static_cast<int>(d.encoding)));
            m_syncing = false;
        }
        syncPreset();
        updateLosses();
    });
    for (QComboBox* box : { m_endian, m_encoding })
        connect(box, &QComboBox::currentIndexChanged, this, [this] {
            if (m_syncing)
                return;
            syncPreset();
            updateLosses();
        });
    syncPreset();
    const auto sync = [this] {
        const int c = m_container->currentData().toInt();
        const QString p = m_path->text();
        const bool sarc = c == YazSarc || (c == ByExtension && p.endsWith(".szs", Qt::CaseInsensitive));
        const bool zs = c == Zstandard || (c == ByExtension && p.endsWith(".zs", Qt::CaseInsensitive));
        m_member->setEnabled(sarc);
        m_level->setEnabled(zs);
    };
    connect(m_container, &QComboBox::currentIndexChanged, this, sync);
    connect(m_path, &QLineEdit::textChanged, this, sync);
    sync();
    updateLosses();
}

QString SaveAsDialog::path() const { return m_path->text(); }

Target SaveAsDialog::target() const {
    return { m_doc.format, m_target->currentData().toUInt(), static_cast<Endian>(m_endian->currentData().toInt()),
        static_cast<Encoding>(m_encoding->currentData().toInt()) };
}

void SaveAsDialog::syncPreset() {
    const Target t = target();
    int match = 0;
    for (int i = 1; i < m_preset->count() && !match; ++i) {
        const Preset& p = presets()[static_cast<std::size_t>(m_preset->itemData(i).toInt())];
        if (p.version == t.version && p.endian == t.endian && p.encoding == t.encoding)
            match = i;
    }
    m_preset->setCurrentIndex(match);
}

bool SaveAsDialog::asXml() const {
    const int c = m_container->currentData().toInt();
    return c == Xml || (c == ByExtension && path().endsWith(".xml", Qt::CaseInsensitive));
}

std::vector<Layer> SaveAsDialog::layers(const Document& converted) const {
    const std::string p = ss(path());
    std::vector<Layer> out;
    switch (m_container->currentData().toInt()) {
    case Raw:
        return { };
    case YazSarc:
        out = layers_for_path(p + (p.ends_with(".szs") ? "" : ".szs"), converted);
        break;
    case Zstandard:
        out = layers_for_path(p + (p.ends_with(".zs") ? "" : ".zs"), converted);
        break;
    default:
        out = layers_for_path(p, converted);
        break;
    }
    for (Layer& l : out) {
        if (l.kind == LayerKind::Zstd)
            l.zstd_level = m_level->value();
        if (l.kind == LayerKind::Sarc && !m_member->text().isEmpty())
            l.archive.files[l.sarc_index].name = ss(m_member->text());
        if (l.kind == LayerKind::Sarc)
            l.archive.endian = converted.endian;
    }
    return out;
}

void SaveAsDialog::browse() {
    const QString p = QFileDialog::getSaveFileName(this, tr("Save As"), m_path->text(),
        tr("Yaz SARC archive (*.szs);;Zstandard (*.zs);;XLink database (*.belnk *.bslnk);;"
           "Link database (*.bin);;XML (*.xml);;All files (*)"));
    if (!p.isEmpty())
        m_path->setText(p);
}

void SaveAsDialog::updateLosses() {
    const Target t = target();
    try {
        std::vector<std::string> losses;
        convert(m_doc, t, losses);
        if (losses.empty()) {
            m_losses->setPlainText(t.version == m_doc.version() ? tr("Nothing: this is the database's own version.")
                                                                : tr("Nothing that this database uses."));
            return;
        }
        QString text;
        for (const auto& l : losses)
            text += QString::fromUtf8(l.c_str()) + "\n";
        m_losses->setPlainText(text);
    } catch (const std::exception& e) {
        m_losses->setPlainText(tr("Cannot convert: %1").arg(QString::fromUtf8(e.what())));
    }
}
