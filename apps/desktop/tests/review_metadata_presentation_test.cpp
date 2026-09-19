#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTranslator>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class MetadataTranslator final : public QTranslator {
public:
    bool isEmpty() const override { return false; }
    QString translate(const char* context, const char* source,
                      const char*, int) const override {
        if (QString::fromUtf8(context) == QStringLiteral("ReviewWorkspace"))
            return QStringLiteral("translated:") + QString::fromUtf8(source);
        return {};
    }
};

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    MetadataTranslator translator;
    app.installTranslator(&translator);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    // Invoke through a different QML context, as ReviewWorkspace does in the app.
    component.setData(R"QML(
        import QtQuick
        import Shadow.ReviewMetadataContract
        QtObject {
            id: host
            property var values: ({ selectedCapturedAtUnixSeconds: 0,
                selectedHasCoordinates: false, selectedCameraMake: "Nikon",
                selectedCameraModel: "Z 9", selectedLensMake: "", selectedLensModel: "",
                selectedExposureTimeSeconds: 0, selectedApertureFNumber: 0,
                selectedIsoSpeed: 0, selectedFocalLengthMm: 0, selectedWidth: 0,
                selectedHeight: 0, selectedFocalLength35mm: 0, selectedRawWidth: 0,
                selectedSensorBits: 0, selectedCfaPattern: "", selectedDngVersion: "" })
            property ReviewMetadataPresentation presentation: ReviewMetadataPresentation {
                workspace: host.values
            }
            property var fields: presentation.metadataFields()
        }
    )QML", QUrl(QStringLiteral("qrc:/metadata-host.qml")));
    std::unique_ptr<QObject> host(component.create());
    if (!host) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    auto fields = [&] {
        const QVariant value = host->property("fields");
        return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toList()
                                          : value.toList();
    };
    auto check = [&](bool translated) {
        const auto values = fields();
        if (values.size() != 14)
            return false;
        for (const auto& value : values) {
            const auto row = value.toMap();
            for (const char* key : {"group", "label"}) {
                if (row.value(QString::fromUtf8(key)).toString().startsWith(
                        QStringLiteral("translated:")) != translated) {
                    std::cerr << row.value(QString::fromUtf8(key)).toString().toStdString()
                              << " used the wrong runtime translation context\n";
                    return false;
                }
            }
        }
        return values[5].toMap().value(QStringLiteral("value")) == QStringLiteral("Nikon Z 9");
    };
    if (!check(true))
        return EXIT_FAILURE;
    app.removeTranslator(&translator);
    engine.retranslate();
    if (!check(false))
        return EXIT_FAILURE;
    app.installTranslator(&translator);
    engine.retranslate();
    return check(true) ? EXIT_SUCCESS : EXIT_FAILURE;
}
