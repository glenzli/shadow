#include "amap_place_search_service.hpp"

#include "amap_coordinate_transform.hpp"
#include "amap_web_service_protocol.hpp"
#include "map_provider_preferences.hpp"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QVariantMap>

#include <utility>

namespace {

constexpr qsizetype MINIMUM_QUERY_LENGTH = 2;
constexpr qsizetype MAXIMUM_RESPONSE_BYTES = 2 * 1024 * 1024;
constexpr int REQUEST_TIMEOUT_MS = 15'000;

[[nodiscard]] QString placeLabel(
    const shadow::desktop::maps::AmapPlaceSearchResult& place
) {
    QStringList parts{place.name};
    if (!place.locality.isEmpty() && place.locality != place.name) {
        parts.push_back(place.locality);
    }
    if (!place.address.isEmpty() && place.address != place.name) {
        parts.push_back(place.address);
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace

AmapPlaceSearchService::AmapPlaceSearchService(
    MapProviderPreferences* const preferences,
    QObject* const parent
) :
    AmapPlaceSearchService(
        preferences,
        nullptr,
        QUrl(QStringLiteral("https://restapi.amap.com/v3/place/text")),
        parent
    ) {}

AmapPlaceSearchService::AmapPlaceSearchService(
    MapProviderPreferences* const preferences,
    QNetworkAccessManager* const network,
    QUrl endpoint,
    QObject* const parent
) :
    QObject(parent), preferences_(preferences),
    network_(network != nullptr ? network : new QNetworkAccessManager(this)),
    endpoint_(std::move(endpoint)) {
    Q_ASSERT(preferences_ != nullptr);
    const auto synchronize = [this]() {
        if (!available()) {
            resetUnavailable();
        } else {
            emit stateChanged();
        }
    };
    connect(
        preferences_,
        &MapProviderPreferences::amapWebServiceKeyStoredChanged,
        this,
        synchronize
    );
    connect(
        preferences_,
        &MapProviderPreferences::amapPlacesAllowedChanged,
        this,
        synchronize
    );
}

AmapPlaceSearchService::~AmapPlaceSearchService() {
    cancelRequest();
}

bool AmapPlaceSearchService::available() const noexcept {
    return preferences_ != nullptr && preferences_->amapWebServiceKeyStored()
           && preferences_->amapPlacesAllowed();
}

bool AmapPlaceSearchService::busy() const noexcept {
    return busy_;
}

QVariantList AmapPlaceSearchService::results() const {
    return results_;
}

QString AmapPlaceSearchService::activeQuery() const {
    return active_query_;
}

QString AmapPlaceSearchService::errorText() const {
    return error_text_;
}

void AmapPlaceSearchService::search(
    const QString& query,
    const double latitude,
    const double longitude
) {
    const QString normalized = query.simplified();
    cancelRequest();
    active_query_ = normalized;
    results_.clear();
    error_text_.clear();
    if (normalized.size() < MINIMUM_QUERY_LENGTH) {
        emit stateChanged();
        return;
    }
    if (!available()) {
        error_text_ = tr("Allow AMap place search in Map & Location Services.");
        emit stateChanged();
        return;
    }
    const SecretStoreResult secret = preferences_->readAmapWebServiceKey();
    if (!secret.succeeded() || secret.value.isEmpty()) {
        error_text_ = tr("The stored AMap Web Service key is unavailable.");
        emit stateChanged();
        return;
    }

    QUrl url = endpoint_;
    QUrlQuery url_query(url);
    url_query.addQueryItem(QStringLiteral("keywords"), normalized);
    url_query.addQueryItem(QStringLiteral("offset"), QStringLiteral("10"));
    url_query.addQueryItem(QStringLiteral("page"), QStringLiteral("1"));
    url_query.addQueryItem(QStringLiteral("extensions"), QStringLiteral("base"));
    url_query.addQueryItem(QStringLiteral("output"), QStringLiteral("JSON"));
    if (shadow::desktop::maps::amapDomesticCoordinateSupported(latitude, longitude)) {
        const shadow::desktop::maps::GeographicCoordinate provider_coordinate =
            shadow::desktop::maps::wgs84ToGcj02(latitude, longitude);
        url_query.addQueryItem(
            QStringLiteral("location"),
            QStringLiteral("%1,%2")
                .arg(
                    QString::number(provider_coordinate.longitude, 'f', 6),
                    QString::number(provider_coordinate.latitude, 'f', 6)
                )
        );
        url_query.addQueryItem(QStringLiteral("sortrule"), QStringLiteral("distance"));
    }
    url_query.addQueryItem(QStringLiteral("key"), secret.value);
    url.setQuery(url_query);

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(
        QNetworkRequest::CacheLoadControlAttribute,
        QNetworkRequest::AlwaysNetwork
    );
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy
    );
    request.setTransferTimeout(REQUEST_TIMEOUT_MS);
    busy_ = true;
    const std::uint64_t request_generation = ++generation_;
    reply_ = network_->get(request);
    connect(reply_, &QNetworkReply::finished, this, [this, request_generation]() {
        finish(request_generation);
    });
    emit stateChanged();
}

void AmapPlaceSearchService::clear() {
    cancelRequest();
    active_query_.clear();
    results_.clear();
    error_text_.clear();
    emit stateChanged();
}

void AmapPlaceSearchService::cancelRequest() noexcept {
    ++generation_;
    if (reply_ != nullptr) {
        QNetworkReply* const reply = std::exchange(reply_, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    busy_ = false;
}

void AmapPlaceSearchService::finish(const std::uint64_t generation) {
    if (generation != generation_ || reply_ == nullptr) {
        return;
    }
    QNetworkReply* const reply = std::exchange(reply_, nullptr);
    const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError network_error = reply->error();
    const QByteArray payload = reply->read(MAXIMUM_RESPONSE_BYTES + 1);
    reply->deleteLater();
    busy_ = false;
    results_.clear();
    error_text_.clear();
    if (network_error != QNetworkReply::NoError || http_status != 200) {
        error_text_ = http_status == 401 || http_status == 403
                          ? tr("AMap rejected the Web Service key or its API permission.")
                          : tr("AMap place search is temporarily unavailable.");
        emit stateChanged();
        return;
    }
    if (payload.size() > MAXIMUM_RESPONSE_BYTES) {
        error_text_ = tr("AMap returned an oversized place-search response.");
        emit stateChanged();
        return;
    }
    const shadow::desktop::maps::AmapPlaceSearchResponse response =
        shadow::desktop::maps::parseAmapPlaceSearchResponse(payload);
    if (response.failure) {
        error_text_ = tr("AMap could not complete this place search.");
        emit stateChanged();
        return;
    }
    results_.reserve(static_cast<qsizetype>(response.results.size()));
    for (const auto& result : response.results) {
        const shadow::desktop::maps::GeographicCoordinate coordinate =
            shadow::desktop::maps::gcj02ToWgs84(
                result.gcj02_latitude,
                result.gcj02_longitude
            );
        results_.push_back(QVariantMap{
            {QStringLiteral("key"), QStringLiteral("amap:") + result.id},
            {QStringLiteral("label"), placeLabel(result)},
            {QStringLiteral("name"), result.name},
            {QStringLiteral("latitude"), coordinate.latitude},
            {QStringLiteral("longitude"), coordinate.longitude},
            {QStringLiteral("provider"), QStringLiteral("amap")},
        });
    }
    emit stateChanged();
}

void AmapPlaceSearchService::resetUnavailable() {
    cancelRequest();
    results_.clear();
    error_text_.clear();
    emit stateChanged();
}
