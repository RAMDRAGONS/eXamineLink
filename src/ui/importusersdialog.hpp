#pragma once

#include <QDialog>

#include "model/bridge.hpp"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;

// Picks which users of another database to bring into this one, and, when
// the two are of different formats, which XLink properties the EffectLink
// conditions stand for and which action slot older users' actions go in.
class ImportUsersDialog : public QDialog {
    Q_OBJECT
public:
    ImportUsersDialog(
        const xl::Document& doc, const xl::Document& src, const QString& src_path, QWidget* parent = nullptr);

    // Indices of the chosen users in the source's user list.
    std::vector<int> users() const;
    xl::BridgeOptions options() const;
    bool replace() const;

private:
    void filter(const QString& text);
    void setAll(bool on);
    void updateCount();

    const xl::Document& m_doc;
    QLineEdit* m_filter;
    QListWidget* m_users;
    QComboBox* m_slot = nullptr;
    std::array<QComboBox*, 3> m_conditions { };
    QCheckBox* m_replace;
    QPushButton* m_ok = nullptr;
};
