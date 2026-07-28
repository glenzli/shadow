#include "justified_review_layout_model.hpp"

#include <QAbstractItemModel>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int MIN_LAYOUT_WIDTH = 80;
constexpr int MIN_ROW_HEIGHT = 48;
constexpr int MAX_ROW_HEIGHT = 480;
constexpr int MIN_TARGET_ROW_HEIGHT = 96;
constexpr int MAX_TARGET_ROW_HEIGHT = 360;
constexpr int MAX_SPACING = 32;
constexpr qreal DEFAULT_ASPECT_RATIO = 4.0 / 3.0;
constexpr qreal MIN_ASPECT_RATIO = 0.12;
constexpr qreal MAX_ASPECT_RATIO = 8.0;

[[nodiscard]] int roundedDimension(const qreal value) {
    return std::max(1, static_cast<int>(std::lround(value)));
}

} // namespace

JustifiedReviewLayoutModel::JustifiedReviewLayoutModel(QObject* const parent)
    : QAbstractListModel(parent) {}

JustifiedReviewLayoutModel::~JustifiedReviewLayoutModel() {
    disconnectSourceModel();
}

int JustifiedReviewLayoutModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant JustifiedReviewLayoutModel::data(
    const QModelIndex& index,
    const int role
) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const Row& row = rows_.at(index.row());
    switch (role) {
    case ItemsRole:
        return row.items;
    case RowHeightRole:
        return row.height;
    case UsedWidthRole:
        return row.used_width;
    default:
        return {};
    }
}

QHash<int, QByteArray> JustifiedReviewLayoutModel::roleNames() const {
    return {
        {ItemsRole, "items"},
        {RowHeightRole, "rowHeight"},
        {UsedWidthRole, "usedWidth"},
    };
}

QAbstractItemModel* JustifiedReviewLayoutModel::sourceModel() const noexcept {
    return source_model_;
}

void JustifiedReviewLayoutModel::setSourceModel(
    QAbstractItemModel* const source_model
) {
    if (source_model_ == source_model) {
        return;
    }
    beginResetModel();
    disconnectSourceModel();
    source_model_ = source_model;
    rows_.clear();
    endResetModel();

    if (source_model_ != nullptr) {
        const auto rebuild = [this]() { this->rebuild(); };
        source_connections_.append(connect(
            source_model_, &QAbstractItemModel::modelReset, this, rebuild
        ));
        source_connections_.append(connect(
            source_model_, &QAbstractItemModel::layoutChanged, this, rebuild
        ));
        source_connections_.append(connect(
            source_model_, &QAbstractItemModel::rowsInserted, this,
            [rebuild](const QModelIndex&, const int, const int) { rebuild(); }
        ));
        source_connections_.append(connect(
            source_model_, &QAbstractItemModel::rowsRemoved, this,
            [rebuild](const QModelIndex&, const int, const int) { rebuild(); }
        ));
        source_connections_.append(connect(
            source_model_, &QAbstractItemModel::dataChanged, this,
            [rebuild](const QModelIndex&, const QModelIndex&, const QList<int>&) {
                rebuild();
            }
        ));
        source_connections_.append(connect(
            source_model_, &QObject::destroyed, this,
            [this]() {
                beginResetModel();
                source_model_ = nullptr;
                source_connections_.clear();
                rows_.clear();
                endResetModel();
                emit sourceModelChanged();
            }
        ));
    }

    rebuild();
    emit sourceModelChanged();
}

int JustifiedReviewLayoutModel::availableWidth() const noexcept {
    return available_width_;
}

void JustifiedReviewLayoutModel::setAvailableWidth(const int width) {
    const int normalized = std::max(0, width);
    if (available_width_ == normalized) {
        return;
    }
    available_width_ = normalized;
    rebuild();
    emit layoutChanged();
}

int JustifiedReviewLayoutModel::targetRowHeight() const noexcept {
    return target_row_height_;
}

void JustifiedReviewLayoutModel::setTargetRowHeight(const int height) {
    const int normalized = std::clamp(
        height, MIN_TARGET_ROW_HEIGHT, MAX_TARGET_ROW_HEIGHT
    );
    if (target_row_height_ == normalized) {
        return;
    }
    target_row_height_ = normalized;
    rebuild();
    emit layoutChanged();
}

int JustifiedReviewLayoutModel::spacing() const noexcept {
    return spacing_;
}

void JustifiedReviewLayoutModel::setSpacing(const int spacing) {
    const int normalized = std::clamp(spacing, 0, MAX_SPACING);
    if (spacing_ == normalized) {
        return;
    }
    spacing_ = normalized;
    rebuild();
    emit layoutChanged();
}

