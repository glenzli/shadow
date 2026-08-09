#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QObject>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeCulling final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool arenaActive MEMBER arena_active NOTIFY stateChanged)
    Q_PROPERTY(bool arenaComplete MEMBER arena_complete NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap currentLeft READ currentLeft CONSTANT)
    Q_PROPERTY(QVariantMap currentRight READ currentRight CONSTANT)
    Q_PROPERTY(int rankedCandidateCount MEMBER ranked_count NOTIFY stateChanged)
    Q_PROPERTY(int totalArenaCandidateCount MEMBER total_count CONSTANT)
    Q_PROPERTY(int comparisonCount MEMBER comparison_count NOTIFY stateChanged)
    Q_PROPERTY(int refinementRound MEMBER refinement_round NOTIFY stateChanged)
    Q_PROPERTY(bool canRefineRunnerUps MEMBER can_refine_runner_ups NOTIFY stateChanged)
    Q_PROPERTY(QVariantList history MEMBER history NOTIFY stateChanged)
    Q_PROPERTY(QVariantList tiers MEMBER tiers NOTIFY stateChanged)
    Q_PROPERTY(QVariantList unresolvedCandidates MEMBER unresolved NOTIFY stateChanged)

  public:
    using QObject::QObject;

    [[nodiscard]] QVariantMap currentLeft() const {
        return photo(QStringLiteral("left-photo"));
    }

    [[nodiscard]] QVariantMap currentRight() const {
        return photo(QStringLiteral("right-photo"));
    }

    Q_INVOKABLE void chooseLeft() {
        ++left_choice_count;
        ++comparison_count;
        history = {QVariantMap{{QStringLiteral("choice"), QStringLiteral("left")}}};
        emit stateChanged();
    }

    Q_INVOKABLE void chooseRight() { ++right_choice_count; }
    Q_INVOKABLE void chooseEqual() { ++equal_choice_count; }
    Q_INVOKABLE void skipCurrent() { ++skip_count; }
    Q_INVOKABLE void undoLastChoice() { ++undo_count; }
    Q_INVOKABLE void leaveArena() { arena_active = false; emit stateChanged(); }
    Q_INVOKABLE void selectTopResult() {}
    Q_INVOKABLE void refineRunnerUps() {}
    Q_INVOKABLE void clearCandidates() {}

    static QVariantMap photo(const QString& id) {
        return {
            {QStringLiteral("photoId"), id},
            {QStringLiteral("representationId"), id + QStringLiteral("-representation")},
            {QStringLiteral("title"), id},
            {QStringLiteral("visualSource"), QString{}},
        };
    }

    bool arena_active = true;
    bool arena_complete = false;
    int ranked_count = 1;
    int total_count = 4;
    int comparison_count = 0;
    int refinement_round = 0;
    bool can_refine_runner_ups = false;
    QVariantList history;
    QVariantList tiers;
    QVariantList unresolved;
    int left_choice_count = 0;
    int right_choice_count = 0;
    int equal_choice_count = 0;
    int skip_count = 0;
    int undo_count = 0;

  signals:
    void stateChanged();
};

class FakeReview final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* culling READ culling CONSTANT)

  public:
    explicit FakeReview(QObject* parent = nullptr) : QObject(parent), culling_(this) {}

    [[nodiscard]] QObject* culling() { return &culling_; }

    FakeCulling culling_;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "review culling arena failed: " << message << '\n';
    }
    return condition;
}

void drain_bindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QEventLoop frame_loop;
    QTimer::singleShot(40, &frame_loop, &QEventLoop::quit);
    frame_loop.exec();
    QCoreApplication::processEvents();
}

[[nodiscard]] QQuickItem* find_item(QQuickItem& root, const QString& object_name) {
    if (root.objectName() == object_name) {
        return &root;
    }
    for (QQuickItem* const child : root.childItems()) {
        if (auto* const match = find_item(*child, object_name); match != nullptr) {
            return match;
        }
    }
    return nullptr;
}

void click(QQuickWindow& window, QQuickItem& item) {
    const QPointF position = item.mapToScene(QPointF{item.width() / 2.0, item.height() / 2.0});
    QMouseEvent press(
        QEvent::MouseButtonPress,
        position,
        position,
        position,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice()
    );
    QGuiApplication::sendEvent(&window, &press);
    QMouseEvent release(
        QEvent::MouseButtonRelease,
        position,
        position,
        position,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice()
    );
    QGuiApplication::sendEvent(&window, &release);
    drain_bindings();
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.ReviewCullingArenaContract"),
        QStringLiteral("ReviewCullingArena")
    );

    QQuickWindow window;
    window.setGeometry(0, 0, 1'100, 700);
    window.show();
    FakeReview review;
    std::unique_ptr<QObject> object{component.createWithInitialProperties({
        {QStringLiteral("review"), QVariant::fromValue(&review)},
        {QStringLiteral("width"), 1'100.0},
        {QStringLiteral("height"), 700.0},
    })};
    auto* const root = qobject_cast<QQuickItem*>(object.get());
    if (root == nullptr) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    root->setParentItem(window.contentItem());
    drain_bindings();

    auto* const left_pane = find_item(*root, QStringLiteral("cullingArenaLeftPane"));
    auto* const right_pane = find_item(*root, QStringLiteral("cullingArenaRightPane"));
    auto* const left_button = find_item(
        *root, QStringLiteral("cullingArenaChooseLeftButton")
    );
    bool valid = true;
    valid &= require(
        left_pane != nullptr && right_pane != nullptr
            && left_pane->width() > 0.0 && right_pane->width() > 0.0,
        "the arena presents one visible 1:1 pair"
    );
    valid &= require(left_button != nullptr, "the guided left-choice action is reachable");
    if (left_button != nullptr) {
        click(window, *left_button);
    }
    valid &= require(
        review.culling_.left_choice_count == 1
            && review.culling_.comparison_count == 1,
        "a real button click advances one pairwise decision"
    );

    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "review_culling_arena_test.moc"
