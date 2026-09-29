#include "review_controller_backend_operations.hpp"

#include <optional>
#include <stdexcept>

namespace ReviewControllerBackendOperations {

[[nodiscard]] ReviewImportCoordinator::Operations
import_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review import backend is required");
    }
    return {
        .begin = [backend](const quint64 scan_id) { backend->beginFolderScan(scan_id); },
        .scan =
            [backend](const QString& folder_path, const quint64 scan_id) {
                return backend->scanFolder(folder_path, scan_id);
            },
        .progress = [backend](const quint64 scan_id) { return backend->scanProgress(scan_id); },
        .cancel = [backend](const quint64 scan_id) { return backend->cancelFolderScan(scan_id); },
    };
}

[[nodiscard]] ReviewLibraryQueryCoordinator::Operations
query_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review Library query backend is required");
    }
    return {
        .page = [backend](
                    const BackendLibraryPhotoFilter& filter,
                    const BackendLibraryPhotoOrder order,
                    const BackendLibraryPhotoCursor& cursor,
                    const std::uint32_t limit
                ) { return backend->libraryPhotoPage(filter, order, cursor, limit); },
        .count = [backend](
                     const BackendLibraryPhotoFilter& filter
                 ) { return backend->libraryPhotoCount(filter); },
    };
}

[[nodiscard]] ReviewLocationCompletionCoordinator::Operations
location_completion_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review location-completion backend is required");
    }
    return {
        .page = [backend](
                    const BackendLibraryPhotoFilter& filter,
                    const BackendLibraryPhotoOrder order,
                    const BackendLibraryPhotoCursor& cursor,
                    const std::uint32_t limit
                ) { return backend->libraryPhotoPage(filter, order, cursor, limit); },
        .anchors = [backend](const std::int64_t start, const std::int64_t end) {
            return backend->locationReferenceAnchors(start, end);
        },
    };
}

[[nodiscard]] ReviewLocationReferenceCoordinator::Operations
location_reference_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review location-reference backend is required");
    }
    return {
        .libraries = [backend]() { return backend->locationReferenceLibraries(); },
        .add_or_rescan = [backend](const QString& root_path, const std::int64_t clock_offset_seconds) {
            return backend->addLocationReferenceLibrary(root_path, clock_offset_seconds);
        },
        .remove = [backend](const QString& id) { return backend->removeLocationReferenceLibrary(id); },
    };
}

[[nodiscard]] ReviewComparisonCoordinator::Operations
comparison_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review comparison backend is required");
    }
    return {
        .prepare =
            [backend](const QString& left_visual_handle, const QString& right_visual_handle) {
                return backend->prepareReviewComparison(left_visual_handle, right_visual_handle);
            },
        .confirm_ready =
            [backend](
                const QString& presentation_id,
                const QString& left_request_ticket,
                const QString& right_request_ticket
            ) {
                backend->confirmReviewComparisonReady(
                    presentation_id,
                    left_request_ticket,
                    right_request_ticket
                );
            },
        .cancel = [backend](
                      const QString& presentation_id
                  ) { backend->cancelReviewComparison(presentation_id); },
        .record =
            [backend](const QString& presentation_id, const BackendPairwiseOutcome outcome) {
                return backend->recordReviewComparison(presentation_id, outcome);
            },
        .forget =
            [backend](const QString& event_id) { return backend->forgetReviewFeedback(event_id); },
    };
}

