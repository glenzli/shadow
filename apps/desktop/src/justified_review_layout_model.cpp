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

[[nodiscard]] QString representationKey(const QVariantMap& item) {
    return item.value(QStringLiteral("photoId")).toString() + QChar{0x001f}
           + item.value(QStringLiteral("representationId")).toString();
}

[[nodiscard]] int roundedDimension(const qreal value) {
    return std::max(1, static_cast<int>(std::lround(value)));
}

} // namespace

JustifiedReviewLayoutModel::JustifiedReviewLayoutModel(QObject* const parent) :
    QAbstractListModel(parent) {}

JustifiedReviewLayoutModel::~JustifiedReviewLayoutModel() {
    disconnectSourceModel();
}

int JustifiedReviewLayoutModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant JustifiedReviewLayoutModel::data(const QModelIndex& index, const int role) const {
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
    case RowKindRole:
        return row.kind;
    case SectionKeyRole:
        return row.section_key;
    case SectionTitleRole:
        return row.section_title;
    case SectionSubtitleRole:
        return row.section_subtitle;
    case SectionItemCountRole:
        return row.section_item_count;
    case SectionOrdinalRole:
        return row.section_ordinal;
    default:
        return {};
    }
}

QHash<int, QByteArray> JustifiedReviewLayoutModel::roleNames() const {
    return {
        {ItemsRole, "items"},
        {RowHeightRole, "rowHeight"},
        {UsedWidthRole, "usedWidth"},
        {RowKindRole, "rowKind"},
        {SectionKeyRole, "sectionKey"},
        {SectionTitleRole, "sectionTitle"},
        {SectionSubtitleRole, "sectionSubtitle"},
        {SectionItemCountRole, "sectionItemCount"},
        {SectionOrdinalRole, "sectionOrdinal"},
    };
}

QAbstractItemModel* JustifiedReviewLayoutModel::sourceModel() const noexcept {
    return source_model_;
}

