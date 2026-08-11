#include "review_controller.hpp"

#include "map_provider_preferences.hpp"

#include <QCoreApplication>
#include <QTimer>

#include <utility>

// Owns the signal topology between the stable QML facade and its focused
// coordinators. Keeping this graph together makes cross-workflow invalidation
// and status routing reviewable without hiding it in the constructor.
void ReviewController::initializeCoordinatorWiring() {
    filtered_model_.setSourceModel(&model_);
    connect(&filtered_model_, &ReviewFilterModel::filtersChanged, this, [this]() {
        scheduleFilterQuery();
        refreshRemoteLibraryPresentation();
        emit itemCountChanged();
        emit filtersChanged();
    });
    connect(&filtered_model_, &ReviewFilterModel::semanticFilterChanged, this, [this]() {
        emit itemCountChanged();
        emit filtersChanged();
    });
    connect(&filtered_model_, &ReviewFilterModel::smartCategoryFilterChanged, this, [this]() {
        emit itemCountChanged();
        emit filtersChanged();
    });
    const auto notify_filtered_count = [this]() { emit filtersChanged(); };
    connect(&filtered_model_, &QAbstractItemModel::modelReset, this, notify_filtered_count);
    connect(
        &filtered_model_,
        &QAbstractItemModel::rowsInserted,
        this,
        [notify_filtered_count](const QModelIndex&, const int, const int) {
            notify_filtered_count();
        }
    );
    connect(
        &filtered_model_,
        &QAbstractItemModel::rowsRemoved,
        this,
        [notify_filtered_count](const QModelIndex&, const int, const int) {
            notify_filtered_count();
        }
    );
    query_coordinator_.setDecisionReconciler([this](BackendReviewDecisionState state) {
        decision_coordinator_.reconcile(std::move(state));
    });
    connect(
        &photo_inspection_coordinator_,
        &ReviewPhotoInspectionCoordinator::stateChanged,
        this,
        &ReviewController::photoInspectionChanged
    );
    connect(
        &focus_detail_coordinator_,
        &ReviewFocusDetailCoordinator::stateChanged,
        this,
        &ReviewController::focusDetailChanged
    );
    connect(
        &metadata_coordinator_,
        &ReviewLibraryMetadataCoordinator::stateChanged,
        this,
        &ReviewController::libraryMetadataChanged
    );
    connect(
        &metadata_coordinator_,
        &ReviewLibraryMetadataCoordinator::libraryChanged,
        this,
        [this](const QString& photo_id) {
            if (!photo_id.isEmpty()) {
                photo_inspection_coordinator_.retry();
            }
            requestLibraryReset();
            refreshLibraryFacets();
            refreshTravelCollections();
        }
    );
    connect(
        &comparison_coordinator_,
        &ReviewComparisonCoordinator::stateChanged,
        this,
        &ReviewController::comparisonStateChanged
    );
    connect(
        &comparison_coordinator_,
        &ReviewComparisonCoordinator::statusTextChanged,
        this,
        &ReviewController::comparisonStatusTextChanged
    );
    connect(
        &comparison_coordinator_,
        &ReviewComparisonCoordinator::recorded,
        this,
        &ReviewController::comparisonRecorded
    );
    connect(
        &comparison_coordinator_,
        &ReviewComparisonCoordinator::forgotten,
        this,
        &ReviewController::comparisonForgotten
    );
    connect(&decision_coordinator_, &ReviewDecisionCoordinator::stateChanged, this, [this]() {
        query_coordinator_.setDecisionBusy(decision_coordinator_.busy());
        emit decisionStateChanged();
    });
    connect(&decision_coordinator_, &ReviewDecisionCoordinator::statusTextChanged, this, [this]() {
        setDecisionStatusMessage(decision_coordinator_.statusMessage());
    });
    connect(
        &decision_coordinator_,
        &ReviewDecisionCoordinator::undone,
        this,
        &ReviewController::decisionUndone
    );
    connect(
        &facet_coordinator_,
        &ReviewLibraryFacetCoordinator::facetsChanged,
        this,
        &ReviewController::libraryFacetsChanged
    );
    connect(
        &travel_collection_coordinator_,
        &ReviewTravelCollectionCoordinator::projectionChanged,
        this,
        &ReviewController::travelCollectionsChanged
    );
    connect(
        &facet_coordinator_,
        &ReviewLibraryFacetCoordinator::globalStatusMessageChanged,
        this,
        [this]() { setStatusMessage(facet_coordinator_.globalStatusMessage()); }
    );
    connect(
        &place_resolution_coordinator_,
        &ReviewLibraryPlaceResolutionCoordinator::stateChanged,
        this,
        &ReviewController::libraryPlaceResolutionChanged
    );
    connect(
        &place_resolution_coordinator_,
        &ReviewLibraryPlaceResolutionCoordinator::placesChanged,
        this,
        [this]() {
            photo_inspection_coordinator_.retry();
            refreshLibraryFacets();
            refreshTravelCollections();
            if (!filtered_model_.countryKey().isEmpty()
                || !filtered_model_.localityKey().isEmpty()) {
                scheduleFilterQuery();
            }
        }
    );
    const auto synchronize_place_provider = [this]() {
        place_resolution_coordinator_.synchronizeProviderAvailability();
        emit libraryPlaceResolutionChanged();
    };
    connect(
        map_provider_preferences_,
        &MapProviderPreferences::googleApiKeyStoredChanged,
        this,
        synchronize_place_provider
    );
    connect(
        map_provider_preferences_,
        &MapProviderPreferences::googleReverseGeocodingAllowedChanged,
        this,
        synchronize_place_provider
    );
    connect(
        map_provider_preferences_,
        &MapProviderPreferences::amapWebServiceKeyStoredChanged,
        this,
        synchronize_place_provider
    );
    connect(
        map_provider_preferences_,
        &MapProviderPreferences::amapReverseGeocodingAllowedChanged,
        this,
        synchronize_place_provider
    );
    connect(
        &keyword_coordinator_,
        &ReviewLibraryKeywordCoordinator::keywordsChanged,
        this,
        &ReviewController::libraryKeywordsChanged
    );
    connect(
        &keyword_coordinator_,
        &ReviewLibraryKeywordCoordinator::photoKeywordsChanged,
        this,
        &ReviewController::libraryKeywordsChanged
    );
    connect(
        &keyword_coordinator_,
        &ReviewLibraryKeywordCoordinator::statusMessageChanged,
        this,
        [this]() { setStatusMessage(keyword_coordinator_.statusMessage()); }
    );
    connect(
        &keyword_coordinator_,
        &ReviewLibraryKeywordCoordinator::keywordMutationAccepted,
        this,
        [this]() {
            requestLibraryReset();
            refreshLibraryFacets();
        }
    );
    connect(
        &map_coordinator_,
        &ReviewLibraryMapCoordinator::stateChanged,
        this,
        &ReviewController::libraryMapChanged
    );
    connect(
        &organization_coordinator_,
        &ReviewLibraryOrganizationCoordinator::stateProjected,
        this,
        [this](const QString& photo_id, const bool liked, const QString& color_label) {
            emit colorLabelChanged(photo_id, color_label);
            emit likedChanged(photo_id, liked);
            if (filtered_model_.hasActiveServerFilter()) {
                scheduleFilterQuery();
            } else {
                refreshLibraryFacets();
            }
        }
    );
    connect(
        &model_,
        &ReviewModel::localSourceAvailabilityChanged,
        this,
        &ReviewController::sourceAvailabilityChanged
    );
    connect(
        &organization_coordinator_,
        &ReviewLibraryOrganizationCoordinator::statusMessageChanged,
        this,
        [this]() { setDecisionStatusMessage(organization_coordinator_.statusMessage()); }
    );
    connect(
        &album_coordinator_,
        &ReviewLibraryAlbumCoordinator::albumsChanged,
        this,
        &ReviewController::libraryAlbumsChanged
    );
    connect(
        &album_coordinator_,
        &ReviewLibraryAlbumCoordinator::albumSelectionChanged,
        this,
        [this]() {
            refreshRemoteLibraryPresentation();
            emit libraryAlbumChanged();
            emit itemCountChanged();
        }
    );
    connect(
        &album_coordinator_,
        &ReviewLibraryAlbumCoordinator::queryChanged,
        this,
        &ReviewController::scheduleFilterQuery
    );
    connect(
        &album_coordinator_,
        &ReviewLibraryAlbumCoordinator::statusMessageChanged,
        this,
        [this]() {
            const auto channel = album_coordinator_.statusChannel();
            if (channel == ReviewLibraryAlbumCoordinator::StatusChannel::Decision) {
                setDecisionStatusMessage(album_coordinator_.statusMessage());
            } else if (channel == ReviewLibraryAlbumCoordinator::StatusChannel::Global) {
                setStatusMessage(album_coordinator_.statusMessage());
            }
        }
    );
    connect(
        &source_health_coordinator_,
        &ReviewSourceHealthCoordinator::sourceHealthChanged,
        this,
        &ReviewController::librarySourceHealthChanged
    );
    connect(
        &source_health_coordinator_,
        &ReviewSourceHealthCoordinator::libraryFolderScanRequested,
        this,
        [this](const QString& folder_path) { scanFolder(QUrl::fromLocalFile(folder_path)); }
    );
    connect(
        &source_health_coordinator_,
        &ReviewSourceHealthCoordinator::libraryVisibilityChanged,
        this,
        [this]() {
            requestLibraryReset();
            refreshLibraryFacets();
        }
    );
    connect(
        &source_health_coordinator_,
        &ReviewSourceHealthCoordinator::missingLocationReviewChanged,
        this,
        &ReviewController::missingSourceLocationReviewChanged
    );
    connect(
        &source_health_coordinator_,
        &ReviewSourceHealthCoordinator::globalStatusMessageChanged,
        this,
        [this]() { setStatusMessage(source_health_coordinator_.globalStatusMessage()); }
    );
    connect(&import_coordinator_, &ReviewImportCoordinator::runningChanged, this, [this]() {
        query_coordinator_.setScanRunning(import_coordinator_.scanning());
        emit scanningChanged();
    });
    connect(
        &import_coordinator_,
        &ReviewImportCoordinator::folderPathChanged,
        this,
        &ReviewController::folderPathChanged
    );
    connect(&import_coordinator_, &ReviewImportCoordinator::progressChanged, this, [this]() {
        emit scanProgressChanged();
        if (import_coordinator_
                .takeStreamRefreshRequest(model_.rowCount(), query_coordinator_.pageRunning())) {
            static_cast<void>(query_coordinator_.refreshStreamingPrefix(true));
        }
    });
    connect(&import_coordinator_, &ReviewImportCoordinator::statusMessageChanged, this, [this]() {
        setStatusMessage(import_coordinator_.statusMessage());
    });
    connect(
        &import_coordinator_,
        &ReviewImportCoordinator::terminalRefreshRequested,
        this,
        [this]() {
            refreshLibrarySourceHealth();
            requestLibraryReset();
        }
    );
    connect(&query_coordinator_, &ReviewLibraryQueryCoordinator::workStateChanged, this, [this]() {
        emit busyChanged();
        emit refreshingChanged();
        emit loadingMoreChanged();
    });
    connect(
        &query_coordinator_,
        &ReviewLibraryQueryCoordinator::hasMoreChanged,
        this,
        &ReviewController::hasMoreChanged
    );
    connect(&query_coordinator_, &ReviewLibraryQueryCoordinator::itemCountChanged, this, [this]() {
        emit itemCountChanged();
        emit filtersChanged();
    });
    connect(
        &query_coordinator_,
        &ReviewLibraryQueryCoordinator::decisionsReconciled,
        this,
        [this]() {
            refreshRemoteLibraryPresentation();
            emit decisionStateChanged();
        }
    );
    connect(
        &remote_library_coordinator_,
        &ReviewRemoteLibraryCoordinator::stateChanged,
        this,
        [this]() {
            refreshRemoteLibraryPresentation();
            emit remoteLibraryChanged();
            emit itemCountChanged();
        }
    );
    connect(
        &remote_library_coordinator_,
        &ReviewRemoteLibraryCoordinator::connectionChanged,
        this,
        &ReviewController::remoteLibraryChanged
    );
    connect(
        &remote_library_coordinator_,
        &ReviewRemoteLibraryCoordinator::remotePhotoReady,
        this,
        &ReviewController::remotePhotoReady
    );
    connect(
        &remote_library_coordinator_,
        &ReviewRemoteLibraryCoordinator::localLibraryRefreshRequested,
        this,
        &ReviewController::refreshVisibleLibrary
    );
    connect(
        &query_coordinator_,
        &ReviewLibraryQueryCoordinator::initialPagePresented,
        this,
        [this,
         startup_dependencies_loaded =
             false](const BackendLibraryPhotoFilter& filter, const quint64 generation) mutable {
            facet_coordinator_.refresh(filter, generation);
            if (!import_coordinator_.scanning()) {
                place_resolution_coordinator_.synchronizeProviderAvailability();
            }
            if (startup_dependencies_loaded) {
                return;
            }
            startup_dependencies_loaded = true;
            travel_collection_coordinator_.refresh(travel_living_place_rules_, generation);
            refreshSharedGradeNodes();
            refreshLibraryAlbums();
            refreshLibraryKeywords();
            refreshLibrarySourceHealth();
        }
    );
    connect(
        &query_coordinator_,
        &ReviewLibraryQueryCoordinator::resetPresentationStarted,
        this,
        [this]() {
            setStatusMessage(
                import_coordinator_.scanning() ? import_coordinator_.statusMessage()
                                               : import_coordinator_.refreshingStatusMessage()
            );
        }
    );
    connect(
        &query_coordinator_,
        &ReviewLibraryQueryCoordinator::statusMessageChanged,
        this,
        [this]() { setStatusMessage(query_coordinator_.statusMessage()); }
    );
    connect(
        &query_coordinator_,
        &ReviewLibraryQueryCoordinator::readyStatusRequested,
        this,
        [this]() {
            if (import_coordinator_.scanning()) {
                setStatusMessage(import_coordinator_.statusMessage());
            } else {
                updateReadyStatus();
            }
        }
    );
    connect(
        &shared_grade_coordinator_,
        &ReviewSharedGradeCoordinator::nodesChanged,
        this,
        &ReviewController::sharedGradeNodesChanged
    );
    connect(
        &shared_grade_coordinator_,
        &ReviewSharedGradeCoordinator::statusMessageChanged,
        this,
        [this]() { setStatusMessage(shared_grade_coordinator_.statusMessage()); }
    );
    connect(
        &shared_grade_coordinator_,
        &ReviewSharedGradeCoordinator::libraryRefreshRequested,
        this,
        &ReviewController::refreshVisibleLibrary
    );
    if (auto* const application = QCoreApplication::instance()) {
        application->installEventFilter(this);
    }
    remote_library_coordinator_.start();
    QTimer::singleShot(0, this, [this]() { requestLibraryReset(); });
}
