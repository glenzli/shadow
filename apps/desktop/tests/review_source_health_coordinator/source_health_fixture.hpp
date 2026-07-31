#pragma once

#include "review_source_health_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace review_source_health_test {

using SourceHealthOperation = std::function<QVector<BackendLibrarySourceHealth>()>;
using RemoveSourceOperation = std::function<bool(const QString&)>;
using MissingLocationOperation =
    std::function<BackendMissingSourceLocationPage(const QString&, const QString&, std::uint32_t)>;
using RelinkOperation = std::function<
    BackendVerifiedSourceRelinkReceipt(const QString&, const QString&, const QString&)>;
using LibraryRelinkOperation =
    std::function<BackendVerifiedSourceRelinkReceipt(const QString&, const QString&)>;
using ArchivePhotoOperation = std::function<bool(const QString&)>;

inline void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "source-health coordinator contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate> void wait_until(Predicate predicate, const std::string& message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

inline ReviewSourceHealthCoordinator::Operations operations(
    SourceHealthOperation source_health = [] { return QVector<BackendLibrarySourceHealth>{}; },
    MissingLocationOperation missing_locations =
        [](const QString&, const QString&, const std::uint32_t) {
            return BackendMissingSourceLocationPage{};
        },
    RelinkOperation relink = [](const QString&,
                                const QString&,
                                const QString&) { return BackendVerifiedSourceRelinkReceipt{}; },
    RemoveSourceOperation remove_source = [](const QString&) { return true; },
    LibraryRelinkOperation relink_library =
        [](const QString&, const QString&) { return BackendVerifiedSourceRelinkReceipt{}; },
    ArchivePhotoOperation archive_photo = [](const QString&) { return true; }
) {
    if (!source_health) {
        source_health = [] { return QVector<BackendLibrarySourceHealth>{}; };
    }
    if (!missing_locations) {
        missing_locations = [](const QString&, const QString&, const std::uint32_t) {
            return BackendMissingSourceLocationPage{};
        };
    }
    if (!relink) {
        relink = [](const QString&, const QString&, const QString&) {
            return BackendVerifiedSourceRelinkReceipt{};
        };
    }
    if (!remove_source) {
        remove_source = [](const QString&) { return true; };
    }
    if (!relink_library) {
        relink_library = [](const QString&, const QString&) {
            return BackendVerifiedSourceRelinkReceipt{};
        };
    }
    if (!archive_photo) {
        archive_photo = [](const QString&) { return true; };
    }
    return {
        .source_health = std::move(source_health),
        .remove_source = std::move(remove_source),
        .missing_locations = std::move(missing_locations),
        .relink = std::move(relink),
        .relink_library = std::move(relink_library),
        .archive_photo = std::move(archive_photo),
    };
}

inline BackendMissingSourceLocation location(const QString& identity) {
    return {
        .location_id = identity + QStringLiteral("-location"),
        .photo_id = identity + QStringLiteral("-photo"),
        .title = identity + QStringLiteral("-title"),
        .source_display_path =
            QStringLiteral("/former-source/") + identity + QStringLiteral(".nef"),
        .has_captured_at = true,
        .captured_at_unix_seconds = 1'700'000'123,
        .camera_key = identity + QStringLiteral("-camera"),
        .last_seen_at_ms = 1'700'000'456'789,
    };
}

} // namespace review_source_health_test
