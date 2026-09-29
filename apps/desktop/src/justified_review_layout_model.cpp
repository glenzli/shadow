#include "justified_review_layout_model.hpp"

#include "review_diagnostics.hpp"

#include <QAbstractItemModel>
#include <QElapsedTimer>

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
const QString SOURCE_ROW_KEY = QStringLiteral("__sourceRow");

[[nodiscard]] QString representationKey(const QVariantMap& item) {
    return item.value(QStringLiteral("photoId")).toString() + QChar{0x001f}
           + item.value(QStringLiteral("representationId")).toString();
}

[[nodiscard]] int roundedDimension(const qreal value) {
    return std::max(1, static_cast<int>(std::lround(value)));
}

} // namespace

JustifiedReviewLayoutModel::JustifiedReviewLayoutModel(QObject* const parent) :
    QAbstractListModel(parent) {
    rebuild_timer_.setSingleShot(true);
    connect(
        &rebuild_timer_,
        &QTimer::timeout,
        this,
        &JustifiedReviewLayoutModel::flushSourceUpdates
    );
}

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
    case ItemsRole: {
        QVariantList visible_items;
        visible_items.reserve(row.items.size());
        const auto roles = source_model_ ? source_model_->roleNames() : QHash<int, QByteArray>{};
        for (const QVariant& value : row.items) {
            visible_items.push_back(materializeItem(value.toMap(), roles));
        }
        return visible_items;
    }
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
    rebuild_timer_.stop();
    beginResetModel();
    disconnectSourceModel();
    source_model_ = source_model;
    rows_.clear();
    item_positions_.clear();
    section_header_rows_.clear();
    projected_source_count_ = 0;
    endResetModel();

    if (source_model_ != nullptr) {
        const auto request_rebuild = [this]() { this->requestRebuild(); };
        source_connections_.append(
            connect(source_model_, &QAbstractItemModel::modelReset, this, request_rebuild)
        );
        source_connections_.append(
            connect(source_model_, &QAbstractItemModel::layoutChanged, this, request_rebuild)
        );
        source_connections_.append(connect(
            source_model_,
            &QAbstractItemModel::rowsInserted,
            this,
            [this](const QModelIndex& parent, const int first, const int) {
                if (parent.isValid()) {
                    requestRebuild();
                } else {
                    requestAppend(first);
                }
            }
        ));
        source_connections_.append(connect(
            source_model_,
            &QAbstractItemModel::rowsRemoved,
            this,
            [request_rebuild](const QModelIndex&, const int, const int) { request_rebuild(); }
        ));
        source_connections_.append(
            connect(source_model_, &QAbstractItemModel::rowsMoved, this, request_rebuild)
        );
        source_connections_.append(connect(
            source_model_,
            &QAbstractItemModel::dataChanged,
            this,
            &JustifiedReviewLayoutModel::updateSourceItems
        ));
        source_connections_.append(connect(source_model_, &QObject::destroyed, this, [this]() {
            rebuild_timer_.stop();
            beginResetModel();
            source_model_ = nullptr;
            source_connections_.clear();
            rows_.clear();
            item_positions_.clear();
            section_header_rows_.clear();
            projected_source_count_ = 0;
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
    const bool appended = appendGroupedSourceRows(normalized);
    section_variants_ = std::move(normalized_variants);
    sections_ = std::move(normalized);
    if (!appended) {
        rebuild();
    }
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

    const auto position = item_positions_.value(photo_id + QChar{0x001f} + representation_id);
    const int current_row = absoluteRow(position);
    const int current_column = position.column;
    if (current_row < 0) {
        for (int row = 0; row < rows_.size(); ++row) {
            if (!rows_.at(row).items.isEmpty()) {
                QVariantMap first = rows_.at(row).items.front().toMap();
                first = materializeItem(
                    first,
                    source_model_ ? source_model_->roleNames() : QHash<int, QByteArray>{}
                );
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
    QVariantMap result = materializeItem(
        rows_.at(target_row).items.at(target_column).toMap(),
        source_model_ ? source_model_->roleNames() : QHash<int, QByteArray>{}
    );
    result.insert(QStringLiteral("layoutRow"), target_row);
    return result;
}

JustifiedReviewLayoutModel::ProjectionRoles
JustifiedReviewLayoutModel::projectionRoles(const QHash<int, QByteArray>& roles) {
    return {
        .photo_id = roles.key("photoId", -1),
        .representation_id = roles.key("representationId", -1),
        .visual_width = roles.key("visualWidth", -1),
        .visual_height = roles.key("visualHeight", -1),
    };
}

QVariantMap
JustifiedReviewLayoutModel::sourceItem(const int row, const ProjectionRoles& roles) const {
    QVariantMap item;
    if (source_model_ == nullptr) {
        return item;
    }

    const QModelIndex index = source_model_->index(row, 0);
    if (!index.isValid()) {
        return item;
    }

    item.insert(SOURCE_ROW_KEY, row);
    if (roles.photo_id >= 0) {
        item.insert(QStringLiteral("photoId"), source_model_->data(index, roles.photo_id));
    }
    if (roles.representation_id >= 0) {
        item.insert(
            QStringLiteral("representationId"),
            source_model_->data(index, roles.representation_id)
        );
    }
    if (roles.visual_width >= 0) {
        item.insert(QStringLiteral("visualWidth"), source_model_->data(index, roles.visual_width));
    }
    if (roles.visual_height >= 0) {
        item.insert(
            QStringLiteral("visualHeight"),
            source_model_->data(index, roles.visual_height)
        );
    }
    return item;
}

QVariantMap JustifiedReviewLayoutModel::materializeItem(
    const QVariantMap& geometry,
    const QHash<int, QByteArray>& roles
) const {
    QVariantMap item;
    if (source_model_ != nullptr) {
        const int photo_role = roles.key("photoId", -1);
        const int representation_role = roles.key("representationId", -1);
        const QString expected_photo = geometry.value(QStringLiteral("photoId")).toString();
        const QString expected_representation =
            geometry.value(QStringLiteral("representationId")).toString();
        const auto matches = [&](const QModelIndex& candidate) {
            return candidate.isValid()
                   && (photo_role < 0
                       || source_model_->data(candidate, photo_role).toString() == expected_photo)
                   && (representation_role < 0
                       || source_model_->data(candidate, representation_role).toString()
                              == expected_representation);
        };
        QModelIndex source_index = source_model_->index(geometry.value(SOURCE_ROW_KEY).toInt(), 0);
        if (!matches(source_index) && photo_role >= 0 && representation_role >= 0) {
            source_index = {};
            for (int row = 0; row < source_model_->rowCount(); ++row) {
                const QModelIndex candidate = source_model_->index(row, 0);
                if (matches(candidate)) {
                    source_index = candidate;
                    break;
                }
            }
        }
        if (matches(source_index)) {
            for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
                item.insert(
                    QString::fromLatin1(it.value()),
                    source_model_->data(source_index, it.key())
                );
            }
        }
    }
    for (auto it = geometry.cbegin(); it != geometry.cend(); ++it) {
        if (it.key() != SOURCE_ROW_KEY) {
            item.insert(it.key(), it.value());
        }
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

int JustifiedReviewLayoutModel::absoluteRow(const ItemPosition& position) const {
    if (position.section_ordinal >= 0) {
        return position.section_ordinal < section_header_rows_.size()
                   ? section_header_rows_.at(position.section_ordinal) + position.row
                   : -1;
    }
    return position.row;
}

void JustifiedReviewLayoutModel::disconnectSourceModel() {
    for (const auto& connection : source_connections_) {
        disconnect(connection);
    }
    source_connections_.clear();
}

void JustifiedReviewLayoutModel::requestRebuild() {
    full_rebuild_pending_ = true;
    append_pending_ = false;
    if (!rebuild_timer_.isActive()) {
        rebuild_timer_.start(0);
    }
}

void JustifiedReviewLayoutModel::requestAppend(const int first) {
    if (full_rebuild_pending_) {
        return;
    }
    if (source_model_ && source_model_->rowCount() == projected_source_count_) {
        return;
    }
    if (!source_model_ || available_width_ < MIN_LAYOUT_WIDTH || first < projected_source_count_) {
        requestRebuild();
        return;
    }
    append_pending_ = true;
    if (!rebuild_timer_.isActive()) {
        rebuild_timer_.start(0);
    }
}

void JustifiedReviewLayoutModel::flushSourceUpdates() {
    if (full_rebuild_pending_) {
        rebuild();
    } else if (append_pending_) {
        appendSourceRows();
    }
}

void JustifiedReviewLayoutModel::appendSourceRows() {
    rebuild_timer_.stop();
    append_pending_ = false;
    if (!source_model_) {
        rebuild();
        return;
    }
    const int source_count = source_model_->rowCount();
    if (source_count == projected_source_count_) {
        return;
    }
    if (!sections_.isEmpty() || available_width_ < MIN_LAYOUT_WIDTH
        || source_count < projected_source_count_) {
        rebuild();
        return;
    }
    auto& diagnostics = ReviewDiagnostics::instance();
    QElapsedTimer diagnostic_clock;
    if (diagnostics.enabled()) {
        diagnostic_clock.start();
    }

    const bool replace_tail = !rows_.isEmpty() && !rows_.last().justified;
    QVariantList suffix_items = replace_tail ? rows_.last().items : QVariantList{};
    const ProjectionRoles roles = projectionRoles(source_model_->roleNames());
    for (int source_row = projected_source_count_; source_row < source_count; ++source_row) {
        QVariantMap item = sourceItem(source_row, roles);
        if (item.isEmpty()) {
            rebuild();
            return;
        }
        suffix_items.push_back(std::move(item));
    }
    QVector<Row> suffix_rows;
    appendPhotoRows(suffix_rows, std::move(suffix_items));
    if (suffix_rows.isEmpty()) {
        rebuild();
        return;
    }

    const int first_changed_row = static_cast<int>(rows_.size()) - static_cast<int>(replace_tail);
    if (replace_tail) {
        for (const QVariant& value : rows_.last().items) {
            item_positions_.remove(representationKey(value.toMap()));
        }
        rows_.last() = std::move(suffix_rows.front());
        suffix_rows.removeFirst();
        const auto& items = rows_.last().items;
        for (int column = 0; column < items.size(); ++column) {
            item_positions_.insert(
                representationKey(items.at(column).toMap()),
                {first_changed_row, column, -1}
            );
        }
        const QModelIndex changed = index(first_changed_row, 0);
        emit dataChanged(changed, changed, {ItemsRole, RowHeightRole, UsedWidthRole});
    }
    if (!suffix_rows.isEmpty()) {
        const int first_inserted = static_cast<int>(rows_.size());
        beginInsertRows(
            {},
            first_inserted,
            first_inserted + static_cast<int>(suffix_rows.size()) - 1
        );
        rows_.append(std::move(suffix_rows));
        for (int row = first_inserted; row < rows_.size(); ++row) {
            const auto& items = rows_.at(row).items;
            for (int column = 0; column < items.size(); ++column) {
                item_positions_.insert(
                    representationKey(items.at(column).toMap()),
                    {row, column, -1}
                );
            }
        }
        endInsertRows();
    }
    projected_source_count_ = source_count;
    if (diagnostics.enabled()) {
        diagnostics.record(
            ReviewDiagnostics::Stage::LayoutAppend,
            diagnostic_clock.elapsed(),
            source_count
        );
    }
}

bool JustifiedReviewLayoutModel::appendGroupedSourceRows(const QVector<Section>& next_sections) {
    if (!source_model_ || full_rebuild_pending_ || rows_.isEmpty()
        || available_width_ < MIN_LAYOUT_WIDTH || sections_.isEmpty()
        || sections_.size() != next_sections.size()) {
        return false;
    }
    const int source_count = source_model_->rowCount();
    if (source_count <= projected_source_count_) {
        return false;
    }

    // The fast path is valid only when every existing section keeps its exact
    // identity, labels and membership prefix. A sort, filter, regroup or edit
    // to an old member still takes the full rebuild path.
    QHash<QString, int> appended_membership;
    qsizetype previous_members = 0;
    for (int section_index = 0; section_index < sections_.size(); ++section_index) {
        const Section& old = sections_.at(section_index);
        const Section& next = next_sections.at(section_index);
        if (old.key != next.key || old.title != next.title || old.subtitle != next.subtitle
            || old.navigation_label != next.navigation_label
            || old.navigation_short_label != next.navigation_short_label
            || old.navigation_major_label != next.navigation_major_label
            || next.representation_keys.size() < old.representation_keys.size()
            || !std::equal(
                old.representation_keys.cbegin(),
                old.representation_keys.cend(),
                next.representation_keys.cbegin()
            )) {
            return false;
        }
        previous_members += old.representation_keys.size();
        for (qsizetype member = old.representation_keys.size();
             member < next.representation_keys.size();
             ++member) {
            const QString& key = next.representation_keys.at(member);
            if (key.isEmpty() || item_positions_.contains(key)
                || appended_membership.contains(key)) {
                return false;
            }
            appended_membership.insert(key, section_index);
        }
    }
    if (previous_members != projected_source_count_
        || item_positions_.size() != projected_source_count_
        || appended_membership.size() != source_count - projected_source_count_) {
        return false;
    }

    QVector<int> header_rows;
    header_rows.reserve(sections_.size());
    for (int row = 0; row < rows_.size(); ++row) {
        const Row& entry = rows_.at(row);
        if (entry.kind == QStringLiteral("section")) {
            const int ordinal = static_cast<int>(header_rows.size());
            if (ordinal >= sections_.size() || entry.section_key != sections_.at(ordinal).key
                || entry.section_item_count != sections_.at(ordinal).representation_keys.size()) {
                return false;
            }
            header_rows.push_back(row);
        } else if (
            entry.kind != QStringLiteral("photos") || header_rows.isEmpty()
            || entry.section_key != sections_.at(header_rows.size() - 1).key
        ) {
            // A partial grouping leaves fallback photos after its sections.
            return false;
        }
    }
    if (header_rows.size() != sections_.size()) {
        return false;
    }

    const ProjectionRoles roles = projectionRoles(source_model_->roleNames());
    QHash<QString, QVariantMap> appended_items;
    appended_items.reserve(appended_membership.size());
    for (int source_row = projected_source_count_; source_row < source_count; ++source_row) {
        QVariantMap item = sourceItem(source_row, roles);
        const QString key = representationKey(item);
        if (item.isEmpty() || !appended_membership.contains(key) || appended_items.contains(key)) {
            return false;
        }
        appended_items.insert(key, std::move(item));
    }

    struct SectionTail final {
        int section_index;
        int header_row;
        int tail_row;
        bool replace_tail;
        QVector<Row> suffix_rows;
        int member_count;
    };
    QVector<SectionTail> tails;
    tails.reserve(sections_.size());
    for (int section_index = 0; section_index < sections_.size(); ++section_index) {
        const Section& old = sections_.at(section_index);
        const Section& next = next_sections.at(section_index);
        if (next.representation_keys.size() == old.representation_keys.size()) {
            continue;
        }
        const int header_row = header_rows.at(section_index);
        const int tail_row =
            (section_index + 1 < header_rows.size() ? header_rows.at(section_index + 1)
                                                    : static_cast<int>(rows_.size()))
            - 1;
        if (tail_row <= header_row || rows_.at(tail_row).kind != QStringLiteral("photos")
            || rows_.at(tail_row).section_key != old.key) {
            return false;
        }
        const bool replace_tail = !rows_.at(tail_row).justified;
        QVariantList suffix_items = replace_tail ? rows_.at(tail_row).items : QVariantList{};
        for (qsizetype member = old.representation_keys.size();
             member < next.representation_keys.size();
             ++member) {
            suffix_items.push_back(appended_items.value(next.representation_keys.at(member)));
        }
        QVector<Row> suffix_rows;
        appendPhotoRows(
            suffix_rows,
            std::move(suffix_items),
            old.key,
            replace_tail && tail_row == header_row + 1
        );
        if (suffix_rows.isEmpty()) {
            return false;
        }
        tails.push_back({
            section_index,
            header_row,
            tail_row,
            replace_tail,
            std::move(suffix_rows),
            static_cast<int>(next.representation_keys.size()),
        });
    }

    auto& diagnostics = ReviewDiagnostics::instance();
    QElapsedTimer diagnostic_clock;
    if (diagnostics.enabled()) {
        diagnostic_clock.start();
    }
    rebuild_timer_.stop();
    append_pending_ = false;
    for (auto tail = tails.rbegin(); tail != tails.rend(); ++tail) {
        rows_[tail->header_row].section_item_count = tail->member_count;
        emit dataChanged(
            index(tail->header_row, 0),
            index(tail->header_row, 0),
            {SectionItemCountRole}
        );
        if (tail->replace_tail) {
            rows_[tail->tail_row] = std::move(tail->suffix_rows.front());
            tail->suffix_rows.removeFirst();
            const auto& items = rows_.at(tail->tail_row).items;
            for (int column = 0; column < items.size(); ++column) {
                item_positions_.insert(
                    representationKey(items.at(column).toMap()),
                    {tail->tail_row - tail->header_row, column, tail->section_index}
                );
            }
            emit dataChanged(
                index(tail->tail_row, 0),
                index(tail->tail_row, 0),
                {ItemsRole, RowHeightRole, UsedWidthRole}
            );
        }
        if (!tail->suffix_rows.isEmpty()) {
            const int first_inserted = tail->tail_row + 1;
            const int inserted_count = static_cast<int>(tail->suffix_rows.size());
            beginInsertRows({}, first_inserted, first_inserted + inserted_count - 1);
            int insertion_row = first_inserted;
            for (Row& row : tail->suffix_rows) {
                rows_.insert(rows_.begin() + insertion_row++, std::move(row));
            }
            for (int row = first_inserted; row < first_inserted + inserted_count; ++row) {
                const auto& items = rows_.at(row).items;
                for (int column = 0; column < items.size(); ++column) {
                    item_positions_.insert(
                        representationKey(items.at(column).toMap()),
                        {row - tail->header_row, column, tail->section_index}
                    );
                }
            }
            for (int section = tail->section_index + 1; section < section_header_rows_.size();
                 ++section) {
                section_header_rows_[section] += inserted_count;
            }
            endInsertRows();
        }
    }
    projected_source_count_ = source_count;
    emit sectionAnchorsChanged();
    if (diagnostics.enabled()) {
        diagnostics.record(
            ReviewDiagnostics::Stage::LayoutAppend,
            diagnostic_clock.elapsed(),
            source_count
        );
    }
    return true;
}

void JustifiedReviewLayoutModel::updateSourceItems(
    const QModelIndex& first,
    const QModelIndex& last,
    const QList<int>& roles
) {
    // A pending full rebuild supersedes updates. An append can still carry
    // independent badge changes for rows already in the layout.
    if (source_model_ == nullptr || full_rebuild_pending_) {
        return;
    }
    const auto names = source_model_->roleNames();
    const int photo_role = names.key("photoId", -1);
    const int representation_role = names.key("representationId", -1);
    if (roles.isEmpty() || !first.isValid() || !last.isValid() || first.parent().isValid()
        || last.parent().isValid() || photo_role < 0 || representation_role < 0) {
        requestRebuild();
        return;
    }
    for (const int role : roles) {
        const auto name = names.value(role);
        if (name.isEmpty() || name == "photoId" || name == "representationId"
            || name == "visualWidth" || name == "visualHeight") {
            requestRebuild();
            return;
        }
    }
    QSet<int> changed_rows;
    for (int row = first.row(); row <= last.row(); ++row) {
        const auto source_index = source_model_->index(row, 0);
        const QString key = source_model_->data(source_index, photo_role).toString() + QChar{0x001f}
                            + source_model_->data(source_index, representation_role).toString();
        const auto position = item_positions_.constFind(key);
        if (position == item_positions_.cend()) {
            requestRebuild();
            return;
        }
        // Photo roles are materialized only for visible rows. Their current
        // source values need no second resident copy in the layout.
        changed_rows.insert(absoluteRow(position.value()));
    }
    for (const int row : changed_rows) {
        emit dataChanged(index(row, 0), index(row, 0), {ItemsRole});
    }
}

void JustifiedReviewLayoutModel::rebuild() {
    auto& diagnostics = ReviewDiagnostics::instance();
    QElapsedTimer diagnostic_clock;
    if (diagnostics.enabled()) {
        diagnostic_clock.start();
    }
    rebuild_timer_.stop();
    full_rebuild_pending_ = false;
    append_pending_ = false;
    QVector<Row> next_rows;
    if (source_model_ != nullptr && available_width_ >= MIN_LAYOUT_WIDTH) {
        const int source_count = source_model_->rowCount();
        QVariantList source_items;
        source_items.reserve(source_count);
        const ProjectionRoles roles = projectionRoles(source_model_->roleNames());
        for (int source_row = 0; source_row < source_count; ++source_row) {
            QVariantMap item = sourceItem(source_row, roles);
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
                appendPhotoRows(next_rows, std::move(members), section.key);
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
    projected_source_count_ = source_model_ ? source_model_->rowCount() : 0;
    item_positions_.clear();
    section_header_rows_.clear();
    int current_section_ordinal = -1;
    int current_section_header = -1;
    for (int row = 0; row < rows_.size(); ++row) {
        const Row& layout_row = rows_.at(row);
        if (layout_row.kind == QStringLiteral("section")) {
            section_header_rows_.push_back(row);
            current_section_ordinal = static_cast<int>(section_header_rows_.size()) - 1;
            current_section_header = row;
        }
        const auto& items = layout_row.items;
        for (int column = 0; column < items.size(); ++column) {
            const bool grouped = !layout_row.section_key.isEmpty() && current_section_ordinal >= 0;
            item_positions_.insert(
                representationKey(items.at(column).toMap()),
                {grouped ? row - current_section_header : row,
                 column,
                 grouped ? current_section_ordinal : -1}
            );
        }
    }
    endResetModel();
    emit sectionAnchorsChanged();
    if (diagnostics.enabled()) {
        diagnostics.record(
            ReviewDiagnostics::Stage::LayoutRebuild,
            diagnostic_clock.elapsed(),
            projected_source_count_
        );
    }
}

void JustifiedReviewLayoutModel::appendPhotoRows(
    QVector<Row>& rows,
    QVariantList items,
    const QString& section_key,
    const bool initial_section_row
) const {
    QVariantList current_items;
    qreal current_aspect_sum = 0.0;
    bool first_section_row = initial_section_row;
    const auto append_row =
        [&rows,
         this,
         &section_key,
         &first_section_row](QVariantList row_items, const qreal aspect_sum, const bool justify) {
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
            const bool section_lead_row = first_section_row && !section_key.isEmpty();
            qreal x = 0.0;
            for (QVariant& variant : row_items) {
                QVariantMap item = variant.toMap();
                const qreal width = aspectRatio(item) * static_cast<qreal>(height);
                item.insert(QStringLiteral("layoutX"), roundedDimension(x));
                item.insert(QStringLiteral("layoutWidth"), roundedDimension(width));
                item.insert(QStringLiteral("sectionKey"), section_key);
                item.insert(QStringLiteral("sectionLeadRow"), section_lead_row);
                variant = item;
                x += width + static_cast<qreal>(spacing_);
            }
            Row row;
            row.items = std::move(row_items);
            row.section_key = section_key;
            row.height = height;
            row.used_width = roundedDimension(std::max(0.0, x - spacing_));
            row.justified = justify;
            rows.append(std::move(row));
            first_section_row = false;
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
