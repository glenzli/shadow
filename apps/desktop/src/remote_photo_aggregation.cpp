#include "remote_photo_aggregation.hpp"

#include <QStringList>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

[[nodiscard]] bool isHexDigest(const QString& digest) {
    if (digest.size() != 64) {
        return false;
    }
    return std::all_of(digest.cbegin(), digest.cend(), [](const QChar character) {
        return (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
               || (character >= QLatin1Char('a') && character <= QLatin1Char('f'))
               || (character >= QLatin1Char('A') && character <= QLatin1Char('F'));
    });
}

[[nodiscard]] QString presentationPhotoId(const BackendRemoteLibraryPhoto& photo) {
    if (photo.has_original_identity && isHexDigest(photo.original_digest_hex)) {
        return QStringLiteral("remote-content:%1").arg(photo.original_digest_hex.toLower());
    }
    return QStringLiteral("remote:%1:%2").arg(photo.server_id, photo.remote_photo_id);
}

[[nodiscard]] QString sourceIdentity(const RemotePhotoSourceChoice& source) {
    return source.photo.server_id + QChar::Null + source.photo.remote_photo_id + QChar::Null
           + source.photo.remote_representation_id;
}

[[nodiscard]] int sourceRank(const RemotePhotoSourceChoice& source) {
    int rank = 0;
    if (source.originals_available) {
        rank += 8;
    }
    if (source.photo.has_preview) {
        rank += 4;
    }
    if (source.photo.has_raw_representation) {
        rank += 2;
    }
    if (source.photo.has_cached_original) {
        rank += 16;
    }
    return rank;
}

[[nodiscard]] bool preferredBefore(
    const RemotePhotoSourceChoice& candidate,
    const RemotePhotoSourceChoice& current
) {
    const int candidate_rank = sourceRank(candidate);
    const int current_rank = sourceRank(current);
    if (candidate_rank != current_rank) {
        return candidate_rank > current_rank;
    }
    if (candidate.connection_id != current.connection_id) {
        return candidate.connection_id < current.connection_id;
    }
    if (candidate.photo.server_id != current.photo.server_id) {
        return candidate.photo.server_id < current.photo.server_id;
    }
    if (candidate.photo.remote_photo_id != current.photo.remote_photo_id) {
        return candidate.photo.remote_photo_id < current.photo.remote_photo_id;
    }
    return candidate.photo.remote_representation_id < current.photo.remote_representation_id;
}

[[nodiscard]] std::uint32_t saturatedAdd(
    const std::uint32_t left,
    const std::uint32_t right
) {
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    return right > maximum - left ? maximum : left + right;
}

void mergeAggregateSource(
    RemotePhotoAggregate& aggregate,
    RemotePhotoSourceChoice source
) {
    aggregate.representation_count =
        std::max(aggregate.representation_count, source.photo.representation_count);
    aggregate.has_raw_representation =
        aggregate.has_raw_representation || source.photo.has_raw_representation;
    aggregate.has_raster_representation =
        aggregate.has_raster_representation || source.photo.has_raster_representation;
    aggregate.has_cached_original =
        aggregate.has_cached_original || source.photo.has_cached_original;
    const QString identity = sourceIdentity(source);
    for (auto& existing : aggregate.sources) {
        if (sourceIdentity(existing) == identity) {
            if (preferredBefore(source, existing)) {
                existing = std::move(source);
            }
            return;
        }
    }

    aggregate.source_location_count = aggregate.sources.isEmpty()
                                          ? std::max<std::uint32_t>(
                                                1,
                                                source.photo.source_location_count
                                            )
                                          : saturatedAdd(
                                                aggregate.source_location_count,
                                                std::max<std::uint32_t>(
                                                    1,
                                                    source.photo.source_location_count
                                                )
                                            );
    aggregate.sources.push_back(std::move(source));
}

void finishAggregate(RemotePhotoAggregate& aggregate) {
    if (aggregate.sources.isEmpty()) {
        aggregate.preferred_source_index = 0;
        return;
    }
    qsizetype preferred = 0;
    for (qsizetype index = 1; index < aggregate.sources.size(); ++index) {
        if (preferredBefore(aggregate.sources.at(index), aggregate.sources.at(preferred))) {
            preferred = index;
        }
    }
    aggregate.preferred_source_index = preferred;
}

} // namespace

const RemotePhotoSourceChoice* RemotePhotoAggregate::preferredSource() const noexcept {
    return preferred_source_index >= 0 && preferred_source_index < sources.size()
               ? &sources.at(preferred_source_index)
               : nullptr;
}

RemotePhotoAggregateMap aggregateRemotePhotos(
    const QHash<QString, BackendRemoteLibrarySnapshot>& snapshots
) {
    RemotePhotoAggregateMap aggregates;
    QStringList connection_ids = snapshots.keys();
    std::sort(connection_ids.begin(), connection_ids.end());
    for (const auto& connection_id : connection_ids) {
        const auto found = snapshots.constFind(connection_id);
        if (found == snapshots.cend()) {
            continue;
        }
        const bool originals_available = found->has_server && found->server.originals_available;
        for (const auto& photo : found->photos) {
            const QString presentation_id = presentationPhotoId(photo);
            auto aggregate = aggregates.find(presentation_id);
            if (aggregate == aggregates.end()) {
                RemotePhotoAggregate created;
                created.presentation_photo_id = presentation_id;
                aggregate = aggregates.insert(presentation_id, std::move(created));
            }
            mergeAggregateSource(
                aggregate.value(),
                {
                    .connection_id = connection_id,
                    .photo = photo,
                    .originals_available = originals_available,
                }
            );
        }
    }
    for (auto aggregate = aggregates.begin(); aggregate != aggregates.end(); ++aggregate) {
        finishAggregate(aggregate.value());
    }
    return aggregates;
}
