#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QJSValue>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

class FakeLutEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(bool stateBusy READ stateBusy CONSTANT)
    Q_PROPERTY(bool hasLut READ hasLut NOTIFY lutChanged)
    Q_PROPERTY(QString lutResourceId READ lutResourceId NOTIFY lutChanged)
    Q_PROPERTY(QString lutTitle READ lutTitle NOTIFY lutChanged)
    Q_PROPERTY(
        double lutIntensity
        READ lutIntensity
        WRITE setLutIntensity
        NOTIFY lutChanged
    )

public:
    using QObject::QObject;

    [[nodiscard]] bool active() const noexcept {
        return true;
    }
    [[nodiscard]] bool stateBusy() const noexcept {
        return false;
    }
    [[nodiscard]] bool hasLut() const noexcept {
        return !resource_id_.isEmpty();
    }
    [[nodiscard]] QString lutResourceId() const {
        return resource_id_;
    }
    [[nodiscard]] QString lutTitle() const {
        return title_;
    }
    [[nodiscard]] double lutIntensity() const noexcept {
        return intensity_;
    }

    void setLutIntensity(const double value) {
        intensity_ = value;
        emit lutChanged();
    }

    Q_INVOKABLE void setLutResource(
        const QString& resource_id,
        const QString& title,
        const QString& managed_path
    ) {
        resource_id_ = resource_id;
        title_ = title;
        managed_path_ = managed_path;
        emit lutChanged();
    }

    Q_INVOKABLE void clearLut() {
        ++clear_count_;
        resource_id_.clear();
        title_.clear();
        managed_path_.clear();
        emit lutChanged();
    }

    Q_INVOKABLE void beginParameterEdit(const QString& key) {
        begin_key_ = key;
    }

    Q_INVOKABLE void endParameterEdit(const QString& key) {
        end_key_ = key;
    }

    QString resource_id_;
    QString title_;
    QString managed_path_;
    QString begin_key_;
    QString end_key_;
    int clear_count_ = 0;

signals:
    void lutChanged();

private:
    double intensity_ = 1.0;
};

class FakeLutLibrary final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        QVariantList availableEntries
        READ availableEntries
        NOTIFY availableEntriesChanged
    )

public:
    using QObject::QObject;

    [[nodiscard]] QVariantList availableEntries() const {
        return entries_;
    }

    void setEntries(QVariantList entries) {
        entries_ = std::move(entries);
        emit availableEntriesChanged();
    }

signals:
    void availableEntriesChanged();

private:
    QVariantList entries_;
};

class LutPreviewProvider final : public QQuickImageProvider {
public:
    LutPreviewProvider()
        : QQuickImageProvider(QQuickImageProvider::Image) {}

    [[nodiscard]] QImage requestImage(
        const QString&,
        QSize* size,
        const QSize&
    ) override {
        if (size != nullptr) {
            *size = QSize(1, 1);
        }
        return QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
    }
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision LUT section contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] QVariantMap entry(
    const QString& id,
    const QString& title,
    const QString& root,
    const QString& relative_path,
    const int size
) {
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), title},
        {QStringLiteral("directory"), root},
        {QStringLiteral("path"), root + QStringLiteral("/") + relative_path},
        {QStringLiteral("fileName"), relative_path},
        {
            QStringLiteral("managedPath"),
            QStringLiteral("/catalog/") + id + QStringLiteral(".cube"),
        },
        {QStringLiteral("size"), size},
    };
}

[[nodiscard]] bool invoke(
    QObject* target,
    const char* method,
    const QVariant& argument
) {
    return QMetaObject::invokeMethod(
        target,
        method,
        Q_ARG(QVariant, argument)
    );
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("shadow-lut"), new LutPreviewProvider);

    const QString source_path = QStringLiteral(
        SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionLutSection.qml"
    );
    QQmlComponent component(&engine, QUrl::fromLocalFile(source_path));
    FakeLutEditor editor;
    FakeLutLibrary library;
    std::unique_ptr<QObject> section(component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("lutLibrary"), QVariant::fromValue(&library)},
        {QStringLiteral("textPrimary"), QColor(QStringLiteral("#eeeeee"))},
        {QStringLiteral("textSecondary"), QColor(QStringLiteral("#bbbbbb"))},
        {QStringLiteral("textMuted"), QColor(QStringLiteral("#888888"))},
        {QStringLiteral("accent"), QColor(QStringLiteral("#3d9cff"))},
    }));
    if (!section) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QJSValue groups = section->property("browserGroups").value<QJSValue>();
    if (!require(groups.isArray() && groups.property("length").toInt() == 0,
                 "an empty library produces no synthetic empty-state row")) {
        return EXIT_FAILURE;
    }

    library.setEntries({
        entry(
            QStringLiteral("warm"),
            QStringLiteral("Warm Film"),
            QStringLiteral("/managed/A"),
            QStringLiteral("Film/warm.cube"),
            33
        ),
        entry(
            QStringLiteral("cool"),
            QStringLiteral("Cool Film"),
            QStringLiteral("/managed/A"),
            QStringLiteral("Film/cool.cube"),
            33
        ),
        entry(
            QStringLiteral("clean"),
            QStringLiteral("Clean"),
            QStringLiteral("/managed/B"),
            QStringLiteral("clean.cube"),
            17
        ),
    });
    QCoreApplication::processEvents();
    groups = section->property("browserGroups").value<QJSValue>();
    if (!require(groups.property("length").toInt() == 2,
                 "nested entries are grouped by managed directory")
        || !require(
            groups.property(0).property("title").toString()
                == QStringLiteral("A / Film")
                && groups.property(0)
                    .property("entries")
                    .property("length")
                    .toInt()
                    == 2,
            "the nested A/Film group is stable and complete"
        )
        || !require(
            groups.property(1).property("title").toString()
                == QStringLiteral("B"),
            "root-level LUTs retain their source-root identity"
        )) {
        return EXIT_FAILURE;
    }

    section->setProperty("browserExpanded", true);
    const QVariantMap selected = entry(
        QStringLiteral("warm"),
        QStringLiteral("Warm Film"),
        QStringLiteral("/managed/A"),
        QStringLiteral("Film/warm.cube"),
        33
    );
    if (!require(invoke(section.get(), "selectLut", selected),
                 "selection helper is invokable")
        || !require(
            editor.resource_id_ == QStringLiteral("warm")
                && editor.title_ == QStringLiteral("Warm Film")
                && editor.managed_path_
                    == QStringLiteral("/catalog/warm.cube")
                && !section->property("browserExpanded").toBool(),
            "selection preserves the exact resource contract and closes the browser"
        )
        || !require(
            QMetaObject::invokeMethod(section.get(), "clearSelection")
                && editor.clear_count_ == 1,
            "clear delegates to the editor"
        )
        || !require(
            QMetaObject::invokeMethod(section.get(), "beginIntensityEdit")
                && invoke(section.get(), "setIntensity", 0.42)
                && QMetaObject::invokeMethod(section.get(), "endIntensityEdit"),
            "intensity gesture helpers are invokable"
        )
        || !require(
            editor.begin_key_ == QStringLiteral("lut_intensity")
                && editor.end_key_ == QStringLiteral("lut_intensity")
                && editor.lutIntensity() == 0.42,
            "intensity keeps one complete parameter gesture"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "precision_lut_section_contract_test.moc"
