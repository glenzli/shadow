#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeCullingSelection final : public QObject {
    Q_OBJECT

  public:
    using QObject::QObject;

    Q_INVOKABLE QVariantMap selectedSnapshot() const { return current; }

    Q_INVOKABLE void selectPhoto(const QVariantMap& target, const int modifiers) {
        Q_UNUSED(modifiers)
        selected_photo_id = target.value(QStringLiteral("photoId")).toString();
    }

    QVariantMap current;
    QString selected_photo_id;
};

namespace {

[[nodiscard]] QVariantMap snapshot(const QString& photo_id) {
    return {
        {QStringLiteral("photoId"), photo_id},
        {QStringLiteral("representationId"), photo_id + QStringLiteral("-representation")},
        {QStringLiteral("visualHandle"), photo_id + QStringLiteral("-handle")},
        {QStringLiteral("title"), photo_id},
        {QStringLiteral("visualSource"), QStringLiteral("image://grid/") + photo_id},
        {QStringLiteral("visualWidth"), 1'600},
        {QStringLiteral("visualHeight"), 1'200},
    };
}

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "review culling state contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool invoke(QObject* target, const char* method) {
    return QMetaObject::invokeMethod(target, method);
}

[[nodiscard]] bool invoke(QObject* target, const char* method, const QVariant& value) {
    return QMetaObject::invokeMethod(target, method, Q_ARG(QVariant, value));
}

[[nodiscard]] QString current_id(QObject* culling, const char* property) {
    return culling->property(property)
        .toMap()
        .value(QStringLiteral("photoId"))
        .toString();
}

[[nodiscard]] QString tier_id(
    QObject* culling,
    const qsizetype tier_index,
    const qsizetype photo_index
) {
    const auto tiers = culling->property("tiers").toList();
    return tiers.at(tier_index)
        .toList()
        .at(photo_index)
        .toMap()
        .value(QStringLiteral("photoId"))
        .toString();
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(
            QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/ReviewCullingState.qml")
        )
    );
    FakeCullingSelection selection;
    std::unique_ptr<QObject> culling(component.createWithInitialProperties({
        {QStringLiteral("selection"), QVariant::fromValue(&selection)},
    }));
    if (!culling) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    if (!require(invoke(culling.get(), "addCandidate", snapshot(QStringLiteral("photo-a"))),
                 "first candidate can be added")
        || !require(invoke(culling.get(), "addCandidate", snapshot(QStringLiteral("photo-b"))),
                    "second candidate can be added")
        || !require(invoke(culling.get(), "addCandidate", snapshot(QStringLiteral("photo-c"))),
                    "third candidate can be added")
        || !require(invoke(culling.get(), "addCandidate", snapshot(QStringLiteral("photo-d"))),
                    "fourth candidate can be added")
        || !require(
            invoke(culling.get(), "addCandidate", snapshot(QStringLiteral("photo-a")))
                && culling->property("candidateCount").toInt() == 4,
            "duplicate identities do not create duplicate candidates"
        )
        || !require(
            invoke(culling.get(), "startArena")
                && culling->property("arenaActive").toBool()
                && current_id(culling.get(), "currentLeft") == QStringLiteral("photo-a")
                && current_id(culling.get(), "currentRight") == QStringLiteral("photo-b"),
            "the duel starts with one guided pair"
        )) {
        return EXIT_FAILURE;
    }

    (void)invoke(culling.get(), "chooseRight"); // B > A
    if (!require(
            current_id(culling.get(), "currentLeft") == QStringLiteral("photo-b")
                && current_id(culling.get(), "currentRight") == QStringLiteral("photo-c"),
            "each new candidate first challenges the current leader"
        )) {
        return EXIT_FAILURE;
    }
    (void)invoke(culling.get(), "chooseEqual"); // C = B
    (void)invoke(culling.get(), "chooseLeft");  // B/C > D
    if (!require(
            culling->property("arenaComplete").toBool()
                && culling->property("comparisonCount").toInt() == 3,
            "one comparison per entrant completes the top-choice pass"
        )
        || !require(
            tier_id(culling.get(), 0, 0) == QStringLiteral("photo-b")
                && tier_id(culling.get(), 0, 1) == QStringLiteral("photo-c")
                && tier_id(culling.get(), 1, 0) == QStringLiteral("photo-a")
                && tier_id(culling.get(), 1, 1) == QStringLiteral("photo-d"),
            "results retain only the top tier and photos that lost directly to it"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            invoke(culling.get(), "undoLastChoice")
                && !culling->property("arenaComplete").toBool()
                && current_id(culling.get(), "currentLeft") == QStringLiteral("photo-b")
                && current_id(culling.get(), "currentRight") == QStringLiteral("photo-d"),
            "undo restores the exact pending duel"
        )) {
        return EXIT_FAILURE;
    }
    (void)invoke(culling.get(), "chooseRight"); // D > B/C after reconsidering.
    if (!require(
            culling->property("arenaComplete").toBool()
                && tier_id(culling.get(), 0, 0) == QStringLiteral("photo-d")
                && tier_id(culling.get(), 1, 0) == QStringLiteral("photo-b")
                && tier_id(culling.get(), 1, 1) == QStringLiteral("photo-c"),
            "a new winner discards indirect losses and keeps only its direct runner-up pool"
        )
        || !require(
            invoke(culling.get(), "refineRunnerUps")
                && culling->property("refinementRound").toInt() == 1
                && current_id(culling.get(), "currentLeft") == QStringLiteral("photo-b")
                && current_id(culling.get(), "currentRight") == QStringLiteral("photo-c"),
            "the runner-up pool can start one explicit follow-up duel"
        )
        || !require(
            invoke(culling.get(), "chooseLeft")
                && culling->property("arenaComplete").toBool()
                && tier_id(culling.get(), 0, 0) == QStringLiteral("photo-b")
                && tier_id(culling.get(), 1, 0) == QStringLiteral("photo-c"),
            "runner-up refinement remains another bounded top-choice pass"
        )
        || !require(
            invoke(culling.get(), "selectTopResult")
                && selection.selected_photo_id == QStringLiteral("photo-b")
                && !culling->property("arenaActive").toBool(),
            "the top result returns to ordinary Library selection without applying metadata"
        )
        || !require(
            invoke(culling.get(), "clearCandidates")
                && culling->property("candidateCount").toInt() == 0,
            "the temporary draft clears only on an explicit action"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "review_culling_state_contract_test.moc"
