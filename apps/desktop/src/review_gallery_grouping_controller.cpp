#include "review_gallery_grouping_controller.hpp"

#include "review_diagnostics.hpp"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QLocale>
#include <QTimeZone>
#include <QVariantMap>

#include <algorithm>
#include <iterator>
#include <utility>

namespace {

constexpr QChar REPRESENTATION_SEPARATOR{0x001f};
constexpr QChar GROUP_SEPARATOR{0x001d};

struct DimensionDefinition final {
    const char* key;
    const char* category;
    const char* label;
    const char* category_label;
    const char* selection_style;
};

constexpr DimensionDefinition DIMENSIONS[] = {
    {"date.year",
     "date",
     QT_TRANSLATE_NOOP("ReviewGalleryGroupingController", "Year"),
     QT_TRANSLATE_NOOP("ReviewGalleryGroupingController", "DATE"),
     "exclusive"},
    {"date.month",
     "date",
     QT_TRANSLATE_NOOP("ReviewGalleryGroupingController", "Month"),
     QT_TRANSLATE_NOOP("ReviewGalleryGroupingController", "DATE"),
     "exclusive"},
    {"date.week",
     "date",
     QT_TRANSLATE_NOOP("ReviewGalleryGroupingController", "Week"),
     QT_TRANSLATE_NOOP("ReviewGalleryGroupingController", "DATE"),
     "exclusive"},
    {"place.name",
     "place",
     QT_TRANSLATE_NOOP("ReviewGalleryGroupingController", "Location"),
     QT_TRANSLATE_NOOP("ReviewGalleryGroupingController", "LOCATION"),
     "multiple"},
};

struct GroupingValue final {
    QString key;
    QString label;
    QString navigation_short_label;
    QString navigation_major_label;
};

struct BaseSection final {
    QString key;
    QString title;
    QString subtitle;
    QStringList representation_keys;
};

struct CompiledSection final {
    QString key;
    QString title;
    QString subtitle;
    QString navigation_label;
    QString navigation_short_label;
    QString navigation_major_label;
    QStringList representation_keys;
};

[[nodiscard]] int roleForName(const QHash<int, QByteArray>& roles, const QByteArrayView name) {
    for (auto iterator = roles.cbegin(); iterator != roles.cend(); ++iterator) {
        if (iterator.value() == name) {
            return iterator.key();
        }
    }
    return -1;
}

[[nodiscard]] QString representationKey(
    QAbstractItemModel& model,
    const QModelIndex& index,
    const int photo_role,
    const int representation_role
) {
    if (photo_role < 0 || representation_role < 0) {
        return {};
    }
    const QString photo_id = model.data(index, photo_role).toString();
    const QString representation_id = model.data(index, representation_role).toString();
    if (photo_id.isEmpty() || representation_id.isEmpty()) {
        return {};
    }
    return photo_id + REPRESENTATION_SEPARATOR + representation_id;
}

[[nodiscard]] QVector<BaseSection> normalizedBaseSections(const QVariantList& source) {
    QVector<BaseSection> result;
    QSet<QString> seen;
    result.reserve(source.size());
    for (const QVariant& value : source) {
        const QVariantMap map = value.toMap();
        const QString key = map.value(QStringLiteral("key")).toString().trimmed();
        const QString title = map.value(QStringLiteral("title")).toString().trimmed();
        if (key.isEmpty() || title.isEmpty() || seen.contains(key)) {
            continue;
        }
        const QVariantList raw_keys = map.value(QStringLiteral("representationKeys")).toList();
        QStringList representation_keys;
        representation_keys.reserve(raw_keys.size());
        QSet<QString> seen_representation_keys;
        seen_representation_keys.reserve(raw_keys.size());
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
        seen.insert(key);
        result.push_back({
            .key = key,
            .title = title,
            .subtitle = map.value(QStringLiteral("subtitle")).toString().trimmed(),
            .representation_keys = std::move(representation_keys),
        });
    }
    return result;
}

[[nodiscard]] QVariantList baseSectionVariants(const QVector<BaseSection>& sections) {
    QVariantList result;
    result.reserve(sections.size());
    for (const BaseSection& section : sections) {
        result.push_back(
            QVariantMap{
                {QStringLiteral("key"), section.key},
                {QStringLiteral("title"), section.title},
                {QStringLiteral("subtitle"), section.subtitle},
                {QStringLiteral("representationKeys"), section.representation_keys},
            }
        );
    }
    return result;
}

[[nodiscard]] QDate captureDate(
    QAbstractItemModel& model,
    const QModelIndex& index,
    const int capture_day_role,
    const int captured_at_role,
    QHash<QString, QDate>& parsed_days
) {
    if (capture_day_role >= 0) {
        const QString day_text = model.data(index, capture_day_role).toString();
        auto cached = parsed_days.constFind(day_text);
        QDate date;
        if (cached != parsed_days.cend()) {
            date = cached.value();
        } else {
            date = QDate::fromString(day_text, Qt::ISODate);
            if (parsed_days.size() < 4096) {
                parsed_days.insert(day_text, date);
            }
        }
        if (date.isValid()) {
            return date;
        }
    }
    if (captured_at_role < 0) {
        return {};
    }
    const qint64 seconds = model.data(index, captured_at_role).toLongLong();
    return seconds == 0 ? QDate{} : QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC).date();
}

