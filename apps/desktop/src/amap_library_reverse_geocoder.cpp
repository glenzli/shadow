#include "amap_library_reverse_geocoder.hpp"

#include "amap_coordinate_transform.hpp"
#include "amap_web_service_protocol.hpp"
#include "map_provider_preferences.hpp"

#include <QCoreApplication>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QUrlQuery>

#include <cstdint>
#include <optional>
#include <utility>

namespace {

constexpr qsizetype MAXIMUM_RESPONSE_BYTES = 1024 * 1024;
constexpr int REQUEST_TIMEOUT_MS = 15'000;

[[nodiscard]] QString requestFailure(const int http_status) {
    if (http_status == 401 || http_status == 403) {
        return QCoreApplication::translate(
            "AmapLibraryReverseGeocoder",
            "AMap rejected the Web Service key or its API permission."
        );
    }
    if (http_status == 429) {
        return QCoreApplication::translate(
            "AmapLibraryReverseGeocoder",
            "AMap reverse geocoding is rate limited."
        );
    }
    if (http_status >= 500) {
        return QCoreApplication::translate(
            "AmapLibraryReverseGeocoder",
            "AMap reverse geocoding is temporarily unavailable."
        );
    }
    return QCoreApplication::translate(
        "AmapLibraryReverseGeocoder",
        "Could not reach AMap reverse geocoding."
    );
}

[[nodiscard]] QString providerFailure(const shadow::desktop::maps::AmapServiceFailure& failure) {
    if (failure.code == QStringLiteral("10001") || failure.code == QStringLiteral("10002")
        || failure.code == QStringLiteral("10003") || failure.code == QStringLiteral("10004")
        || failure.code == QStringLiteral("10007") || failure.code == QStringLiteral("10009")) {
        return QCoreApplication::translate(
            "AmapLibraryReverseGeocoder",
            "AMap rejected the Web Service key or its API permission."
        );
    }
    if (failure.code == QStringLiteral("10021") || failure.code == QStringLiteral("10044")) {
        return QCoreApplication::translate(
            "AmapLibraryReverseGeocoder",
            "AMap reverse-geocoding quota is exhausted."
        );
    }
    return QCoreApplication::translate(
        "AmapLibraryReverseGeocoder",
        "AMap returned no usable address for these coordinates."
    );
}

class AmapLibraryReverseGeocoder final : public QObject, public LibraryReverseGeocoder {
  public:
    AmapLibraryReverseGeocoder(
        MapProviderPreferences* const preferences,
        QNetworkAccessManager* const network,
        QUrl endpoint
    ) :
        preferences_(preferences),
        network_(network != nullptr ? network : new QNetworkAccessManager(this)),
        endpoint_(std::move(endpoint)) {
        Q_ASSERT(preferences_ != nullptr);
        const auto finish_if_unavailable = [this]() {
            if (!available()) {
                finishUnavailableRequest();
            }
        };
        QObject::connect(
            preferences_,
            &MapProviderPreferences::amapWebServiceKeyStoredChanged,
            this,
            finish_if_unavailable
        );
        QObject::connect(
            preferences_,
            &MapProviderPreferences::amapReverseGeocodingAllowedChanged,
            this,
            finish_if_unavailable
        );
    }

    ~AmapLibraryReverseGeocoder() override {
        cancel();
    }

    [[nodiscard]] bool available() const noexcept override {
        return preferences_ != nullptr && preferences_->amapWebServiceKeyStored()
               && preferences_->amapReverseGeocodingAllowed();
    }

