#include "review_gallery_grouping_controller.hpp"

#include <QAbstractListModel>
#include <QCoreApplication>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "review gallery grouping contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Photo final {
    QString photo_id;
    QString representation_id;
    QString capture_day;
    qint64 captured_at = 0;
    QString place_name;
};

class PhotoModel final : public QAbstractListModel {
  public:
    enum Role {
        PhotoIdRole = Qt::UserRole + 1,
        RepresentationIdRole,
        CaptureDayRole,
        CapturedAtRole,
        PlaceNameRole,
        RatingRole,
    };

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(photos_.size());
    }

    mutable int reads = 0;
    void notify(int role) {
        emit dataChanged(index(0, 0), index(0, 0), {role});
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override {
        ++reads;
        if (!index.isValid() || index.row() < 0 || index.row() >= photos_.size()) {
            return {};
        }
        const Photo& photo = photos_.at(index.row());
        switch (role) {
        case PhotoIdRole:
            return photo.photo_id;
        case RepresentationIdRole:
            return photo.representation_id;
        case CaptureDayRole:
            return photo.capture_day;
        case CapturedAtRole:
            return photo.captured_at;
        case PlaceNameRole:
            return photo.place_name;
        default:
            return {};
        }
    }

    [[nodiscard]] QHash<int, QByteArray> roleNames() const override {
        return {
            {PhotoIdRole, "photoId"},
            {RepresentationIdRole, "representationId"},
            {CaptureDayRole, "captureDay"},
            {CapturedAtRole, "capturedAtUnixSeconds"},
            {PlaceNameRole, "placeName"},
            {RatingRole, "decisionRating"},
        };
    }

    void replace(QVector<Photo> photos) {
        beginResetModel();
        photos_ = std::move(photos);
        endResetModel();
    }

  private:
    QVector<Photo> photos_;
};

[[nodiscard]] QString representationKey(const char* const id) {
    const QString value = QString::fromLatin1(id);
    return value + QChar{0x001f} + QStringLiteral("representation-") + value;
}

[[nodiscard]] QVariantMap section(const QVariantList& sections, const int index) {
    return sections.at(index).toMap();
}

void base_sections_pass_through_without_user_grouping() {
    ReviewGalleryGroupingController grouping;
    grouping.setBaseSections({
        QVariantMap{
            {QStringLiteral("key"), QStringLiteral("high")},
            {QStringLiteral("title"), QStringLiteral("Highly related")},
            {QStringLiteral("representationKeys"),
             QStringList{representationKey("a"), representationKey("b")}},
        },
    });

    require(grouping.sections().size() == 1, "semantic grouping remains available by itself");
    require(
        section(grouping.sections(), 0).value(QStringLiteral("key")).toString()
            == QStringLiteral("high"),
        "the search owner retains its section identity when no extra dimension is selected"
    );
}

