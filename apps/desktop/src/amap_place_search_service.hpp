#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <cstdint>

class MapProviderPreferences;
class QNetworkAccessManager;
class QNetworkReply;

/// Owns AMap place-search admission, request identity, cancellation, bounded
/// parsing, and provider-coordinate adaptation for the Library map.
class AmapPlaceSearchService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY stateChanged)
    Q_PROPERTY(QString activeQuery READ activeQuery NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)

  public:
    explicit AmapPlaceSearchService(
        MapProviderPreferences* preferences,
        QObject* parent = nullptr
    );
    AmapPlaceSearchService(
        MapProviderPreferences* preferences,
        QNetworkAccessManager* network,
        QUrl endpoint,
        QObject* parent = nullptr
    );
    ~AmapPlaceSearchService() override;

    AmapPlaceSearchService(const AmapPlaceSearchService&) = delete;
    AmapPlaceSearchService& operator=(const AmapPlaceSearchService&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QVariantList results() const;
    [[nodiscard]] QString activeQuery() const;
    [[nodiscard]] QString errorText() const;

    Q_INVOKABLE void search(const QString& query, double latitude, double longitude);
    Q_INVOKABLE void clear();

  signals:
    void stateChanged();

  private:
    void cancelRequest() noexcept;
    void finish(std::uint64_t generation);
    void resetUnavailable();

    MapProviderPreferences* preferences_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    QUrl endpoint_;
    QNetworkReply* reply_ = nullptr;
    QVariantList results_;
    QString active_query_;
    QString error_text_;
    std::uint64_t generation_ = 0;
    bool busy_ = false;
};