[[nodiscard]] GroupingValue
dateGroupingValue(const QString& dimension, const QDate& date, const QLocale& locale) {
    if (!date.isValid()) {
        return {
            QStringLiteral("unknown"),
            QCoreApplication::translate("ReviewGalleryGroupingController", "Unknown date"),
            QStringLiteral("?"),
            {},
        };
    }
    if (dimension == QStringLiteral("date.year")) {
        const QString year = QString::number(date.year());
        return {year, year, year, year};
    }
    if (dimension == QStringLiteral("date.month")) {
        return {
            QStringLiteral("%1-%2").arg(date.year()).arg(date.month(), 2, 10, QChar{'0'}),
            QCoreApplication::translate("ReviewGalleryGroupingController", "%2 %1")
                .arg(date.year())
                .arg(locale.monthName(date.month(), QLocale::LongFormat)),
            locale.monthName(date.month(), QLocale::ShortFormat),
            QString::number(date.year()),
        };
    }
    int week_year = 0;
    const int week = date.weekNumber(&week_year);
    return {
        QStringLiteral("%1-W%2").arg(week_year).arg(week, 2, 10, QChar{'0'}),
        QCoreApplication::translate("ReviewGalleryGroupingController", "%1 · Week %2")
            .arg(week_year)
            .arg(week),
        QStringLiteral("W%1").arg(week),
        QString::number(week_year),
    };
}

[[nodiscard]] GroupingValue placeGroupingValue(const QString& raw_place) {
    const QString place = raw_place.simplified();
    if (place.isEmpty()) {
        return {
            QStringLiteral("unknown"),
            QCoreApplication::translate("ReviewGalleryGroupingController", "Unknown location"),
            QStringLiteral("?"),
            {},
        };
    }
    return {place.toCaseFolded(), place, place.left(1).toUpper(), {}};
}

