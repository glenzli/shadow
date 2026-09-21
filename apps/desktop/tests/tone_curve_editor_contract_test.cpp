#include "tone_curve_point_model.hpp"
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

class CurveController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int toneCurveChannel READ channel WRITE setChannel NOTIFY toneCurveChanged)
    Q_PROPERTY(QVariantList toneCurveModifiedChannels READ modified NOTIFY toneCurveChanged)
    Q_PROPERTY(bool hasToneCurve READ hasCurve NOTIFY toneCurveChanged)
    Q_PROPERTY(bool toneCurveEditable READ editable CONSTANT)
    Q_PROPERTY(QAbstractItemModel* toneCurvePoints READ points CONSTANT)
  public:
    int channel() const {
        return channel_;
    }
    bool editable() const {
        return true;
    }
    bool hasCurve() const {
        return model.pointCount() > 2;
    }
    QAbstractItemModel* points() {
        return &model;
    }
    QVariantList modified() const {
        QVariantList result;
        for (const auto& curve : saved)
            result.push_back(curve.size() > 2);
        return result;
    }
    void setChannel(int value) {
        if (value == channel_)
            return;
        saved[static_cast<std::size_t>(channel_)] = model.points();
        channel_ = value;
        auto values = saved[static_cast<std::size_t>(channel_)];
        if (values.isEmpty())
            values = {{0, 0}, {1, 1}};
        static_cast<void>(model.replace(values));
        emit toneCurveChanged();
    }
    Q_INVOKABLE void beginToneCurveGesture(int) {
        ++begins;
    }
    Q_INVOKABLE void endToneCurveGesture(int) {
        ++ends;
    }
    Q_INVOKABLE void moveToneCurvePoint(int i, double x, double y) {
        static_cast<void>(model.movePoint(i, x, y));
        saved[static_cast<std::size_t>(channel_)] = model.points();
        emit toneCurveChanged();
    }
    Q_INVOKABLE void addToneCurvePoint(double x, double y) {
        static_cast<void>(model.addPoint(x, y));
        saved[static_cast<std::size_t>(channel_)] = model.points();
        emit toneCurveChanged();
    }
    Q_INVOKABLE void removeToneCurvePoint(int i) {
        static_cast<void>(model.removePoint(i));
        emit toneCurveChanged();
    }
    Q_INVOKABLE void resetToneCurve() {
        model.resetLinear();
        saved[static_cast<std::size_t>(channel_)].clear();
        emit toneCurveChanged();
    }
    ToneCurvePointModel model;
    int begins = 0, ends = 0;
    int channel_ = 0;
    std::array<QVector<ToneCurvePoint>, 5> saved;
  signals:
    void toneCurveChanged();
    void selectedGradeNodeChanged();
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
QQuickItem* findItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems())
        if (auto* found = findItem(child, name))
            return found;
    return nullptr;
}
int main(int argc, char** argv) {
    QQuickStyle::setStyle("Basic");
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    CurveController controller;
    QQmlComponent component(
        &engine,
        QUrl("qrc:/qt/qml/Shadow/CurveContract/qml/ToneCurveEditor.qml")
    );
    std::unique_ptr<QObject> object(
        component.createWithInitialProperties({{"controller", QVariant::fromValue(&controller)}})
    );
    if (!object)
        std::cerr << component.errorString().toStdString();
    auto* root = qobject_cast<QQuickItem*>(object.get());
    require(root, "packaged curve editor loads");
    QQuickWindow window;
    window.resize(280, 360);
    root->setParentItem(window.contentItem());
    root->setWidth(280);
    root->setHeight(350);
    window.show();
    QTest::qWait(80);
    auto clickChannel = [&](int index) {
        auto* tab = findItem(root, QString("toneCurveChannel%1").arg(index));
        require(
            tab && tab->width() >= 26 && tab->height() >= 26,
            "channel target remains usable at narrow width"
        );
        const auto bounds = tab->mapRectToItem(root, tab->boundingRect());
        require(bounds.left() >= 0 && bounds.right() <= 280.5, "channel fits the narrow panel");
        QTest::mouseClick(
            &window,
            Qt::LeftButton,
            Qt::NoModifier,
            tab->mapToScene({tab->width() / 2, tab->height() / 2}).toPoint()
        );
        QTest::qWait(30);
        require(controller.channel() == index, "pointer switches selected channel");
    };
    for (int i = 0; i < 5; ++i)
        clickChannel(i);
    clickChannel(2);
    auto* frame = findItem(root, "toneCurveFrame");
    require(frame, "curve plot exists");
    auto center = frame->mapToScene({frame->width() / 2, frame->height() / 2}).toPoint();
    QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, center);
    QTest::qWait(30);
    require(controller.model.pointCount() == 3, "double click adds a red channel point");
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, center);
    QTest::mouseMove(&window, center + QPoint(8, -20), 30);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, center + QPoint(8, -20));
    QTest::qWait(30);
    require(
        controller.begins == controller.ends && controller.begins > 0,
        "drag is one completed gesture"
    );
    const auto red = controller.model.points();
    clickChannel(4);
    require(controller.model.pointCount() == 2, "blue starts neutral");
    clickChannel(2);
    require(controller.model.points() == red, "switching channels retains the red curve");
    require(
        controller.modified()[2].toBool() && !controller.modified()[4].toBool(),
        "modified indicator reflects authored channels"
    );
    std::cout << "Curve pointer, channel retention and narrow layout passed\n";
    return 0;
}
#include "tone_curve_editor_contract_test.moc"
