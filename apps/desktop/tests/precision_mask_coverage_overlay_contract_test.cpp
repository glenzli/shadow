#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QSize>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantMap>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

class FakeMaskCoverageEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(bool hasSelectedGradeNode READ hasSelectedGradeNode NOTIFY selectedGradeNodeChanged)
    Q_PROPERTY(QVariantMap selectedLocalMask READ selectedLocalMask NOTIFY parametersChanged)
    Q_PROPERTY(QString maskCoverageSource READ maskCoverageSource NOTIFY maskCoverageSourceChanged)

  public:
    using QObject::QObject;

    [[nodiscard]] bool active() const noexcept {
        return true;
    }
    [[nodiscard]] bool hasSelectedGradeNode() const noexcept {
        return has_selected_node_;
    }
    [[nodiscard]] QVariantMap selectedLocalMask() const {
        return {{QStringLiteral("kind"), mask_kind_}};
    }
    [[nodiscard]] QString maskCoverageSource() const {
        return source_;
    }

    void setMaskKind(const int kind) {
        mask_kind_ = kind;
        emit parametersChanged();
    }
    void setSource(QString source) {
        source_ = std::move(source);
        emit maskCoverageSourceChanged();
    }

  signals:
    void selectedGradeNodeChanged();
    void parametersChanged();
    void maskCoverageSourceChanged();

  private:
    bool has_selected_node_ = true;
    int mask_kind_ = 1;
    QString source_ = QStringLiteral("image://mask-contract/coverage?preview=17");
};

class AlphaCoverageProvider final : public QQuickImageProvider {
  public:
    AlphaCoverageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    [[nodiscard]] QImage requestImage(const QString&, QSize* size, const QSize&) override {
        QImage image(2, 2, QImage::Format_Alpha8);
        image.fill(192);
        if (size != nullptr) {
            *size = image.size();
        }
        return image;
    }
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision mask coverage overlay contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool waitFor(const std::function<bool()>& predicate, const int timeout_ms = 2'000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents();
        if (predicate()) {
            return true;
        }
        QThread::msleep(1);
    }
    return predicate();
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("mask-contract"), new AlphaCoverageProvider());
    QQmlComponent component(&engine);
    component.loadFromModule(
        QStringLiteral("Shadow.MaskCoverageContract"),
        QStringLiteral("PrecisionMaskCoverageOverlay")
    );
    FakeMaskCoverageEditor editor;
    std::unique_ptr<QObject> overlay(component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("interactionEnabled"), true},
        {QStringLiteral("coverageVisible"), true},
        {QStringLiteral("readyPreviewGeneration"), QStringLiteral("17")},
    }));
    if (!overlay) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    for (int kind = 1; kind <= 5; ++kind) {
        editor.setMaskKind(kind);
        if (!require(
                waitFor([&overlay]() {
                    return overlay->property("visible").toBool()
                           && overlay->property("coverageReady").toBool();
                }),
                "all five mask kinds consume the same real coverage texture"
            )) {
            return EXIT_FAILURE;
        }
    }

    overlay->setProperty("coverageVisible", false);
    if (!require(
            waitFor([&overlay]() {
                return !overlay->property("visible").toBool()
                       && overlay->property("coverageReady").toBool();
            }),
            "the O overlay toggle hides presentation without discarding exact coverage"
        )) {
        return EXIT_FAILURE;
    }
    overlay->setProperty("coverageVisible", true);

    overlay->setProperty("readyPreviewGeneration", QStringLiteral("18"));
    if (!require(
            waitFor([&overlay]() {
                return !overlay->property("visible").toBool()
                       && !overlay->property("coverageReady").toBool();
            }),
            "coverage stays hidden when its paired preview generation is stale"
        )) {
        return EXIT_FAILURE;
    }

    overlay->setProperty("readyPreviewGeneration", QStringLiteral("17"));
    editor.setSource({});
    if (!require(
            waitFor([&overlay]() { return !overlay->property("visible").toBool(); }),
            "an empty provider identity is the only empty-state presentation"
        )) {
        return EXIT_FAILURE;
    }

    editor.setSource(QStringLiteral("image://mask-contract/coverage?preview=17"));
    editor.setMaskKind(0);
    if (!require(
            waitFor([&overlay]() { return !overlay->property("visible").toBool(); }),
            "a node without a mask never shows residual coverage"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "precision_mask_coverage_overlay_contract_test.moc"
