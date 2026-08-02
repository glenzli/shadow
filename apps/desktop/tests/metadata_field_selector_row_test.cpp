#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QString>
#include <QVariant>

#include <cstdlib>
#include <iostream>
#include <memory>

class ToggleReceiver final : public QObject {
    Q_OBJECT

  public slots:
    void receive(const bool checked) {
        called = true;
        requested_checked = checked;
    }

  public:
    bool called = false;
    bool requested_checked = true;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Metadata field selector contract failed: " << message << '\n';
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
        QStringLiteral("Shadow.MetadataFieldSelectorContract"),
        QStringLiteral("MetadataFieldSelectorRow")
    );
    std::unique_ptr<QObject> row{component.createWithInitialProperties({
        {QStringLiteral("fieldLabel"), QStringLiteral("Camera")},
        {QStringLiteral("fieldValue"), QStringLiteral("Nikon Z 9")},
        {QStringLiteral("checked"), true},
    })};
    if (!row) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    drainBindings();

    QObject* const check_box =
        row->findChild<QObject*>(QStringLiteral("metadataFieldSidebarCheckBox"));
    if (!require(check_box != nullptr, "the packaged row exposes its field checkbox")
        || !require(
            check_box->property("shadowStyled").toBool(),
            "the metadata selector reuses ShadowCheckBox"
        )
        || !require(
            check_box->property("checked").toBool()
                && check_box->property("compact").toBool(),
            "the shared checkbox reflects the selected compact presentation"
        )
        || !require(
            check_box->property("accessibleName").toString().contains(
                QStringLiteral("Camera")
            ),
            "the field identity remains available to assistive technology"
        )) {
        return EXIT_FAILURE;
    }

    ToggleReceiver receiver;
    QObject::connect(
        row.get(), SIGNAL(toggleRequested(bool)),
        &receiver, SLOT(receive(bool))
    );
    check_box->setProperty("checked", false);
    if (!require(
            QMetaObject::invokeMethod(check_box, "clicked"),
            "the shared checkbox exposes its click interaction"
        )
        || !require(
            receiver.called && !receiver.requested_checked,
            "checkbox activation requests the updated sidebar visibility"
        )) {
        return EXIT_FAILURE;
    }

    row->setProperty("checked", false);
    drainBindings();
    return require(
               !check_box->property("checked").toBool(),
               "external preference updates continue to project into the checkbox"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "metadata_field_selector_row_test.moc"
