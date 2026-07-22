#pragma once

#include "desktop_backend.hpp"

#include <QAbstractListModel>
#include <QVector>

class EditVersionModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        CommitIdRole = Qt::UserRole + 1,
        LabelRole,
        CreatedAtTextRole,
        SelectedRole,
        ParentCountRole,
        ChangeSummaryRole,
        ParentSummaryRole,
    };
    Q_ENUM(Role)

    explicit EditVersionModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(QVector<BackendEditVersion> versions);
    void setSelectedCommit(const QString& commit_id);
  Q_INVOKABLE void retranslateUi();

private:
  bool eventFilter(QObject *watched, QEvent *event) override;

  QVector<BackendEditVersion> versions_;
};
