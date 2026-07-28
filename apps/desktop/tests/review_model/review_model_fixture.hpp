#pragma once

#include "review_model.hpp"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QPersistentModelIndex>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace review_model_test {

inline void require(
    const bool condition,
    const std::string& message
) {
    if (!condition) {
        std::cerr << "review model contract failed: "
                  << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] inline QVariant value(
    const ReviewModel& model,
    const int row,
    const ReviewModel::Role role
) {
    return model.data(model.index(row, 0), role);
}

struct ModelSignalCounts final {
    int resets = 0;
    int inserted = 0;
    int inserted_rows = 0;
    int removed = 0;
    int removed_rows = 0;
    int moved = 0;
    int changed = 0;
    int first_changed_row = -1;
    int last_changed_row = -1;
    QList<int> last_changed_roles;
};

inline void observe_model(
    ReviewModel& model,
    ModelSignalCounts& counts
) {
    QObject::connect(
        &model,
        &QAbstractItemModel::modelReset,
        [&counts]() { ++counts.resets; }
    );
    QObject::connect(
        &model,
        &QAbstractItemModel::rowsInserted,
        [&counts](
            const QModelIndex&,
            const int first,
            const int last
        ) {
            ++counts.inserted;
            counts.inserted_rows += last - first + 1;
        }
    );
    QObject::connect(
        &model,
        &QAbstractItemModel::rowsRemoved,
        [&counts](
            const QModelIndex&,
            const int first,
            const int last
        ) {
            ++counts.removed;
            counts.removed_rows += last - first + 1;
        }
    );
    QObject::connect(
        &model,
        &QAbstractItemModel::rowsMoved,
        [&counts](
            const QModelIndex&,
            const int,
            const int,
            const QModelIndex&,
            const int
        ) { ++counts.moved; }
    );
    QObject::connect(
        &model,
        &QAbstractItemModel::dataChanged,
        [&counts](
            const QModelIndex& top_left,
            const QModelIndex& bottom_right,
            const QList<int>& roles
        ) {
            ++counts.changed;
            counts.first_changed_row = top_left.row();
            counts.last_changed_row = bottom_right.row();
            counts.last_changed_roles = roles;
        }
    );
}

[[nodiscard]] inline ReviewItem keyed_item(
    const char* const key,
    const char* const title
) {
    ReviewItem item;
    item.photo_id =
        QString::fromLatin1(key) + QStringLiteral("-photo");
    item.representation_id = QString::fromLatin1(key);
    item.title = QString::fromLatin1(title);
    return item;
}

} // namespace review_model_test