template <typename AddSection>
void compileRows(
    QAbstractItemModel& model,
    const int first,
    const int last,
    const QVector<BaseSection>& base_sections,
    const QHash<QString, int>& base_membership,
    const QSet<QString>& selected_dimensions,
    AddSection&& add_section
) {
    const QHash<int, QByteArray> roles = model.roleNames();
    const int photo_role = roleForName(roles, "photoId");
    const int representation_role = roleForName(roles, "representationId");
    const int capture_day_role = roleForName(roles, "captureDay");
    const int captured_at_role = roleForName(roles, "capturedAtUnixSeconds");
    const int place_role = roleForName(roles, "placeName");
    const QLocale locale;
    QString date_dimension;
    for (const DimensionDefinition& definition : DIMENSIONS) {
        const QString dimension = QString::fromLatin1(definition.key);
        if (dimension.startsWith(QStringLiteral("date."))
            && selected_dimensions.contains(dimension)) {
            date_dimension = dimension;
            break;
        }
    }
    const bool group_by_place = selected_dimensions.contains(QStringLiteral("place.name"));
    QHash<QString, QDate> parsed_days;
    QHash<qint64, GroupingValue> date_values;
    QHash<QString, GroupingValue> place_values;
    for (int row = first; row <= last; ++row) {
        const QModelIndex index = model.index(row, 0);
        const QString representation_key =
            representationKey(model, index, photo_role, representation_role);
        if (representation_key.isEmpty()) {
            continue;
        }

        QStringList key_parts;
        QStringList title_parts;
        QString subtitle;
        QString navigation_label;
        QString navigation_short_label;
        QString navigation_major_label;
        if (!base_sections.isEmpty()) {
            const int base_index = base_membership.value(representation_key, -1);
            if (base_index >= 0) {
                const BaseSection& base = base_sections.at(base_index);
                key_parts.push_back(QStringLiteral("base:") + base.key);
                title_parts.push_back(base.title);
                subtitle = base.subtitle;
            } else {
                key_parts.push_back(QStringLiteral("base:other"));
                title_parts.push_back(
                    QCoreApplication::translate("ReviewGalleryGroupingController", "Other photos")
                );
            }
        }

        const auto add_dimension = [&](const QString& dimension, const GroupingValue& value) {
            key_parts.push_back(dimension + QStringLiteral(":") + value.key);
            title_parts.push_back(value.label);
            if (navigation_label.isEmpty()) {
                navigation_label = value.label;
                navigation_short_label = value.navigation_short_label;
                navigation_major_label = value.navigation_major_label;
            }
        };
        if (!date_dimension.isEmpty()) {
            const QDate date =
                captureDate(model, index, capture_day_role, captured_at_role, parsed_days);
            const qint64 day_key = date.isValid() ? date.toJulianDay() : 0;
            const auto cached = date_values.constFind(day_key);
            GroupingValue value = cached != date_values.cend()
                                      ? cached.value()
                                      : dateGroupingValue(date_dimension, date, locale);
            if (cached == date_values.cend() && date_values.size() < 4096) {
                date_values.insert(day_key, value);
            }
            add_dimension(date_dimension, value);
        }
        if (group_by_place) {
            const QString raw_place =
                place_role < 0 ? QString{} : model.data(index, place_role).toString();
            const auto cached = place_values.constFind(raw_place);
            GroupingValue value =
                cached != place_values.cend() ? cached.value() : placeGroupingValue(raw_place);
            if (cached == place_values.cend() && place_values.size() < 4096) {
                place_values.insert(raw_place, value);
            }
            add_dimension(QStringLiteral("place.name"), value);
        }

        const QString group_key =
            QStringLiteral("gallery-group:") + key_parts.join(GROUP_SEPARATOR);
        add_section(group_key, representation_key, [&]() {
            return CompiledSection{
                .key = group_key,
                .title = title_parts.join(
                    QCoreApplication::translate("ReviewGalleryGroupingController", " · ")
                ),
                .subtitle = subtitle,
                .navigation_label = navigation_label,
                .navigation_short_label = navigation_short_label,
                .navigation_major_label = navigation_major_label,
                .representation_keys = {},
            };
        });
    }
}

[[nodiscard]] QVariantMap sectionVariant(const CompiledSection& section) {
    return {
        {QStringLiteral("key"), section.key},
        {QStringLiteral("title"), section.title},
        {QStringLiteral("subtitle"), section.subtitle},
        {QStringLiteral("navigationLabel"), section.navigation_label},
        {QStringLiteral("navigationShortLabel"), section.navigation_short_label},
        {QStringLiteral("navigationMajorLabel"), section.navigation_major_label},
        {QStringLiteral("representationKeys"), section.representation_keys},
    };
}

} // namespace

ReviewGalleryGroupingController::ReviewGalleryGroupingController(QObject* const parent) :
    QObject(parent) {}

ReviewGalleryGroupingController::~ReviewGalleryGroupingController() {
    disconnectSourceModel();
}

QAbstractItemModel* ReviewGalleryGroupingController::sourceModel() const noexcept {
    return source_model_;
}

void ReviewGalleryGroupingController::setSourceModel(QAbstractItemModel* const source_model) {
    if (source_model_ == source_model) {
        return;
    }
    disconnectSourceModel();
    source_model_ = source_model;
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
            [this](const QModelIndex& parent, int first, int last) {
                appendRows(parent, first, last);
            }
        ));
        source_connections_.append(connect(
            source_model_,
            &QAbstractItemModel::rowsRemoved,
            this,
            [rebuild](const QModelIndex&, int, int) { rebuild(); }
        ));
        source_connections_.append(
            connect(source_model_, &QAbstractItemModel::rowsMoved, this, rebuild)
        );
        source_connections_.append(connect(
            source_model_,
            &QAbstractItemModel::dataChanged,
            this,
            [this](const QModelIndex&, const QModelIndex&, const QList<int>& changed_roles) {
                if (selected_dimensions_.isEmpty()) {
                    return;
                }
                const auto names = source_model_->roleNames();
                const bool affects_grouping =
                    changed_roles.isEmpty()
                    || std::any_of(changed_roles.cbegin(), changed_roles.cend(), [&](int role) {
                           const auto name = names.value(role);
                           return name.isEmpty() || name == "photoId" || name == "representationId"
                                  || name == "captureDay" || name == "capturedAtUnixSeconds"
                                  || name == "placeName";
                       });
                if (affects_grouping) {
                    this->rebuild();
                }
            }
        ));
        source_connections_.append(connect(source_model_, &QObject::destroyed, this, [this]() {
            source_model_ = nullptr;
            source_connections_.clear();
            this->rebuild();
            emit sourceModelChanged();
        }));
    }
    rebuild();
    emit sourceModelChanged();
}

