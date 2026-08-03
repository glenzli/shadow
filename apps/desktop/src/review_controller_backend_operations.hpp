#pragma once

#include "desktop_backend.hpp"
#include "review_comparison_coordinator.hpp"
#include "review_decision_coordinator.hpp"
#include "review_import_coordinator.hpp"
#include "review_library_album_coordinator.hpp"
#include "review_library_facet_coordinator.hpp"
#include "review_library_keyword_coordinator.hpp"
#include "review_library_map_coordinator.hpp"
#include "review_library_metadata_coordinator.hpp"
#include "review_library_organization_coordinator.hpp"
#include "review_library_place_resolution_coordinator.hpp"
#include "review_library_query_coordinator.hpp"
#include "review_model.hpp"
#include "review_shared_grade_coordinator.hpp"
#include "review_source_health_coordinator.hpp"
#include "review_travel_collection_coordinator.hpp"

#include <memory>

namespace ReviewControllerBackendOperations {

[[nodiscard]] ReviewImportCoordinator::Operations
import_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewLibraryQueryCoordinator::Operations
query_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewComparisonCoordinator::Operations
comparison_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewSourceHealthCoordinator::Operations
source_health_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewLibraryAlbumCoordinator::Operations
album_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewLibraryFacetCoordinator::Operations
facet_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewTravelCollectionCoordinator::Operations
travel_collection_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewLibraryKeywordCoordinator::Operations
keyword_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewLibraryMetadataCoordinator::Operations
metadata_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewLibraryMapCoordinator::Operations
map_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewLibraryPlaceResolutionCoordinator::Operations
place_resolution_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewLibraryOrganizationCoordinator::Operations
organization_operations(const std::shared_ptr<DesktopBackend>& backend, ReviewModel& model);
[[nodiscard]] ReviewSharedGradeCoordinator::Operations
shared_grade_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] ReviewDecisionCoordinator::Operations
decision_operations(const std::shared_ptr<DesktopBackend>& backend);
[[nodiscard]] BackendReviewDecisionState
backend_decision_state(const QString& photo_id, const ReviewDecisionValue& value);

} // namespace ReviewControllerBackendOperations
