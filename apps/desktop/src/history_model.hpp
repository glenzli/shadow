#pragma once

#include "backend/history_types.hpp"

#include <QAbstractListModel>
#include <QHash>
#include <QVector>

/// Read-only presentation models for the two durable History scopes.
/// Pagination, worker lifetime, and current-photo invalidation live in
/// `HistoryCoordinator`; these models own only role projection and row updates.

class PhotoHistoryModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role {
        CommitIdRole = Qt::UserRole + 1,
        NameRole,
        CreatedAtMsRole,
        ParentCommitIdsRole,
        RefsRole,
        NamedRole,
        WorkingRole,
        RootRole,
        RecipeSchemaChangedRole,
        GradeNodesAddedRole,
        GradeNodesRemovedRole,
        GradeNodesMovedRole,
        GradeNodesModifiedRole,
        RenderOpsAddedRole,
        RenderOpsRemovedRole,
        RenderOpsModifiedRole,
        ParameterBlocksChangedRole,
        ChangedParameterKeysRole,
        HasOtherChangesRole,
    };

    explicit PhotoHistoryModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(QVector<BackendPhotoHistoryEntry> entries);
    void append(QVector<BackendPhotoHistoryEntry> entries);
    void clear();

  private:
    QVector<BackendPhotoHistoryEntry> entries_;
};

class LibraryHistoryModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role {
        CommitIdRole = Qt::UserRole + 1,
        MessageRole,
        CreatedAtMsRole,
        ParentCommitIdsRole,
        RefsRole,
        RootRole,
        HeadRole,
        PhotoChangesRole,
        SharedGradeChangesRole,
        MaskChangesRole,
        StyleChangesRole,
        OutputStateChangesRole,
        TotalChangesRole,
    };

    explicit LibraryHistoryModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(QVector<BackendLibraryHistoryEntry> entries);
    void append(QVector<BackendLibraryHistoryEntry> entries);
    void clear();

  private:
    QVector<BackendLibraryHistoryEntry> entries_;
};