void large_candidate_section_keeps_unique_members_in_order() {
    QVariantList members;
    members.reserve(10'001);
    for (int index = 0; index < 10'000; ++index) {
        members.push_back(QStringLiteral("photo-") + QString::number(index));
    }
    members.push_back(QStringLiteral("photo-42"));
    ReviewGalleryGroupingController grouping;
    grouping.setBaseSections({QVariantMap{
        {QStringLiteral("key"), QStringLiteral("large-candidate-stack")},
        {QStringLiteral("title"), QStringLiteral("Candidates")},
        {QStringLiteral("representationKeys"), members},
    }});
    const QStringList result =
        section(grouping.sections(), 0).value(QStringLiteral("representationKeys")).toStringList();
    require(
        result.size() == 10'000 && result.first() == QStringLiteral("photo-0")
            && result.last() == QStringLiteral("photo-9999")
            && result.at(42) == QStringLiteral("photo-42"),
        "a large candidate section must keep first occurrence order and remove duplicates"
    );
}

void independent_dimensions_form_composite_groups() {
    PhotoModel photos;
    photos.replace({
        {QStringLiteral("a"),
         QStringLiteral("representation-a"),
         QStringLiteral("2024-03-18"),
         0,
         QStringLiteral("Beijing")},
        {QStringLiteral("b"),
         QStringLiteral("representation-b"),
         QStringLiteral("2024-03-19"),
         0,
         QStringLiteral("Shanghai")},
        {QStringLiteral("c"),
         QStringLiteral("representation-c"),
         QStringLiteral("2024-04-02"),
         0,
         QStringLiteral("Beijing")},
        {QStringLiteral("d"), QStringLiteral("representation-d"), QString{}, 0, QString{}},
    });

    ReviewGalleryGroupingController grouping;
    grouping.setSourceModel(&photos);
    grouping.setBaseSections({
        QVariantMap{
            {QStringLiteral("key"), QStringLiteral("high")},
            {QStringLiteral("title"), QStringLiteral("Highly related")},
            {QStringLiteral("subtitle"), QStringLiteral("Closest matches")},
            {QStringLiteral("representationKeys"),
             QStringList{representationKey("a"), representationKey("b")}},
        },
        QVariantMap{
            {QStringLiteral("key"), QStringLiteral("possible")},
            {QStringLiteral("title"), QStringLiteral("Possibly related")},
            {QStringLiteral("representationKeys"),
             QStringList{representationKey("c"), representationKey("d")}},
        },
    });
    grouping.setDimensionSelected(QStringLiteral("date.month"), true);
    grouping.setDimensionSelected(QStringLiteral("place.name"), true);

    const QVariantList sections = grouping.sections();
    require(sections.size() == 4, "month and location split both relevance bands by composition");
    require(
        section(sections, 0)
                .value(QStringLiteral("title"))
                .toString()
                .contains(QStringLiteral("Highly related"))
            && section(sections, 0)
                   .value(QStringLiteral("title"))
                   .toString()
                   .contains(QStringLiteral("Beijing")),
        "the composed title keeps the semantic outer group and location value"
    );
    require(
        section(sections, 3)
            .value(QStringLiteral("representationKeys"))
            .toStringList()
            .contains(representationKey("d")),
        "missing metadata remains visible in an explicit unknown composite group"
    );
    require(
        section(sections, 0)
                .value(QStringLiteral("navigationLabel"))
                .toString()
                .contains(QStringLiteral("2024"))
            && !section(sections, 0)
                    .value(QStringLiteral("navigationLabel"))
                    .toString()
                    .contains(QStringLiteral("Beijing"))
            && section(sections, 0).value(QStringLiteral("navigationMajorLabel")).toString()
                   == QStringLiteral("2024")
            && !section(sections, 0)
                    .value(QStringLiteral("navigationShortLabel"))
                    .toString()
                    .isEmpty(),
        "a composite group exposes the leading date dimension as its navigator axis"
    );
}

void date_granularities_are_mutually_exclusive_and_source_resets_rebuild() {
    PhotoModel photos;
    photos.replace({
        {QStringLiteral("a"),
         QStringLiteral("representation-a"),
         QStringLiteral("2024-03-18"),
         0,
         QStringLiteral("Beijing")},
        {QStringLiteral("b"),
         QStringLiteral("representation-b"),
         QStringLiteral("2024-04-18"),
         0,
         QStringLiteral("Beijing")},
    });

    ReviewGalleryGroupingController grouping;
    grouping.setSourceModel(&photos);
    grouping.setDimensionSelected(QStringLiteral("date.month"), true);
    require(grouping.sections().size() == 2, "two months initially form two groups");
    grouping.setDimensionSelected(QStringLiteral("date.year"), true);
    require(
        grouping.activeDimensionCount() == 1 && grouping.sections().size() == 1,
        "selecting year replaces month instead of producing a nonsensical nested date group"
    );

    photos.replace({
        {QStringLiteral("a"),
         QStringLiteral("representation-a"),
         QStringLiteral("2024-03-18"),
         0,
         QStringLiteral("Beijing")},
        {QStringLiteral("b"),
         QStringLiteral("representation-b"),
         QStringLiteral("2025-04-18"),
         0,
         QStringLiteral("Beijing")},
    });
    require(grouping.sections().size() == 2, "a source reset recompiles the selected grouping");
    photos.reads = 0;
    photos.notify(PhotoModel::RatingRole);
    require(photos.reads == 0, "rating does not scan the catalog to regroup unchanged dates");
    photos.notify(PhotoModel::CaptureDayRole);
    require(photos.reads > 0, "capture metadata still invalidates date grouping");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    base_sections_pass_through_without_user_grouping();
    large_candidate_section_keeps_unique_members_in_order();
    independent_dimensions_form_composite_groups();
    date_granularities_are_mutually_exclusive_and_source_resets_rebuild();
    return EXIT_SUCCESS;
}
