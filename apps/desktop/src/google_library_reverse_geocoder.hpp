#pragma once

#include "library_reverse_geocoder.hpp"

#include <QUrl>

#include <memory>

class MapProviderPreferences;
class QNetworkAccessManager;

/// Creates the explicitly authorized Google Geocoding API provider.
///
/// The production overload owns its network manager. The injected overload is
/// reserved for deterministic provider-contract tests and local protocol
/// validation; neither overload exposes the stored API key to QML.
[[nodiscard]] std::unique_ptr<LibraryReverseGeocoder>
makeGoogleLibraryReverseGeocoder(MapProviderPreferences* preferences);

[[nodiscard]] std::unique_ptr<LibraryReverseGeocoder> makeGoogleLibraryReverseGeocoder(
    MapProviderPreferences* preferences,
    QNetworkAccessManager* network,
    const QUrl& endpoint
);
