#pragma once

#include <QDialog>

#include "model/document.hpp"

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

// Save As: where, which version of the format, its byte order and text
// encoding (or a game preset that sets all three), and which container. Choosing another version lists what that
// version cannot hold before anything is written.
class SaveAsDialog : public QDialog {
    Q_OBJECT
public:
    SaveAsDialog(const xl::Document& doc, const QString& path, QWidget* parent = nullptr);

    QString path() const;
    // The version to write with the chosen byte order and encoding.
    xl::Target target() const;
    // Containers to write, from the choice and the path's extension.
    std::vector<xl::Layer> layers(const xl::Document& converted) const;
    bool asXml() const;

private:
    void updateLosses();
    void syncPreset();
    void browse();

    const xl::Document& m_doc;
    QLineEdit* m_path;
    QComboBox* m_preset;
    QComboBox* m_target;
    QComboBox* m_endian;
    QComboBox* m_encoding;
    QComboBox* m_container;
    QLineEdit* m_member;
    QSpinBox* m_level;
    QPlainTextEdit* m_losses;
    bool m_syncing = false;
};
