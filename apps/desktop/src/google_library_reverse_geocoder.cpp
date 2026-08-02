#include "google_library_reverse_geocoder.hpp"

#include "map_provider_preferences.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QUrlQuery>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace {

constexpr qsizetype MAXIMUM_RESPONSE_BYTES = 1024 * 1024;
constexpr int REQUEST_TIMEOUT_MS = 15'000;

struct ParsedAddress final {
    QString country_code;
    QString country_name;
    QString administrative_area;
    QString locality;
    QString display_name;
    int locality_priority = std::numeric_limits<int>::max();
};

[[nodiscard]] QStringList componentTypes(const QJsonObject& component) {
    QStringList values;
    const QJsonArray source = component.value(QStringLiteral("types")).toArray();
    values.reserve(source.size());
    for (const QJsonValue& value : source) {
        if (value.isString()) {
            values.push_back(value.toString());
        }
    }
    return values;
}

[[nodiscard]] int localityPriority(const QStringList& types) {
    const QStringList priority{
        QStringLiteral("locality"),
        QStringLiteral("postal_town"),
        QStringLiteral("sublocality_level_1"),
        QStringLiteral("sublocality"),
        QStringLiteral("administrative_area_level_2"),
        QStringLiteral("administrative_area_level_1"),
    };
    for (int index = 0; index < priority.size(); ++index) {
        if (types.contains(priority.at(index))) {
            return index;
        }
    }
    return std::numeric_limits<int>::max();
}

[[nodiscard]] ParsedAddress parseAddress(const QJsonObject& result) {
    ParsedAddress address;
    address.display_name = result.value(QStringLiteral("formatted_address")).toString().trimmed();
    const QJsonArray components = result.value(QStringLiteral("address_components")).toArray();
    for (const QJsonValue& value : components) {
        const QJsonObject component = value.toObject();
        const QStringList types = componentTypes(component);
        const QString long_name = component.value(QStringLiteral("long_name")).toString().trimmed();
        const QString short_name =
            component.value(QStringLiteral("short_name")).toString().trimmed();
        if (types.contains(QStringLiteral("country"))) {
            address.country_code = short_name;
            address.country_name = long_name;
        }
        if (types.contains(QStringLiteral("administrative_area_level_1"))) {
            address.administrative_area = long_name;
        }
        const int priority = localityPriority(types);
        if (!long_name.isEmpty() && priority < address.locality_priority) {
            address.locality = long_name;
            address.locality_priority = priority;
        }
    }
    return address;
}

[[nodiscard]] std::optional<ParsedAddress> bestAddress(const QJsonArray& results) {
    std::optional<ParsedAddress> best;
    for (const QJsonValue& value : results) {
        ParsedAddress candidate = parseAddress(value.toObject());
        if (candidate.country_code.isEmpty() && candidate.country_name.isEmpty()) {
            continue;
        }
        if (!best || candidate.locality_priority < best->locality_priority
            || (candidate.locality_priority == best->locality_priority
                && best->display_name.isEmpty() && !candidate.display_name.isEmpty())) {
            best = std::move(candidate);
        }
    }
    return best;
}

[[nodiscard]] QString providerError(const QString& status) {
    if (status == QStringLiteral("ZERO_RESULTS")) {
        return QCoreApplication::translate(
            "GoogleLibraryReverseGeocoder",
            "Google returned no address for these coordinates."
        );
    }
    if (status == QStringLiteral("OVER_QUERY_LIMIT")) {
        return QCoreApplication::translate(
            "GoogleLibraryReverseGeocoder",
            "Google Geocoding API quota is exhausted or rate limited."
        );
    }
    if (status == QStringLiteral("REQUEST_DENIED")) {
        return QCoreApplication::translate(
            "GoogleLibraryReverseGeocoder",
            "Google rejected the Geocoding API request. Check that billing and the Geocoding "
            "API are enabled and that the key permits this API."
        );
    }
    if (status == QStringLiteral("INVALID_REQUEST")) {
        return QCoreApplication::translate(
            "GoogleLibraryReverseGeocoder",
            "Google rejected the reverse-geocoding coordinates."
        );
    }
    if (status == QStringLiteral("UNKNOWN_ERROR")) {
        return QCoreApplication::translate(
            "GoogleLibraryReverseGeocoder",
            "Google Geocoding API is temporarily unavailable."
        );
    }
    return QCoreApplication::translate(
        "GoogleLibraryReverseGeocoder",
        "Google returned an invalid reverse-geocoding response."
    );
}