    void reverseGeocode(
        const BackendLibraryPlaceResolutionCandidate& candidate,
        Completion completion
    ) override {
        cancel();
        if (!available()) {
            completion(
                std::nullopt,
                QCoreApplication::translate(
                    "AmapLibraryReverseGeocoder",
                    "AMap reverse geocoding is not authorized in Map & Location Services."
                )
            );
            return;
        }
        const double latitude = static_cast<double>(candidate.latitude_e7) / 10'000'000.0;
        const double longitude = static_cast<double>(candidate.longitude_e7) / 10'000'000.0;
        if (!shadow::desktop::maps::amapDomesticCoordinateSupported(latitude, longitude)) {
            completion(
                std::nullopt,
                QCoreApplication::translate(
                    "AmapLibraryReverseGeocoder",
                    "These coordinates are outside the enabled AMap domestic service area."
                )
            );
            return;
        }
        const SecretStoreResult secret = preferences_->readAmapWebServiceKey();
        if (!secret.succeeded() || secret.value.isEmpty()) {
            completion(
                std::nullopt,
                QCoreApplication::translate(
                    "AmapLibraryReverseGeocoder",
                    "The stored AMap Web Service key is unavailable."
                )
            );
            return;
        }

        const shadow::desktop::maps::GeographicCoordinate provider_coordinate =
            shadow::desktop::maps::wgs84ToGcj02(latitude, longitude);
        QUrl url = endpoint_;
        QUrlQuery query(url);
        query.addQueryItem(QStringLiteral("output"), QStringLiteral("JSON"));
        query.addQueryItem(
            QStringLiteral("location"),
            QStringLiteral("%1,%2")
                .arg(
                    QString::number(provider_coordinate.longitude, 'f', 6),
                    QString::number(provider_coordinate.latitude, 'f', 6)
                )
        );
        query.addQueryItem(QStringLiteral("extensions"), QStringLiteral("base"));
        query.addQueryItem(QStringLiteral("radius"), QStringLiteral("1000"));
        query.addQueryItem(QStringLiteral("key"), secret.value);
        url.setQuery(query);

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

        const std::uint64_t request_generation = ++generation_;
        completion_ = std::move(completion);
        reply_ = network_->get(request);
        QObject::connect(reply_, &QNetworkReply::finished, this, [this, request_generation]() {
            finish(request_generation);
        });
    }

    void cancel() noexcept override {
        ++generation_;
        completion_ = {};
        if (reply_ != nullptr) {
            QNetworkReply* const reply = std::exchange(reply_, nullptr);
            reply->abort();
            reply->deleteLater();
        }
    }

  private:
    void finishUnavailableRequest() {
        auto completion = std::move(completion_);
        cancel();
        if (completion) {
            completion(
                std::nullopt,
                QCoreApplication::translate(
                    "AmapLibraryReverseGeocoder",
                    "AMap authorization changed before the request completed."
                )
            );
        }
    }

    void finish(const std::uint64_t request_generation) {
        if (request_generation != generation_ || reply_ == nullptr) {
            return;
        }
        QNetworkReply* const reply = std::exchange(reply_, nullptr);
        auto completion = std::move(completion_);
        completion_ = {};
        const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError network_error = reply->error();
        const QByteArray payload = reply->read(MAXIMUM_RESPONSE_BYTES + 1);
        reply->deleteLater();
        if (!completion) {
            return;
        }
        if (network_error != QNetworkReply::NoError || http_status != 200) {
            completion(std::nullopt, requestFailure(http_status));
            return;
        }
        if (payload.size() > MAXIMUM_RESPONSE_BYTES) {
            completion(
                std::nullopt,
                QCoreApplication::translate(
                    "AmapLibraryReverseGeocoder",
                    "AMap returned an oversized reverse-geocoding response."
                )
            );
            return;
        }
        const shadow::desktop::maps::AmapReverseGeocodeResponse parsed =
            shadow::desktop::maps::parseAmapReverseGeocodeResponse(payload);
        if (!parsed.result) {
            completion(
                std::nullopt,
                parsed.failure ? providerFailure(*parsed.failure)
                               : QCoreApplication::translate(
                                     "AmapLibraryReverseGeocoder",
                                     "AMap returned no usable address for these coordinates."
                                 )
            );
            return;
        }
        const auto& address = *parsed.result;
        completion(
            BackendLibraryPlaceResolutionResult{
                .country_code = address.country_code,
                .country_name = address.country_name,
                .administrative_area = address.administrative_area,
                .locality = address.locality,
                .display_name = address.display_name,
                .provider_id = QStringLiteral("amap-geocoding"),
                .provider_version = QStringLiteral("v3"),
                .locale = QLocale::system().bcp47Name(),
            },
            {}
        );
    }

    MapProviderPreferences* preferences_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    QUrl endpoint_;
    QNetworkReply* reply_ = nullptr;
    Completion completion_;
    std::uint64_t generation_ = 0;
};

} // namespace

std::unique_ptr<LibraryReverseGeocoder>
makeAmapLibraryReverseGeocoder(MapProviderPreferences* const preferences) {
    return std::make_unique<AmapLibraryReverseGeocoder>(
        preferences,
        nullptr,
        QUrl(QStringLiteral("https://restapi.amap.com/v3/geocode/regeo"))
    );
}

std::unique_ptr<LibraryReverseGeocoder> makeAmapLibraryReverseGeocoder(
    MapProviderPreferences* const preferences,
    QNetworkAccessManager* const network,
    const QUrl& endpoint
) {
    return std::make_unique<AmapLibraryReverseGeocoder>(preferences, network, endpoint);
}