[[nodiscard]] ReviewSourceHealthCoordinator::Operations
source_health_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review source-health backend is required");
    }
    return {
        .source_health = [backend]() { return backend->librarySourceHealth(); },
        .remove_source =
            [backend](const QString& source_id) { return backend->removeLibrarySource(source_id); },
        .missing_locations =
            [backend](
                const QString& scan_session_id,
                const QString& after_location_id,
                const std::uint32_t limit
            ) {
                return backend
                    ->missingSourceLocationPage(scan_session_id, after_location_id, limit);
            },
        .relink =
            [backend](
                const QString& scan_session_id,
                const QString& location_id,
                const QString& candidate_path
            ) {
                return backend
                    ->relinkMissingSourceLocation(scan_session_id, location_id, candidate_path);
            },
        .relink_library =
            [backend](const QString& location_id, const QString& candidate_path) {
                return backend->relinkLibrarySourceLocation(location_id, candidate_path);
            },
        .recover_source =
            [backend](const QString& source_id, const QString& replacement_folder) {
                return backend->recoverLibrarySource(source_id, replacement_folder);
            },
        .reconcile_missing = [backend](
                                 const QString& scan_session_id
                             ) { return backend->reconcileMissingSourcePhotos(scan_session_id); },
        .archive_photo =
            [backend](const QString& photo_id) { return backend->archiveLibraryPhoto(photo_id); },
    };
}

[[nodiscard]] ReviewLibraryAlbumCoordinator::Operations
album_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review Library album backend is required");
    }
    return {
        .albums = [backend]() { return backend->libraryAlbums(); },
        .create_manual = [backend](
                             const QString& name
                         ) { static_cast<void>(backend->createManualLibraryAlbum(name)); },
        .create_smart =
            [backend](const QString& name, const BackendLibraryPhotoFilter& query) {
                static_cast<void>(backend->createSmartLibraryAlbum(name, query));
            },
        .rename =
            [backend](const QString& album_id, const QString& name) {
                static_cast<void>(backend->renameLibraryAlbum(album_id, name));
            },
        .remove = [backend](
                      const QString& album_id
                  ) { static_cast<void>(backend->deleteLibraryAlbum(album_id)); },
        .add_photo =
            [backend](const QString& album_id, const QString& photo_id) {
                backend->addPhotoToManualLibraryAlbum(album_id, photo_id);
            },
        .remove_photo =
            [backend](const QString& album_id, const QString& photo_id) {
                static_cast<void>(backend->removePhotoFromManualLibraryAlbum(album_id, photo_id));
            },
    };
}

[[nodiscard]] ReviewLibraryKeywordCoordinator::Operations
keyword_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review Library keyword backend is required");
    }
    return {
        .tree = [backend]() { return backend->libraryKeywords(); },
        .for_photo = [backend](
                         const QString& photo_id
                     ) { return backend->libraryKeywordsForPhoto(photo_id); },
        .create =
            [backend](const QString& parent_id, const QString& name) {
                static_cast<void>(backend->createLibraryKeyword(parent_id, name));
            },
        .rename =
            [backend](const QString& keyword_id, const QString& name) {
                static_cast<void>(backend->renameLibraryKeyword(keyword_id, name));
            },
        .move =
            [backend](const QString& keyword_id, const QString& parent_id) {
                static_cast<void>(backend->moveLibraryKeyword(keyword_id, parent_id));
            },
        .remove = [backend](
                      const QString& keyword_id
                  ) { return backend->deleteLibraryKeywordSubtree(keyword_id); },
        .assign =
            [backend](const QString& keyword_id, const QStringList& photo_ids) {
                return backend->assignLibraryKeyword(keyword_id, photo_ids);
            },
        .unassign =
            [backend](const QString& keyword_id, const QStringList& photo_ids) {
                return backend->removeLibraryKeyword(keyword_id, photo_ids);
            },
    };
}

[[nodiscard]] ReviewLibraryFacetCoordinator::Operations
facet_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review Library facet backend is required");
    }
    return {
        .page = [backend](
                    const BackendLibraryPhotoFilter& filter,
                    const BackendLibraryFacetKind kind,
                    const BackendLibraryFacetCursor& cursor,
                    const std::uint32_t limit
                ) { return backend->libraryFacetPage(filter, kind, cursor, limit); },
        .count = [backend](
                     const BackendLibraryPhotoFilter& filter
                 ) { return backend->libraryPhotoCount(filter); },
    };
}