QVariantMap JustifiedReviewLayoutModel::navigationTarget(
    const QString& photo_id,
    const QString& representation_id,
    const int horizontal_delta,
    const int vertical_delta
) const {
    if (rows_.isEmpty()) {
        return {};
    }

    int current_row = -1;
    int current_column = -1;
    for (int row = 0; row < rows_.size() && current_row < 0; ++row) {
        const QVariantList& items = rows_.at(row).items;
        for (int column = 0; column < items.size(); ++column) {
            const QVariantMap item = items.at(column).toMap();
            if (item.value(QStringLiteral("photoId")).toString() == photo_id
                && item.value(QStringLiteral("representationId")).toString()
                    == representation_id) {
                current_row = row;
                current_column = column;
                break;
            }
        }
    }
    if (current_row < 0) {
        QVariantMap first = rows_.front().items.front().toMap();
        first.insert(QStringLiteral("layoutRow"), 0);
        return first;
    }

    int target_row = current_row;
    int target_column = current_column;
    if (horizontal_delta != 0) {
        const int direction = horizontal_delta > 0 ? 1 : -1;
        target_column += direction;
        if (target_column >= rows_.at(target_row).items.size()
            && target_row + 1 < rows_.size()) {
            ++target_row;
            target_column = 0;
        } else if (target_column < 0 && target_row > 0) {
            --target_row;
            target_column = static_cast<int>(rows_.at(target_row).items.size()) - 1;
        }
    } else if (vertical_delta != 0) {
        const int candidate_row = std::clamp(
            current_row + (vertical_delta > 0 ? 1 : -1),
            0,
            static_cast<int>(rows_.size()) - 1
        );
        if (candidate_row != current_row) {
            const QVariantMap current =
                rows_.at(current_row).items.at(current_column).toMap();
            const qreal current_center =
                current.value(QStringLiteral("layoutX")).toReal()
                + current.value(QStringLiteral("layoutWidth")).toReal() / 2.0;
            const QVariantList& candidates = rows_.at(candidate_row).items;
            qreal nearest_distance = std::numeric_limits<qreal>::max();
            for (int column = 0; column < candidates.size(); ++column) {
                const QVariantMap candidate = candidates.at(column).toMap();
                const qreal candidate_center =
                    candidate.value(QStringLiteral("layoutX")).toReal()
                    + candidate.value(QStringLiteral("layoutWidth")).toReal()
                        / 2.0;
                const qreal distance = std::abs(candidate_center - current_center);
                if (distance < nearest_distance) {
                    nearest_distance = distance;
                    target_column = column;
                }
            }
            target_row = candidate_row;
        }
    }

    if (target_row < 0 || target_row >= rows_.size()
        || target_column < 0
        || target_column >= rows_.at(target_row).items.size()) {
        return {};
    }
    QVariantMap result = rows_.at(target_row).items.at(target_column).toMap();
    result.insert(QStringLiteral("layoutRow"), target_row);
    return result;
}

QVariantMap JustifiedReviewLayoutModel::sourceItem(const int row) const {
    QVariantMap item;
    if (source_model_ == nullptr) {
        return item;
    }

    const QModelIndex index = source_model_->index(row, 0);
    if (!index.isValid()) {
        return item;
    }

    const QHash<int, QByteArray> roles = source_model_->roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        item.insert(
            QString::fromLatin1(it.value()),
            source_model_->data(index, it.key())
        );
    }
    return item;
}

qreal JustifiedReviewLayoutModel::aspectRatio(const QVariantMap& item) {
    const qreal width = item.value(QStringLiteral("visualWidth")).toReal();
    const qreal height = item.value(QStringLiteral("visualHeight")).toReal();
    if (!(width > 0.0) || !(height > 0.0)) {
        return DEFAULT_ASPECT_RATIO;
    }
    return std::clamp(width / height, MIN_ASPECT_RATIO, MAX_ASPECT_RATIO);
}

void JustifiedReviewLayoutModel::disconnectSourceModel() {
    for (const auto& connection : source_connections_) {
        disconnect(connection);
    }
    source_connections_.clear();
}

void JustifiedReviewLayoutModel::rebuild() {
    QVector<Row> next_rows;
    if (source_model_ != nullptr && available_width_ >= MIN_LAYOUT_WIDTH) {
        QVariantList current_items;
        qreal current_aspect_sum = 0.0;

        const auto append_row = [&next_rows, this](
            QVariantList items,
            const qreal aspect_sum,
            const bool justify
        ) {
            if (items.isEmpty() || !(aspect_sum > 0.0)) {
                return;
            }
            const int item_count = static_cast<int>(items.size());
            const int gaps = std::max(0, item_count - 1) * spacing_;
            const qreal natural_height = justify
                ? (static_cast<qreal>(available_width_ - gaps) / aspect_sum)
                : static_cast<qreal>(target_row_height_);
            const int height = std::clamp(
                roundedDimension(natural_height), MIN_ROW_HEIGHT, MAX_ROW_HEIGHT
            );
            qreal x = 0.0;
            for (QVariant& variant : items) {
                QVariantMap item = variant.toMap();
                const qreal width = aspectRatio(item) * static_cast<qreal>(height);
                item.insert(QStringLiteral("layoutX"), roundedDimension(x));
                item.insert(QStringLiteral("layoutWidth"), roundedDimension(width));
                variant = item;
                x += width + static_cast<qreal>(spacing_);
            }
            Row row;
            row.items = std::move(items);
            row.height = height;
            row.used_width = roundedDimension(std::max(0.0, x - spacing_));
            next_rows.append(std::move(row));
        };

        const int source_count = source_model_->rowCount();
        for (int source_row = 0; source_row < source_count; ++source_row) {
            QVariantMap item = sourceItem(source_row);
            if (item.isEmpty()) {
                continue;
            }
            current_aspect_sum += aspectRatio(item);
            current_items.append(std::move(item));
            const int item_count = static_cast<int>(current_items.size());
            const int gaps = std::max(0, item_count - 1) * spacing_;
            const qreal preferred_width = current_aspect_sum
                * static_cast<qreal>(target_row_height_) + gaps;
            if (preferred_width >= available_width_) {
                append_row(std::move(current_items), current_aspect_sum, true);
                current_items.clear();
                current_aspect_sum = 0.0;
            }
        }
        append_row(std::move(current_items), current_aspect_sum, false);
    }

    beginResetModel();
    rows_ = std::move(next_rows);
    endResetModel();
}