QVariantList ReviewGalleryGroupingController::baseSections() const {
    return base_sections_;
}

void ReviewGalleryGroupingController::setBaseSections(const QVariantList& sections) {
    if (base_sections_ == sections) {
        return;
    }
    base_sections_ = sections;
    rebuild();
    emit baseSectionsChanged();
}

QVariantList ReviewGalleryGroupingController::dimensions() const {
    QVariantList result;
    result.reserve(std::size(DIMENSIONS));
    QString previous_category;
    for (const DimensionDefinition& definition : DIMENSIONS) {
        const QString key = QString::fromLatin1(definition.key);
        const QString category = QString::fromLatin1(definition.category);
        result.push_back(
            QVariantMap{
                {QStringLiteral("key"), key},
                {QStringLiteral("category"), category},
                {QStringLiteral("categoryLabel"), tr(definition.category_label)},
                {QStringLiteral("label"), tr(definition.label)},
                {QStringLiteral("selectionStyle"), QString::fromLatin1(definition.selection_style)},
                {QStringLiteral("firstInCategory"), category != previous_category},
                {QStringLiteral("selected"), selected_dimensions_.contains(key)},
            }
        );
        previous_category = category;
    }
    return result;
}

QVariantList ReviewGalleryGroupingController::sections() const {
    return sections_;
}

int ReviewGalleryGroupingController::activeDimensionCount() const noexcept {
    return static_cast<int>(selected_dimensions_.size());
}

QString ReviewGalleryGroupingController::activeSummary() const {
    QStringList labels;
    for (const DimensionDefinition& definition : DIMENSIONS) {
        if (selected_dimensions_.contains(QString::fromLatin1(definition.key))) {
            labels.push_back(tr(definition.label));
        }
    }
    return labels.join(tr(" + "));
}

void ReviewGalleryGroupingController::setDimensionSelected(
    const QString& raw_key,
    const bool selected
) {
    const QString key = raw_key.trimmed();
    const auto definition = std::find_if(
        std::begin(DIMENSIONS),
        std::end(DIMENSIONS),
        [&key](const DimensionDefinition& candidate) {
            return key == QString::fromLatin1(candidate.key);
        }
    );
    if (definition == std::end(DIMENSIONS)) {
        return;
    }
    QSet<QString> next = selected_dimensions_;
    if (selected
        && QString::fromLatin1(definition->selection_style) == QStringLiteral("exclusive")) {
        const QString category = QString::fromLatin1(definition->category);
        for (const DimensionDefinition& candidate : DIMENSIONS) {
            if (category == QString::fromLatin1(candidate.category)) {
                next.remove(QString::fromLatin1(candidate.key));
            }
        }
    }
    if (selected) {
        next.insert(key);
    } else {
        next.remove(key);
    }
    if (next == selected_dimensions_) {
        return;
    }
    selected_dimensions_ = std::move(next);
    rebuild();
    emit groupingChanged();
}

void ReviewGalleryGroupingController::clearGrouping() {
    if (selected_dimensions_.isEmpty()) {
        return;
    }
    selected_dimensions_.clear();
    rebuild();
    emit groupingChanged();
}

void ReviewGalleryGroupingController::retranslateUi() {
    rebuild();
    emit groupingChanged();
}

void ReviewGalleryGroupingController::disconnectSourceModel() {
    for (const QMetaObject::Connection& connection : std::as_const(source_connections_)) {
        disconnect(connection);
    }
    source_connections_.clear();
}

