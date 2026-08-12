#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class MapProviderPreferences;

/// Owns the provider-neutral interactive Library map document.
///
/// Qt WebView is only a presentation host. Credentials are read here and
/// injected directly into the private HTML document; they are never exposed
/// through a QML property or invokable return value. Catalog and QML continue
/// to exchange WGS84 coordinates regardless of the selected provider.
class LibraryWebMapController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY stateChanged)
    Q_PROPERTY(QString providerPolicy READ providerPolicy NOTIFY stateChanged)
    Q_PROPERTY(QString providerId READ providerId NOTIFY stateChanged)
    Q_PROPERTY(bool providerSelected READ providerSelected NOTIFY stateChanged)
    Q_PROPERTY(bool providerAvailable READ providerAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool providerRegionAvailable READ providerRegionAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString providerName READ providerName NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString statusCode READ statusCode NOTIFY stateChanged)
    Q_PROPERTY(double centerLatitude READ centerLatitude NOTIFY centerChanged)
    Q_PROPERTY(double centerLongitude READ centerLongitude NOTIFY centerChanged)
    Q_PROPERTY(double zoomLevel READ zoomLevel NOTIFY centerChanged)

  public:
    explicit LibraryWebMapController(
        MapProviderPreferences* preferences,
        QObject* parent = nullptr
    );

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] QString providerPolicy() const;
    [[nodiscard]] QString providerId() const;
    [[nodiscard]] bool providerSelected() const;
    [[nodiscard]] bool providerAvailable() const;
    [[nodiscard]] bool providerRegionAvailable() const;
    [[nodiscard]] QString providerName() const;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString statusCode() const;
    [[nodiscard]] double centerLatitude() const noexcept;
    [[nodiscard]] double centerLongitude() const noexcept;
    [[nodiscard]] double zoomLevel() const noexcept;

    Q_INVOKABLE void attachWebView(QObject* web_view);
    Q_INVOKABLE void detachWebView(QObject* web_view);
    Q_INVOKABLE void setActive(bool active);
    /// Resolve the `auto` policy for a new intentional map context. Ordinary
    /// viewport movement never calls this, so the chosen provider is sticky.
    Q_INVOKABLE void beginMapContext(double latitude, double longitude);
    /// Re-resolve only after an explicit cross-region navigation, such as
    /// choosing a place-search result. Panning and zooming remain provider-stable.
    Q_INVOKABLE void navigateToContext(double latitude, double longitude, double zoom = -1.0);
    Q_INVOKABLE void setLanguage(const QString& language);
    Q_INVOKABLE void setClusters(const QVariantList& clusters);
    Q_INVOKABLE void setCenter(double latitude, double longitude, double zoom = -1.0);
    Q_INVOKABLE void setPlacementActive(bool active);
    Q_INVOKABLE void setPendingCoordinate(bool present, double latitude, double longitude);
    Q_INVOKABLE void consumeEvents(const QString& json);
    Q_INVOKABLE void handleLoadStatus(int status, const QString& error_text);

  signals:
    void stateChanged();
    void centerChanged();
    void viewportChanged(double south, double west, double north, double east, int zoom);
    void clusterActivated(const QVariantMap& cluster);
    void coordinateProposed(double latitude, double longitude);

  private:
    [[nodiscard]] QString buildDocument();
    [[nodiscard]] QString resolveProviderForCoordinate(double latitude, double longitude) const;
    [[nodiscard]] QString fallbackAvailableProvider() const;
    [[nodiscard]] QVariantMap presentationState() const;
    [[nodiscard]] QVariantMap providerCoordinate(double latitude, double longitude) const;
    void reloadDocument();
    void pushState();
    void runJavaScript(const QString& script);
    void setRuntimeState(bool ready, bool busy, const QString& status_code = {});
    void synchronizeProvider();

    MapProviderPreferences* preferences_ = nullptr;
    QPointer<QObject> web_view_;
    QVariantList clusters_;
    QString language_ = QStringLiteral("en-US");
    bool active_ = false;
    bool ready_ = false;
    bool busy_ = false;
    bool placement_active_ = false;
    bool pending_coordinate_present_ = false;
    QString effective_provider_id_;
    double pending_latitude_ = 0.0;
    double pending_longitude_ = 0.0;
    double center_latitude_ = 20.0;
    double center_longitude_ = 0.0;
    double zoom_level_ = 2.5;
    QString status_code_;
};
