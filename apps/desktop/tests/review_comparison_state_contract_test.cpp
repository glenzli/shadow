#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeReviewSelection final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString selectedPhotoId MEMBER photo_id NOTIFY selectionChanged)

  public:
    using QObject::QObject;

    void select(const QString& value) {
        photo_id = value;
        emit selectionChanged();
    }

    Q_INVOKABLE QVariantMap selectedSnapshot() const {
        if (photo_id.isEmpty()) {
            return {};
        }
        return snapshot(photo_id);
    }

    static QVariantMap snapshot(const QString& photo_id) {
        return {
            {QStringLiteral("photoId"), photo_id},
            {QStringLiteral("representationId"), photo_id + QStringLiteral("-representation")},
            {QStringLiteral("visualHandle"), photo_id + QStringLiteral("-handle")},
            {QStringLiteral("title"), photo_id},
            {QStringLiteral("sourcePath"), QStringLiteral("/photos/") + photo_id},
            {QStringLiteral("visualRole"), QStringLiteral("generated_proxy")},
            {QStringLiteral("visualSource"), QStringLiteral("image://grid/") + photo_id},
            {QStringLiteral("visualWidth"), 1'600},
            {QStringLiteral("visualHeight"), 1'200},
        };
    }

    QString photo_id;

  signals:
    void selectionChanged();
};

class FakeComparisonNavigation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int itemCount MEMBER item_count NOTIFY itemCountChanged)

  public:
    using QObject::QObject;

    Q_INVOKABLE QVariantMap navigationTarget(
        const QString& photo_id,
        const QString& representation_id,
        const int horizontal_delta,
        const int vertical_delta
    ) const {
        Q_UNUSED(representation_id)
        Q_UNUSED(vertical_delta)
        const QStringList photos = {
            QStringLiteral("photo-a"),
            QStringLiteral("photo-b"),
            QStringLiteral("photo-c"),
            QStringLiteral("photo-d"),
        };
        const auto current = photos.indexOf(photo_id);
        const auto requested = current + horizontal_delta;
        if (current < 0 || requested < 0 || requested >= photos.size()) {
            return {};
        }
        return FakeReviewSelection::snapshot(photos.at(requested));
    }

    int item_count = 4;

  signals:
    void itemCountChanged();
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "review comparison state contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool invoke(QObject* target, const char* method) {
    return QMetaObject::invokeMethod(target, method);
}

[[nodiscard]] bool
invoke(QObject* target, const char* method, const QVariant& first, const QVariant& second) {
    return QMetaObject::invokeMethod(
        target,
        method,
        Q_ARG(QVariant, first),
        Q_ARG(QVariant, second)
    );
}

