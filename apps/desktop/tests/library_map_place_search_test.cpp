#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeAmapPlaceSearchService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(QVariantList results READ results NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText CONSTANT)

  public:
    [[nodiscard]] bool available() const noexcept {
        return true;
    }
    [[nodiscard]] bool busy() const noexcept {
        return false;
    }
    [[nodiscard]] QVariantList results() const {
        return results_;
    }
    [[nodiscard]] QString errorText() const {
        return {};
    }

    Q_INVOKABLE void search(const QString& query, const double latitude, const double longitude) {
        ++search_count;
        last_query = query;
        last_latitude = latitude;
        last_longitude = longitude;
    }

    Q_INVOKABLE void clear() {
        ++clear_count;
        results_.clear();
        emit stateChanged();
    }

    int search_count = 0;
    int clear_count = 0;
    QString last_query;
    double last_latitude = 0.0;
    double last_longitude = 0.0;

  signals:
    void stateChanged();

  private:
    QVariantList results_{
        QVariantMap{
            {QStringLiteral("name"), QStringLiteral("外滩")},
            {QStringLiteral("label"), QStringLiteral("外滩 · 上海市黄浦区")},
            {QStringLiteral("latitude"), 31.2400},
            {QStringLiteral("longitude"), 121.4900},
        },
    };
};

class FakeMapController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(double centerLatitude READ centerLatitude NOTIFY centerChanged)
    Q_PROPERTY(double centerLongitude READ centerLongitude NOTIFY centerChanged)
    Q_PROPERTY(double zoomLevel READ zoomLevel NOTIFY centerChanged)

  public:
    [[nodiscard]] double centerLatitude() const noexcept {
        return latitude_;
    }
    [[nodiscard]] double centerLongitude() const noexcept {
        return longitude_;
    }
    [[nodiscard]] double zoomLevel() const noexcept {
        return zoom_level_;
    }

    Q_INVOKABLE void
    navigateToContext(const double latitude, const double longitude, const double zoom_level) {
        ++context_navigation_count;
        latitude_ = latitude;
        longitude_ = longitude;
        zoom_level_ = zoom_level;
        emit zoomLevelChanged();
        emit centerChanged();
    }

    int context_navigation_count = 0;

  signals:
    void centerChanged();
    void zoomLevelChanged();

  private:
    double latitude_ = 31.2304;
    double longitude_ = 121.4737;
    double zoom_level_ = 9.0;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Library-map place-search contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

[[nodiscard]] QQuickItem* findVisualChild(QQuickItem& parent, const QString& object_name) {
    for (QQuickItem* const child : parent.childItems()) {
        if (child->objectName() == object_name) {
            return child;
        }
        if (QQuickItem* const match = findVisualChild(*child, object_name); match != nullptr) {
            return match;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.LibraryMapPlaceSearchContract"),
        QStringLiteral("LibraryMapPlaceSearch")
    );

    FakeAmapPlaceSearchService service;
    FakeMapController map_controller;
    std::unique_ptr<QObject> search_object{component.createWithInitialProperties({
        {QStringLiteral("service"), QVariant::fromValue(&service)},
        {QStringLiteral("mapController"), QVariant::fromValue(&map_controller)},
    })};
    auto* const search = qobject_cast<QQuickItem*>(search_object.get());
    if (search == nullptr) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QQuickWindow window;
    window.setGeometry(0, 0, 520, 300);
    search->setParentItem(window.contentItem());
    search->setPosition(QPointF{20.0, 20.0});
    window.show();
    drainBindings();

    auto* const search_field =
        findVisualChild(*search, QStringLiteral("libraryMapAmapSearchField"));
    auto* const result = findVisualChild(*search, QStringLiteral("libraryMapAmapSearchResult"));
    bool valid = require(
        search->isVisible() && search_field != nullptr && result != nullptr,
        "the packaged search field and result delegate are visible when AMap search is authorized"
    );

    if (search_field != nullptr) {
        search_field->setProperty("text", QStringLiteral("外滩"));
        valid &= require(
            QMetaObject::invokeMethod(search, "submitSearch") && service.search_count == 1
                && service.last_query == QStringLiteral("外滩")
                && qAbs(service.last_latitude - 31.2304) < 0.0001
                && qAbs(service.last_longitude - 121.4737) < 0.0001,
            "submitting a query forwards the visible WGS84 map center to the service"
        );
    }

    if (result != nullptr) {
        const QPointF position =
            result->mapToScene(QPointF{result->width() / 2.0, result->height() / 2.0});
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, position.toPoint());
        drainBindings();
        valid &= require(
            qAbs(map_controller.centerLatitude() - 31.2400) < 0.0001
                && qAbs(map_controller.centerLongitude() - 121.4900) < 0.0001
                && map_controller.zoomLevel() >= 13.0
                && map_controller.context_navigation_count == 1 && service.clear_count == 1,
            "clicking a result opens an intentional provider context in WGS84 and clears transient "
            "results"
        );
    }

    search->setProperty("providerEligible", false);
    drainBindings();
    valid &= require(
        !search->isVisible(),
        "AMap place search stays hidden while another basemap owns the map context"
    );

    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "library_map_place_search_test.moc"
