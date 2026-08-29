#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakePeopleMaskEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool aiMaskBusy MEMBER busy NOTIFY changed)
    Q_PROPERTY(QVariantList aiMaskPeople READ people NOTIFY changed)
    Q_PROPERTY(int aiMaskSelectedPerson READ selectedPerson WRITE setSelectedPerson NOTIFY changed)
    Q_PROPERTY(int aiMaskFaceRegionMask READ faceRegionMask NOTIFY changed)

  public:
    QVariantList people() const {
        return {
            QVariantMap{
                {QStringLiteral("index"), 0},
                {QStringLiteral("thumbnailSource"), QString{}},
                {QStringLiteral("regionsAnalyzed"), false},
                {QStringLiteral("availableRegionMask"), 0},
            },
            QVariantMap{
                {QStringLiteral("index"), 1},
                {QStringLiteral("thumbnailSource"), QString{}},
                {QStringLiteral("regionsAnalyzed"), true},
                {QStringLiteral("availableRegionMask"), (1 << 0) | (1 << 2)},
            },
        };
    }

    int selectedPerson() const noexcept {
        return selected_person;
    }

    int faceRegionMask() const noexcept {
        return region_mask;
    }

    void setSelectedPerson(const int person) {
        selected_person = person;
        ++person_changes;
        emit changed();
    }

    Q_INVOKABLE void toggleAiMaskFaceRegion(const int region, const bool selected) {
        const int bit = 1 << region;
        region_mask = selected ? region_mask | bit : region_mask & ~bit;
        ++region_changes;
        emit changed();
    }

    bool busy = false;
    int selected_person = 0;
    int region_mask = 1;
    int person_changes = 0;
    int region_changes = 0;

  signals:
    void changed();
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "People-mask selector contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.PeopleMaskSelectorContract"),
        QStringLiteral("PrecisionPeopleMaskSelector")
    );
    FakePeopleMaskEditor editor;
    std::unique_ptr<QObject> object{component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("width"), 320.0},
    })};
    auto* const selector = qobject_cast<QQuickItem*>(object.get());
    if (!selector) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QQuickWindow window;
    window.setGeometry(0, 0, 320, 280);
    selector->setParentItem(window.contentItem());
    window.show();
    drainBindings();

    if (!require(
            selector->property("renderedPersonCount").toInt() == 2,
            "each discovered person has one visible card"
        )) {
        return EXIT_FAILURE;
    }
    QMetaObject::invokeMethod(selector, "selectPerson", Q_ARG(QVariant, 1));
    drainBindings();
    if (!require(
            editor.selected_person == 1 && editor.person_changes == 1,
            "selecting a card changes the transient person index"
        )) {
        return EXIT_FAILURE;
    }

    QVariant eyes_available;
    QVariant hair_available;
    QMetaObject::invokeMethod(
        selector,
        "regionAvailable",
        Q_RETURN_ARG(QVariant, eyes_available),
        Q_ARG(QVariant, 2)
    );
    QMetaObject::invokeMethod(
        selector,
        "regionAvailable",
        Q_RETURN_ARG(QVariant, hair_available),
        Q_ARG(QVariant, 7)
    );
    if (!require(
            eyes_available.toBool() && !hair_available.toBool(),
            "analyzed ontology availability disables absent details without hiding the list"
        )) {
        return EXIT_FAILURE;
    }
    QMetaObject::invokeMethod(selector, "toggleRegion", Q_ARG(QVariant, 2), Q_ARG(QVariant, true));
    drainBindings();
    return require(
               editor.region_changes == 1 && (editor.region_mask & (1 << 2)) != 0,
               "detail buttons combine regions instead of replacing the node mask system"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "precision_people_mask_selector_contract_test.moc"