[[nodiscard]] ReviewTravelCollectionCoordinator::Operations
travel_collection_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review Travel collection backend is required");
    }
    return {
        .page = [backend](
                    const BackendLibraryPhotoFilter& filter,
                    const BackendLibraryFacetKind kind,
                    const BackendLibraryFacetCursor& cursor,
                    const std::uint32_t limit
                ) { return backend->libraryFacetPage(filter, kind, cursor, limit); },
        .count = [backend](
                     const BackendLibraryPhotoFilter& filter
                 ) { return backend->libraryPhotoCount(filter); },
    };
}

[[nodiscard]] ReviewLibraryMetadataCoordinator::Operations
metadata_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review Library metadata backend is required");
    }
    return {
        .load =
            [backend](const QString& photo_id) { return backend->libraryMetadataState(photo_id); },
        .set_capture_time =
            [backend](
                const QString& photo_id,
                const QString& mode,
                const std::int64_t captured_at_unix_seconds
            ) {
                return backend
                    ->setLibraryCaptureTimeOverride(photo_id, mode, captured_at_unix_seconds);
            },
        .set_coordinates =
            [backend](
                const QString& photo_id,
                const QString& mode,
                const double latitude_degrees,
                const double longitude_degrees,
                const QString& place_name
            ) {
                return backend->setLibraryCoordinatesOverride(
                    photo_id,
                    mode,
                    latitude_degrees,
                    longitude_degrees,
                    place_name
                );
            },
        .preview_capture_time =
            [backend](
                const QVector<BackendBatchPhotoTarget>& targets,
                const QString& mode,
                const std::int64_t offset_seconds
            ) { return backend->previewLibraryCaptureTimeBatch(targets, mode, offset_seconds); },
        .apply_capture_time = [backend](
                                  const QString& preview_id
                              ) { return backend->applyLibraryCaptureTimeBatch(preview_id); },
        .preview_coordinates =
            [backend](
                const QVector<BackendBatchPhotoTarget>& targets,
                const QString& mode,
                const double latitude_degrees,
                const double longitude_degrees,
                const QString& place_name,
                const QString& source_label
            ) {
                return backend->previewLibraryCoordinateBatch(
                    targets,
                    mode,
                    latitude_degrees,
                    longitude_degrees,
                    place_name,
                    source_label
                );
            },
        .apply_coordinates = [backend](
                                 const QString& preview_id
                             ) { return backend->applyLibraryCoordinateBatch(preview_id); },
        .preview_gpx =
            [backend](
                const QString& path,
                const QVector<BackendBatchPhotoTarget>& targets,
                const std::int64_t offset_seconds,
                const std::uint32_t maximum_gap_seconds
            ) {
                return backend
                    ->previewLibraryGpxImport(path, targets, offset_seconds, maximum_gap_seconds);
            },
        .apply_gpx = [backend](
                         const QString& preview_id
                     ) { return backend->applyLibraryGpxImport(preview_id); },
    };
}

[[nodiscard]] ReviewLibraryMapCoordinator::Operations
map_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review Library map backend is required");
    }
    return {
        .snapshot = [backend](
                        const BackendLibraryPhotoFilter& filter,
                        const BackendLibraryMapViewport& viewport,
                        const BackendLibraryMapGrid& grid
                    ) { return backend->libraryMapSnapshot(filter, viewport, grid); },
    };
}

[[nodiscard]] ReviewLibraryPlaceResolutionCoordinator::Operations
place_resolution_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review Library place-resolution backend is required");
    }
    return {
        .candidates = [backend](
                          const std::uint32_t limit
                      ) { return backend->libraryPlaceResolutionCandidates(limit); },
        .record = [backend](
                      const BackendLibraryPlaceResolutionResult& result
                  ) { return backend->recordLibraryPlaceResolution(result); },
    };
}

