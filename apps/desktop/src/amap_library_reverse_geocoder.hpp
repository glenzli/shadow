#pragma once

#include "library_reverse_geocoder.hpp"

#include <QUrl>

#include <memory>

class MapProviderPreferences;
class QNetworkAccessManager;

[[nodiscard]] std::unique_ptr<LibraryReverseGeocoder>
makeAmapLibraryReverseGeocoder(MapProviderPreferences* preferences);

[[nodiscard]] std::unique_ptr<LibraryReverseGeocoder> makeAmapLibraryReverseGeocoder(
    MapProviderPreferences* preferences,
    QNetworkAccessManager* network,
    const QUrl& endpoint
);