void ReviewGalleryGroupingController::appendRows(
    const QModelIndex& parent,
    const int first,
    const int last
) {
    if (selected_dimensions_.isEmpty()) {
        projected_source_count_ = source_model_ ? source_model_->rowCount() : 0;
        return;
    }
    if (source_model_ == nullptr || parent.isValid() || !base_sections_.isEmpty()
        || first != projected_source_count_ || last != source_model_->rowCount() - 1) {
        rebuild();
        return;
    }

    auto& diagnostics = ReviewDiagnostics::instance();
    QElapsedTimer diagnostic_clock;
    if (diagnostics.enabled()) {
        diagnostic_clock.start();
    }
    QHash<QString, int> next_indices = section_indices_;
    QHash<int, QStringList> added_members;
    QVector<CompiledSection> new_sections;
    const int existing_count = static_cast<int>(sections_.size());
    compileRows(
        *source_model_,
        first,
        last,
        {},
        {},
        selected_dimensions_,
        [&](const QString& group_key, const QString& member, auto&& make_section) {
            const int group_index = next_indices.value(group_key, -1);
            if (group_index < 0) {
                next_indices.insert(
                    group_key,
                    existing_count + static_cast<int>(new_sections.size())
                );
                new_sections.push_back(make_section());
                new_sections.last().representation_keys.push_back(member);
            } else if (group_index < existing_count) {
                added_members[group_index].push_back(member);
            } else {
                new_sections[group_index - existing_count].representation_keys.push_back(member);
            }
        }
    );

    projected_source_count_ = source_model_->rowCount();
    if (added_members.isEmpty() && new_sections.isEmpty()) {
        if (diagnostics.enabled()) {
            diagnostics.record(
                ReviewDiagnostics::Stage::GroupingCompile,
                diagnostic_clock.elapsed(),
                last - first + 1
            );
        }
        return;
    }
    QVariantList next_sections = sections_;
    for (auto iterator = added_members.cbegin(); iterator != added_members.cend(); ++iterator) {
        QVariantMap section = next_sections.at(iterator.key()).toMap();
        QStringList members = section.value(QStringLiteral("representationKeys")).toStringList();
        members.append(iterator.value());
        section.insert(QStringLiteral("representationKeys"), members);
        next_sections[iterator.key()] = section;
    }
    for (const CompiledSection& section : std::as_const(new_sections)) {
        next_sections.push_back(sectionVariant(section));
    }
    section_indices_ = std::move(next_indices);
    sections_ = std::move(next_sections);
    if (diagnostics.enabled()) {
        diagnostics.record(
            ReviewDiagnostics::Stage::GroupingCompile,
            diagnostic_clock.elapsed(),
            last - first + 1
        );
    }
    emit sectionsChanged();
}

void ReviewGalleryGroupingController::rebuild() {
    auto& diagnostics = ReviewDiagnostics::instance();
    QElapsedTimer diagnostic_clock;
    if (diagnostics.enabled()) {
        diagnostic_clock.start();
    }
    const QVector<BaseSection> base_sections = normalizedBaseSections(base_sections_);
    QVariantList next_sections;
    QHash<QString, int> next_section_indices;
    if (selected_dimensions_.isEmpty()) {
        next_sections = baseSectionVariants(base_sections);
    } else if (source_model_ != nullptr) {
        QHash<QString, int> base_membership;
        for (int index = 0; index < base_sections.size(); ++index) {
            for (const QString& key : base_sections.at(index).representation_keys) {
                if (!base_membership.contains(key)) {
                    base_membership.insert(key, index);
                }
            }
        }

        QVector<CompiledSection> compiled;
        compileRows(
            *source_model_,
            0,
            source_model_->rowCount() - 1,
            base_sections,
            base_membership,
            selected_dimensions_,
            [&](const QString& group_key, const QString& member, auto&& make_section) {
                const int group_index = next_section_indices.value(group_key, -1);
                if (group_index < 0) {
                    next_section_indices.insert(group_key, static_cast<int>(compiled.size()));
                    compiled.push_back(make_section());
                    compiled.last().representation_keys.push_back(member);
                } else {
                    compiled[group_index].representation_keys.push_back(member);
                }
            }
        );

        next_sections.reserve(compiled.size());
        for (const CompiledSection& section : std::as_const(compiled)) {
            next_sections.push_back(sectionVariant(section));
        }
    }

    projected_source_count_ = source_model_ ? source_model_->rowCount() : 0;
    section_indices_ = std::move(next_section_indices);
    if (sections_ == next_sections) {
        if (diagnostics.enabled()) {
            diagnostics.record(
                ReviewDiagnostics::Stage::GroupingCompile,
                diagnostic_clock.elapsed(),
                source_model_ ? source_model_->rowCount() : 0
            );
        }
        return;
    }
    sections_ = std::move(next_sections);
    if (diagnostics.enabled()) {
        diagnostics.record(
            ReviewDiagnostics::Stage::GroupingCompile,
            diagnostic_clock.elapsed(),
            source_model_ ? source_model_->rowCount() : 0
        );
    }
    emit sectionsChanged();
}