[[nodiscard]] QString snapshot_id(QObject* comparison, const char* property) {
    return comparison->property(property).toMap().value(QStringLiteral("photoId")).toString();
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(
            QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/ReviewComparisonState.qml")
        )
    );
    FakeReviewSelection selection;
    FakeComparisonNavigation navigation;
    selection.select(QStringLiteral("photo-a"));
    std::unique_ptr<QObject> comparison(component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&navigation)},
        {QStringLiteral("selection"), QVariant::fromValue(&selection)},
        {QStringLiteral("navigationModel"), QVariant::fromValue(&navigation)},
    }));
    if (!comparison) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    if (!require(
            invoke(comparison.get(), "startQuickComparison"),
            "quick comparison action is invokable"
        )
        || !require(
            comparison->property("compareMode").toBool()
                && snapshot_id(comparison.get(), "leftComparisonSnapshot")
                       == QStringLiteral("photo-a")
                && snapshot_id(comparison.get(), "rightComparisonSnapshot")
                       == QStringLiteral("photo-b"),
            "the selected photo and next visible photo open a 1:1 workspace"
        )
        || !require(
            comparison->property("leftComparisonSource").toString()
                    == QStringLiteral("image://grid/photo-a")
                && comparison->property("rightComparisonSource").toString()
                       == QStringLiteral("image://grid/photo-b"),
            "ordinary comparison reuses repeatable Library visual sources"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            invoke(comparison.get(), "navigatePane", 1, 1),
            "right pane navigation is invokable"
        )
        || !require(
            snapshot_id(comparison.get(), "leftComparisonSnapshot") == QStringLiteral("photo-a")
                && snapshot_id(comparison.get(), "rightComparisonSnapshot")
                       == QStringLiteral("photo-c"),
            "right pane navigation leaves the left pane unchanged"
        )
        || !require(
            invoke(comparison.get(), "navigatePane", 0, 1),
            "left pane navigation is invokable"
        )
        || !require(
            snapshot_id(comparison.get(), "leftComparisonSnapshot") == QStringLiteral("photo-b")
                && snapshot_id(comparison.get(), "rightComparisonSnapshot")
                       == QStringLiteral("photo-c"),
            "left pane navigation owns an independent cursor"
        )
        || !require(
            invoke(comparison.get(), "swapPanes")
                && snapshot_id(comparison.get(), "leftComparisonSnapshot")
                       == QStringLiteral("photo-c")
                && snapshot_id(comparison.get(), "rightComparisonSnapshot")
                       == QStringLiteral("photo-b"),
            "pane swapping changes presentation only"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            invoke(comparison.get(), "exitComparison")
                && !comparison->property("compareMode").toBool()
                && snapshot_id(comparison.get(), "leftComparisonSnapshot")
                       == QStringLiteral("photo-c"),
            "leaving comparison preserves the two pane choices for reopening"
        )
        || !require(
            invoke(comparison.get(), "clearComparisonSlots")
                && comparison->property("leftComparisonSnapshot").isNull()
                && comparison->property("rightComparisonSnapshot").isNull(),
            "explicit clearing releases both pane snapshots"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            invoke(
                comparison.get(),
                "startSelectedComparison",
                FakeReviewSelection::snapshot(QStringLiteral("photo-d")),
                FakeReviewSelection::snapshot(QStringLiteral("photo-b"))
            ) && comparison->property("compareMode").toBool()
                && snapshot_id(comparison.get(), "leftComparisonSnapshot")
                       == QStringLiteral("photo-d")
                && snapshot_id(comparison.get(), "rightComparisonSnapshot")
                       == QStringLiteral("photo-b"),
            "an explicit two-photo selection opens those exact photos"
        )) {
        return EXIT_FAILURE;
    }

    const QVariantList group = {
        FakeReviewSelection::snapshot(QStringLiteral("photo-a")),
        FakeReviewSelection::snapshot(QStringLiteral("photo-c")),
        FakeReviewSelection::snapshot(QStringLiteral("photo-d")),
    };
    if (!require(
            QMetaObject::invokeMethod(
                comparison.get(),
                "startGroupComparison",
                Q_ARG(QVariant, QVariant(group))
            ),
            "similar group opens from exact snapshots"
        )
        || !require(
            comparison->property("groupMode").toBool()
                && comparison->property("groupCount").toInt() == 3,
            "group mode retains the bounded candidate set"
        )
        || !require(
            invoke(comparison.get(), "navigatePane", 1, 1)
                && snapshot_id(comparison.get(), "rightComparisonSnapshot")
                       == QStringLiteral("photo-d"),
            "group navigation follows similarity order"
        )
        || !require(
            invoke(comparison.get(), "navigatePane", 1, 1)
                && snapshot_id(comparison.get(), "rightComparisonSnapshot")
                       == QStringLiteral("photo-c"),
            "group navigation never enters an unrelated Library photo"
        )
        || !require(
            invoke(comparison.get(), "exitComparison")
                && !comparison->property("groupMode").toBool(),
            "leaving comparison releases temporary group navigation"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "review_comparison_state_contract_test.moc"