[[nodiscard]] ReviewLibraryOrganizationCoordinator::Operations
organization_operations(const std::shared_ptr<DesktopBackend>& backend, ReviewModel& model) {
    if (!backend) {
        throw std::invalid_argument("Review Library organization backend is required");
    }
    return {
        .current = [&model](const QString& photo_id)
            -> std::optional<ReviewLibraryOrganizationCoordinator::CurrentState> {
            const auto current = model.libraryStateFor(photo_id);
            if (!current) {
                return std::nullopt;
            }
            return ReviewLibraryOrganizationCoordinator::CurrentState{
                .liked = current->liked,
                .color_label = current->color_label,
            };
        },
        .mutate =
            [backend](const QString& photo_id, const bool liked, const QString& color_label) {
                return backend->setPhotoLibraryState(photo_id, liked, color_label);
            },
        .project =
            [&model](const BackendPhotoLibraryState& state) {
                return model.updateLibraryState(
                    state.photo_id,
                    state.liked,
                    state.color_label,
                    state.updated_at_ms
                );
            },
    };
}

[[nodiscard]] ReviewSharedGradeCoordinator::Operations
shared_grade_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review shared Grade Node backend is required");
    }
    return {
        .nodes = [backend]() { return backend->sharedGradeNodes(); },
        .apply =
            [backend](const QString& layer_id, const QVector<BackendBatchPhotoTarget>& targets) {
                return backend->applySharedGradeNodeToPhotos(layer_id, targets);
            },
    };
}

[[nodiscard]] ReviewDecisionCoordinator::Operations
decision_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review decision backend is required");
    }
    return {
        .mutate =
            [backend](
                const QString& photo_id,
                const std::uint64_t expected_head_sequence,
                const BackendReviewDecisionFlag desired_flag,
                const std::uint8_t desired_rating
            ) {
                return backend->setReviewPhotoDecision(
                    photo_id,
                    expected_head_sequence,
                    desired_flag,
                    desired_rating
                );
            },
        .authoritative_state = [backend](
                                   const QString& photo_id
                               ) { return backend->reviewPhotoDecisionState(photo_id); },
    };
}

[[nodiscard]] ReviewRemoteLibraryCoordinator::Operations
remote_library_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Remote Library backend is required");
    }
    return {
        .snapshot = [backend](
                        const QString& connection_id
                    ) { return backend->remoteLibrarySnapshot(connection_id); },
        .begin_sync =
            [backend](
                const QString& connection_id,
                const QString& server_address,
                const QString& authorization
            ) {
                return backend->beginRemoteLibrarySync(
                    connection_id,
                    server_address,
                    authorization
                );
            },
        .sync_step =
            [backend](const std::uint64_t job_id) {
                return backend->stepRemoteLibrarySync(job_id);
            },
        .cancel_sync =
            [backend](const std::uint64_t job_id) {
                return backend->cancelRemoteLibrarySync(job_id);
            },
        .set_review_state =
            [backend](
                const QString& connection_id,
                const QString& remote_photo_id,
                const QString& remote_representation_id,
                const BackendReviewDecisionFlag flag,
                const std::uint8_t rating,
                const bool liked,
                const QString& color_label,
                const std::int64_t updated_at_ms
            ) {
                backend->setRemoteLibraryReviewState(
                    connection_id,
                    remote_photo_id,
                    remote_representation_id,
                    flag,
                    rating,
                    liked,
                    color_label,
                    updated_at_ms
                );
            },
        .materialize =
            [backend](
                const QString& connection_id,
                const QString& server_address,
                const QString& authorization,
                const QString& remote_photo_id,
                const QString& remote_representation_id
            ) {
                return backend->materializeRemoteLibraryPhoto(
                    connection_id,
                    server_address,
                    authorization,
                    remote_photo_id,
                    remote_representation_id
                );
            },
    };
}

[[nodiscard]] BackendReviewDecisionState
backend_decision_state(const QString& photo_id, const ReviewDecisionValue& value) {
    const auto flag = review_decision_flag_from_name(value.flag);
    if (!flag || value.rating < 0 || value.rating > 5) {
        throw std::invalid_argument("Review model contains an invalid decision state");
    }
    return {
        .photo_id = photo_id,
        .head_sequence = value.head_sequence,
        .flag = *flag,
        .rating = static_cast<std::uint8_t>(value.rating),
    };
}

} // namespace ReviewControllerBackendOperations
