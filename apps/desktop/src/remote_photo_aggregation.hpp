#pragma once

#include "backend/remote_library_types.hpp"

#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>

/// One independently addressable server copy of a remote logical photo.
struct RemotePhotoSourceChoice final {
    QString connection_id;
    BackendRemoteLibraryPhoto photo;
    bool originals_available = false;
};

/// A path- and server-independent presentation assembled from remote mirrors.
///
/// Exact original identities merge copies across servers. Until a server has
/// prepared that identity, the source remains isolated by its server Photo ID
/// so filename similarities never cause a destructive false merge.
struct RemotePhotoAggregate final {
    QString presentation_photo_id;
    QVector<RemotePhotoSourceChoice> sources;
    qsizetype preferred_source_index = 0;
    std::uint32_t representation_count = 1;
    std::uint32_t source_location_count = 1;
    bool has_raw_representation = false;
    bool has_raster_representation = false;
    bool is_materialized = false;

    [[nodiscard]] const RemotePhotoSourceChoice* preferredSource() const noexcept;
};

using RemotePhotoAggregateMap = QHash<QString, RemotePhotoAggregate>;

/// Rebuilds the logical remote-photo projection from complete per-connection
/// snapshots. The result owns copies of its source choices and is safe to keep
/// after the input snapshots change.
[[nodiscard]] RemotePhotoAggregateMap aggregateRemotePhotos(
    const QHash<QString, BackendRemoteLibrarySnapshot>& snapshots
);