void JustifiedReviewLayoutModel::setSourceModel(QAbstractItemModel* const source_model) {
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
        source_connections_.append(
            connect(source_model_, &QAbstractItemModel::modelReset, this, rebuild)
        );
        source_connections_.append(
            connect(source_model_, &QAbstractItemModel::layoutChanged, this, rebuild)
        );
        source_connections_.append(connect(
            source_model_,
            &QAbstractItemModel::rowsInserted,
            this,
            [rebuild](const QModelIndex&, const int, const int) { rebuild(); }
        ));
        source_connections_.append(connect(
            source_model_,
            &QAbstractItemModel::rowsRemoved,
            this,
            [rebuild](const QModelIndex&, const int, const int) { rebuild(); }
        ));
        source_connections_.append(connect(
            source_model_,
            &QAbstractItemModel::dataChanged,
            this,
            [rebuild](const QModelIndex&, const QModelIndex&, const QList<int>&) { rebuild(); }
        ));
        source_connections_.append(connect(source_model_, &QObject::destroyed, this, [this]() {
            beginResetModel();
            source_model_ = nullptr;
            source_connections_.clear();
            rows_.clear();
            endResetModel();
            emit sourceModelChanged();
        }));
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
    const int normalized = std::clamp(height, MIN_TARGET_ROW_HEIGHT, MAX_TARGET_ROW_HEIGHT);
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

QVariantList JustifiedReviewLayoutModel::sections() const {
    return section_variants_;
}

void JustifiedReviewLayoutModel::setSections(const QVariantList& sections) {
    QVector<Section> normalized;
    QVariantList normalized_variants;
    QSet<QString> seen_section_keys;
    for (const QVariant& value : sections) {
        const QVariantMap map = value.toMap();
        const QString key = map.value(QStringLiteral("key")).toString().trimmed();
        const QString title = map.value(QStringLiteral("title")).toString().trimmed();
        if (key.isEmpty() || title.isEmpty() || seen_section_keys.contains(key)) {
            continue;
        }
        QStringList representation_keys;
        QSet<QString> seen_representation_keys;
        const QVariantList raw_keys = map.value(QStringLiteral("representationKeys")).toList();
        for (const QVariant& raw_key : raw_keys) {
            const QString representation_key = raw_key.toString();
            if (!representation_key.isEmpty()
                && !seen_representation_keys.contains(representation_key)) {
                representation_keys.push_back(representation_key);
                seen_representation_keys.insert(representation_key);
            }
        }
        if (representation_keys.isEmpty()) {
            continue;
        }
        seen_section_keys.insert(key);
        Section section{
            .key = key,
            .title = title,
            .subtitle = map.value(QStringLiteral("subtitle")).toString().trimmed(),
            .navigation_label = map.value(QStringLiteral("navigationLabel")).toString().trimmed(),
            .navigation_short_label =
                map.value(QStringLiteral("navigationShortLabel")).toString().trimmed(),
            .navigation_major_label =
                map.value(QStringLiteral("navigationMajorLabel")).toString().trimmed(),
            .representation_keys = representation_keys,
        };
        normalized.push_back(section);
        normalized_variants.push_back(
            QVariantMap{
                {QStringLiteral("key"), section.key},
                {QStringLiteral("title"), section.title},
                {QStringLiteral("subtitle"), section.subtitle},
                {QStringLiteral("navigationLabel"), section.navigation_label},
                {QStringLiteral("navigationShortLabel"), section.navigation_short_label},
                {QStringLiteral("navigationMajorLabel"), section.navigation_major_label},
                {QStringLiteral("representationKeys"), section.representation_keys},
            }
        );
    }
    if (section_variants_ == normalized_variants) {
        return;
    }
    section_variants_ = std::move(normalized_variants);
    sections_ = std::move(normalized);
    rebuild();
    emit sectionsChanged();
}

QVariantList JustifiedReviewLayoutModel::sectionAnchors() const {
    QVariantList anchors;
    for (int row_index = 0; row_index < rows_.size(); ++row_index) {
        const Row& row = rows_.at(row_index);
        if (row.kind != QStringLiteral("section")) {
            continue;
        }
        anchors.push_back(
            QVariantMap{
                {QStringLiteral("key"), row.section_key},
                {QStringLiteral("title"), row.section_title},
                {QStringLiteral("navigationLabel"), row.navigation_label},
                {QStringLiteral("navigationShortLabel"), row.navigation_short_label},
                {QStringLiteral("navigationMajorLabel"), row.navigation_major_label},
                {QStringLiteral("rowIndex"), row_index},
                {QStringLiteral("ordinal"), row.section_ordinal},
            }
        );
    }
    return anchors;
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
                && item.value(QStringLiteral("representationId")).toString() == representation_id) {
                current_row = row;
                current_column = column;
                break;
            }
        }
    }
    if (current_row < 0) {
        for (int row = 0; row < rows_.size(); ++row) {
            if (!rows_.at(row).items.isEmpty()) {
                QVariantMap first = rows_.at(row).items.front().toMap();
                first.insert(QStringLiteral("layoutRow"), row);
                return first;
            }
        }
        return {};
    }

    int target_row = current_row;
    int target_column = current_column;
    if (horizontal_delta != 0) {
        const int direction = horizontal_delta > 0 ? 1 : -1;
        target_column += direction;
        if (target_column >= rows_.at(target_row).items.size()) {
            do {
                ++target_row;
            } while (target_row < rows_.size() && rows_.at(target_row).items.isEmpty());
            target_column = 0;
        } else if (target_column < 0) {
            do {
                --target_row;
            } while (target_row >= 0 && rows_.at(target_row).items.isEmpty());
            if (target_row >= 0) {
                target_column = static_cast<int>(rows_.at(target_row).items.size()) - 1;
            }
        }
    } else if (vertical_delta != 0) {
        const int direction = vertical_delta > 0 ? 1 : -1;
        int candidate_row = current_row + direction;
        while (candidate_row >= 0 && candidate_row < rows_.size()
               && rows_.at(candidate_row).items.isEmpty()) {
            candidate_row += direction;
        }
        if (candidate_row != current_row) {
            if (candidate_row < 0 || candidate_row >= rows_.size()) {
                return {};
            }
            const QVariantMap current = rows_.at(current_row).items.at(current_column).toMap();
            const qreal current_center =
                current.value(QStringLiteral("layoutX")).toReal()
                + current.value(QStringLiteral("layoutWidth")).toReal() / 2.0;
            const QVariantList& candidates = rows_.at(candidate_row).items;
            qreal nearest_distance = std::numeric_limits<qreal>::max();
            for (int column = 0; column < candidates.size(); ++column) {
                const QVariantMap candidate = candidates.at(column).toMap();
                const qreal candidate_center =
                    candidate.value(QStringLiteral("layoutX")).toReal()
                    + candidate.value(QStringLiteral("layoutWidth")).toReal() / 2.0;
                const qreal distance = std::abs(candidate_center - current_center);
                if (distance < nearest_distance) {
                    nearest_distance = distance;
                    target_column = column;
                }
            }
            target_row = candidate_row;
        }
    }

    if (target_row < 0 || target_row >= rows_.size() || target_column < 0
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
        item.insert(QString::fromLatin1(it.value()), source_model_->data(index, it.key()));
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
        const int source_count = source_model_->rowCount();
        QVariantList source_items;
        source_items.reserve(source_count);
        for (int source_row = 0; source_row < source_count; ++source_row) {
            QVariantMap item = sourceItem(source_row);
            if (!item.isEmpty()) {
                source_items.push_back(std::move(item));
            }
        }

        if (sections_.isEmpty()) {
            appendPhotoRows(next_rows, std::move(source_items));
        } else {
            QHash<QString, QVariantMap> items_by_key;
            QStringList source_order;
            for (const QVariant& value : source_items) {
                const QVariantMap item = value.toMap();
                const QString key = representationKey(item);
                if (!key.isEmpty() && !items_by_key.contains(key)) {
                    items_by_key.insert(key, item);
                    source_order.push_back(key);
                }
            }
            QSet<QString> assigned;
            int section_ordinal = 0;
            for (const Section& section : sections_) {
                QVariantList members;
                for (const QString& key : section.representation_keys) {
                    if (items_by_key.contains(key) && !assigned.contains(key)) {
                        members.push_back(items_by_key.value(key));
                        assigned.insert(key);
                    }
                }
                if (members.isEmpty()) {
                    continue;
                }
                Row header;
                header.kind = QStringLiteral("section");
                header.section_key = section.key;
                header.section_title = section.title;
                header.section_subtitle = section.subtitle;
                header.navigation_label = section.navigation_label;
                header.navigation_short_label = section.navigation_short_label;
                header.navigation_major_label = section.navigation_major_label;
                header.section_item_count = static_cast<int>(members.size());
                header.section_ordinal = section_ordinal++;
                next_rows.push_back(std::move(header));
                appendPhotoRows(next_rows, std::move(members));
            }
            QVariantList unassigned;
            for (const QString& key : source_order) {
                if (!assigned.contains(key)) {
                    unassigned.push_back(items_by_key.value(key));
                }
            }
            appendPhotoRows(next_rows, std::move(unassigned));
        }
    }

    beginResetModel();
    rows_ = std::move(next_rows);
    endResetModel();
    emit sectionAnchorsChanged();
}