class GoogleLibraryReverseGeocoder final : public QObject, public LibraryReverseGeocoder {
  public:
    GoogleLibraryReverseGeocoder(
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
            &MapProviderPreferences::googleApiKeyStoredChanged,
            this,
            finish_if_unavailable
        );
        QObject::connect(
            preferences_,
            &MapProviderPreferences::googleReverseGeocodingAllowedChanged,
            this,
            finish_if_unavailable
        );
    }

    ~GoogleLibraryReverseGeocoder() override {
        cancel();
    }

    [[nodiscard]] bool available() const noexcept override {
        return preferences_ != nullptr && preferences_->googleApiKeyStored()
               && preferences_->googleReverseGeocodingAllowed();
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
                    "GoogleLibraryReverseGeocoder",
                    "Google reverse geocoding is not authorized in Map & Location Services."
                )
            );
            return;
        }
        const SecretStoreResult secret = preferences_->readGoogleApiKey();
        if (!secret.succeeded() || secret.value.isEmpty()) {
            completion(
                std::nullopt,
                QCoreApplication::translate(
                    "GoogleLibraryReverseGeocoder",
                    "The stored Google API key is unavailable."
                )
            );
            return;
        }

        const double latitude = static_cast<double>(candidate.latitude_e7) / 10'000'000.0;
        const double longitude = static_cast<double>(candidate.longitude_e7) / 10'000'000.0;
        QUrl url = endpoint_;
        QUrlQuery query(url);
        query.addQueryItem(
            QStringLiteral("latlng"),
            QStringLiteral("%1,%2")
                .arg(QString::number(latitude, 'f', 7), QString::number(longitude, 'f', 7))
        );
        const QString locale = QLocale::system().bcp47Name();
        if (!locale.isEmpty()) {
            query.addQueryItem(QStringLiteral("language"), locale);
        }
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
                    "GoogleLibraryReverseGeocoder",
                    "Google precision lookup authorization changed before the request completed."
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
        QByteArray payload = reply->read(MAXIMUM_RESPONSE_BYTES + 1);
        reply->deleteLater();
        if (!completion) {
            return;
        }
        if (network_error != QNetworkReply::NoError || http_status != 200) {
            if (http_status == 401 || http_status == 403) {
                completion(
                    std::nullopt,
                    QCoreApplication::translate(
                        "GoogleLibraryReverseGeocoder",
                        "Google rejected the API key or its Geocoding API permission."
                    )
                );
            } else if (http_status == 429) {
                completion(
                    std::nullopt,
                    QCoreApplication::translate(
                        "GoogleLibraryReverseGeocoder",
                        "Google Geocoding API is rate limited."
                    )
                );
            } else if (http_status >= 500) {
                completion(
                    std::nullopt,
                    QCoreApplication::translate(
                        "GoogleLibraryReverseGeocoder",
                        "Google Geocoding API is temporarily unavailable."
                    )
                );
            } else {
                completion(
                    std::nullopt,
                    QCoreApplication::translate(
                        "GoogleLibraryReverseGeocoder",
                        "Could not reach Google Geocoding API."
                    )
                );
            }
            return;
        }
        if (payload.size() > MAXIMUM_RESPONSE_BYTES) {
            completion(
                std::nullopt,
                QCoreApplication::translate(
                    "GoogleLibraryReverseGeocoder",
                    "Google returned an oversized reverse-geocoding response."
                )
            );
            return;
        }
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(payload, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
            completion(
                std::nullopt,
                QCoreApplication::translate(
                    "GoogleLibraryReverseGeocoder",
                    "Google returned malformed reverse-geocoding data."
                )
            );
            return;
        }
        const QJsonObject root = document.object();
        const QString status = root.value(QStringLiteral("status")).toString();
        if (status != QStringLiteral("OK")) {
            completion(std::nullopt, providerError(status));
            return;
        }
        const auto address = bestAddress(root.value(QStringLiteral("results")).toArray());
        if (!address) {
            completion(
                std::nullopt,
                QCoreApplication::translate(
                    "GoogleLibraryReverseGeocoder",
                    "Google returned no structured country for these coordinates."
                )
            );
            return;
        }
        const QString locale = QLocale::system().bcp47Name();
        completion(
            BackendLibraryPlaceResolutionResult{
                .country_code = address->country_code,
                .country_name = address->country_name,
                .administrative_area = address->administrative_area,
                .locality = address->locality,
                .display_name = address->display_name,
                .provider_id = QStringLiteral("google-geocoding"),
                .provider_version = QStringLiteral("v1"),
                .locale = locale,
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
makeGoogleLibraryReverseGeocoder(MapProviderPreferences* const preferences) {
    return std::make_unique<GoogleLibraryReverseGeocoder>(
        preferences,
        nullptr,
        QUrl(QStringLiteral("https://maps.googleapis.com/maps/api/geocode/json"))
    );
}

std::unique_ptr<LibraryReverseGeocoder> makeGoogleLibraryReverseGeocoder(
    MapProviderPreferences* const preferences,
    QNetworkAccessManager* const network,
    const QUrl& endpoint
) {
    return std::make_unique<GoogleLibraryReverseGeocoder>(preferences, network, endpoint);
}