void JustifiedReviewLayoutModel::appendPhotoRows(QVector<Row>& rows, QVariantList items) const {
    QVariantList current_items;
    qreal current_aspect_sum = 0.0;
    const auto append_row =
        [&rows, this](QVariantList row_items, const qreal aspect_sum, const bool justify) {
            if (row_items.isEmpty() || !(aspect_sum > 0.0)) {
                return;
            }
            const int item_count = static_cast<int>(row_items.size());
            const int gaps = std::max(0, item_count - 1) * spacing_;
            const qreal natural_height =
                justify ? (static_cast<qreal>(available_width_ - gaps) / aspect_sum)
                        : static_cast<qreal>(target_row_height_);
            const int height =
                std::clamp(roundedDimension(natural_height), MIN_ROW_HEIGHT, MAX_ROW_HEIGHT);
            qreal x = 0.0;
            for (QVariant& variant : row_items) {
                QVariantMap item = variant.toMap();
                const qreal width = aspectRatio(item) * static_cast<qreal>(height);
                item.insert(QStringLiteral("layoutX"), roundedDimension(x));
                item.insert(QStringLiteral("layoutWidth"), roundedDimension(width));
                variant = item;
                x += width + static_cast<qreal>(spacing_);
            }
            Row row;
            row.items = std::move(row_items);
            row.height = height;
            row.used_width = roundedDimension(std::max(0.0, x - spacing_));
            rows.append(std::move(row));
        };

    for (QVariant& item : items) {
        current_aspect_sum += aspectRatio(item.toMap());
        current_items.append(std::move(item));
        const int item_count = static_cast<int>(current_items.size());
        const int gaps = std::max(0, item_count - 1) * spacing_;
        const qreal preferred_width =
            current_aspect_sum * static_cast<qreal>(target_row_height_) + gaps;
        if (preferred_width >= available_width_) {
            append_row(std::move(current_items), current_aspect_sum, true);
            current_items.clear();
            current_aspect_sum = 0.0;
        }
    }
    append_row(std::move(current_items), current_aspect_sum, false);
}
