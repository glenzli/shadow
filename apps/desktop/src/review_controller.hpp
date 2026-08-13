#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"
#include "review_comparison_coordinator.hpp"
#include "review_decision_coordinator.hpp"
#include "review_filter_model.hpp"
#include "review_focus_detail_coordinator.hpp"
#include "review_import_coordinator.hpp"
#include "review_library_album_coordinator.hpp"
#include "review_library_facet_coordinator.hpp"
#include "review_library_keyword_coordinator.hpp"
#include "review_library_map_coordinator.hpp"
#include "review_library_metadata_coordinator.hpp"
#include "review_location_completion_coordinator.hpp"
#include "review_location_reference_coordinator.hpp"
#include "review_library_organization_coordinator.hpp"
#include "review_library_place_resolution_coordinator.hpp"
#include "review_library_query_coordinator.hpp"
#include "review_model.hpp"
#include "review_photo_inspection_coordinator.hpp"
#include "review_remote_library_coordinator.hpp"
#include "review_shared_grade_coordinator.hpp"
#include "review_source_availability_monitor.hpp"
#include "review_source_health_coordinator.hpp"
#include "review_travel_collection_coordinator.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <memory>

class MapProviderPreferences;

class ReviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY refreshingChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY loadingMoreChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY hasMoreChanged)
    Q_PROPERTY(QString folderPath READ folderPath NOTIFY folderPathChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QVariantMap scanProgress READ scanProgress NOTIFY scanProgressChanged)
    Q_PROPERTY(int itemCount READ itemCount NOTIFY itemCountChanged)
    Q_PROPERTY(QVariantMap photoInspection READ photoInspection NOTIFY photoInspectionChanged)
    Q_PROPERTY(bool photoInspectionBusy READ photoInspectionBusy NOTIFY photoInspectionChanged)
    Q_PROPERTY(bool photoInspectionFailed READ photoInspectionFailed NOTIFY photoInspectionChanged)
    Q_PROPERTY(QString focusDetailImageSource READ focusDetailImageSource NOTIFY focusDetailChanged)
    Q_PROPERTY(QString focusDetailStatusText READ focusDetailStatusText NOTIFY focusDetailChanged)
    Q_PROPERTY(bool focusDetailBusy READ focusDetailBusy NOTIFY focusDetailChanged)
    Q_PROPERTY(bool focusDetailReady READ focusDetailReady NOTIFY focusDetailChanged)
    Q_PROPERTY(bool focusDetailFailed READ focusDetailFailed NOTIFY focusDetailChanged)
    Q_PROPERTY(bool comparisonBusy READ comparisonBusy NOTIFY comparisonStateChanged)
    Q_PROPERTY(bool canUndoComparison READ canUndoComparison NOTIFY comparisonStateChanged)
    Q_PROPERTY(int sessionEvidenceCount READ sessionEvidenceCount NOTIFY comparisonStateChanged)
    Q_PROPERTY(
        QString comparisonStatusText READ comparisonStatusText NOTIFY comparisonStatusTextChanged
    )
    Q_PROPERTY(bool decisionBusy READ decisionBusy NOTIFY decisionStateChanged)
    Q_PROPERTY(bool canUndoDecision READ canUndoDecision NOTIFY decisionStateChanged)
    Q_PROPERTY(QString decisionStatusText READ decisionStatusText NOTIFY decisionStatusTextChanged)
    Q_PROPERTY(QString filterFlag READ filterFlag WRITE setFilterFlag NOTIFY filtersChanged)
    Q_PROPERTY(
        int filterMinimumRating READ filterMinimumRating WRITE setFilterMinimumRating NOTIFY
            filtersChanged
    )
    Q_PROPERTY(
        QString filterColorLabel READ filterColorLabel WRITE setFilterColorLabel NOTIFY
            filtersChanged
    )
    Q_PROPERTY(
        QString filterEditState READ filterEditState WRITE setFilterEditState NOTIFY filtersChanged
    )
    Q_PROPERTY(QString filterLiked READ filterLiked WRITE setFilterLiked NOTIFY filtersChanged)
    Q_PROPERTY(
        QString librarySortKey READ librarySortKey WRITE setLibrarySortKey NOTIFY
            libraryOrderChanged
    )
    Q_PROPERTY(
        bool librarySortDescending READ librarySortDescending WRITE setLibrarySortDescending NOTIFY
            libraryOrderChanged
    )
    Q_PROPERTY(
        QString filterExcludedFlag READ filterExcludedFlag WRITE setFilterExcludedFlag NOTIFY
            filtersChanged
    )
    Q_PROPERTY(
        QString filterExcludedColorLabel READ filterExcludedColorLabel WRITE
            setFilterExcludedColorLabel NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString filterCaptureMonth READ filterCaptureMonth WRITE setFilterCaptureMonth NOTIFY
            filtersChanged
    )
    Q_PROPERTY(
        int filterChineseLunarMonth READ filterChineseLunarMonth WRITE setFilterChineseLunarMonth
            NOTIFY filtersChanged
    )
    Q_PROPERTY(
        int filterChineseLunarDay READ filterChineseLunarDay WRITE setFilterChineseLunarDay NOTIFY
            filtersChanged
    )
    Q_PROPERTY(
        QString filterChineseLunarMonthType READ filterChineseLunarMonthType WRITE
            setFilterChineseLunarMonthType NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString filterCameraKey READ filterCameraKey WRITE setFilterCameraKey NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString filterLensKey READ filterLensKey WRITE setFilterLensKey NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString filterCountryKey READ filterCountryKey WRITE setFilterCountryKey NOTIFY
            filtersChanged
    )
    Q_PROPERTY(
        QString filterLocalityKey READ filterLocalityKey WRITE setFilterLocalityKey NOTIFY
            filtersChanged
    )
    Q_PROPERTY(
        bool travelFilterEnabled READ travelFilterEnabled WRITE setTravelFilterEnabled NOTIFY
            filtersChanged
    )
    Q_PROPERTY(
        bool dailyFilterEnabled READ dailyFilterEnabled WRITE setDailyFilterEnabled NOTIFY
            filtersChanged
    )
    Q_PROPERTY(
        QStringList filterKeywordIdsAll READ filterKeywordIdsAll WRITE setFilterKeywordIdsAll NOTIFY
            filtersChanged
    )
    Q_PROPERTY(
        QStringList filterExcludedKeywordIdsAny READ filterExcludedKeywordIdsAny WRITE
            setFilterExcludedKeywordIdsAny NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QVariantList libraryCaptureMonthFacets READ libraryCaptureMonthFacets NOTIFY
            libraryFacetsChanged
    )
    Q_PROPERTY(
        QVariantList libraryCameraFacets READ libraryCameraFacets NOTIFY libraryFacetsChanged
    )
    Q_PROPERTY(QVariantList libraryLensFacets READ libraryLensFacets NOTIFY libraryFacetsChanged)
    Q_PROPERTY(
        QVariantList libraryCountryFacets READ libraryCountryFacets NOTIFY libraryFacetsChanged
    )
    Q_PROPERTY(QVariantList libraryCityFacets READ libraryCityFacets NOTIFY libraryFacetsChanged)
    Q_PROPERTY(bool libraryFacetsBusy READ libraryFacetsBusy NOTIFY libraryFacetsChanged)
    Q_PROPERTY(
        QVariantList livingPlaceCandidates READ livingPlaceCandidates NOTIFY
            travelCollectionsChanged
    )
    Q_PROPERTY(QVariantList travelGroups READ travelGroups NOTIFY travelCollectionsChanged)
    Q_PROPERTY(qulonglong travelPhotoCount READ travelPhotoCount NOTIFY travelCollectionsChanged)
    Q_PROPERTY(qulonglong dailyPhotoCount READ dailyPhotoCount NOTIFY travelCollectionsChanged)
    Q_PROPERTY(
        bool travelCollectionsBusy READ travelCollectionsBusy NOTIFY travelCollectionsChanged
    )
    Q_PROPERTY(
        QString travelCollectionsErrorText READ travelCollectionsErrorText NOTIFY
            travelCollectionsChanged
    )
    Q_PROPERTY(
        bool libraryPlaceResolutionRunning READ libraryPlaceResolutionRunning NOTIFY
            libraryPlaceResolutionChanged
    )
    Q_PROPERTY(
        QString libraryPlaceResolutionStatusCode READ libraryPlaceResolutionStatusCode NOTIFY
            libraryPlaceResolutionChanged
    )
    Q_PROPERTY(
        QString libraryPlaceResolutionErrorText READ libraryPlaceResolutionErrorText NOTIFY
            libraryPlaceResolutionChanged
    )
    Q_PROPERTY(
        qulonglong libraryPlaceResolutionProcessedCount READ libraryPlaceResolutionProcessedCount
            NOTIFY libraryPlaceResolutionChanged
    )
    Q_PROPERTY(
        qulonglong libraryPlaceResolutionRecordedCount READ libraryPlaceResolutionRecordedCount
            NOTIFY libraryPlaceResolutionChanged
    )
    Q_PROPERTY(
        qulonglong libraryPlaceResolutionFailedCount READ libraryPlaceResolutionFailedCount NOTIFY
            libraryPlaceResolutionChanged
    )
    Q_PROPERTY(QVariantList libraryKeywords READ libraryKeywords NOTIFY libraryKeywordsChanged)
    Q_PROPERTY(
        QVariantList libraryPhotoKeywords READ libraryPhotoKeywords NOTIFY libraryKeywordsChanged
    )
    Q_PROPERTY(bool libraryKeywordsBusy READ libraryKeywordsBusy NOTIFY libraryKeywordsChanged)
    Q_PROPERTY(QVariantList libraryMapClusters READ libraryMapClusters NOTIFY libraryMapChanged)
    Q_PROPERTY(qulonglong libraryMapPhotoCount READ libraryMapPhotoCount NOTIFY libraryMapChanged)
    Q_PROPERTY(bool libraryMapBusy READ libraryMapBusy NOTIFY libraryMapChanged)
    Q_PROPERTY(bool libraryMapFailed READ libraryMapFailed NOTIFY libraryMapChanged)
    Q_PROPERTY(
        QVariantList locationCompletionGroups READ locationCompletionGroups NOTIFY
            locationCompletionChanged
    )
    Q_PROPERTY(
        bool locationCompletionBusy READ locationCompletionBusy NOTIFY locationCompletionChanged
    )
    Q_PROPERTY(
        bool locationCompletionTruncated READ locationCompletionTruncated NOTIFY
            locationCompletionChanged
    )
    Q_PROPERTY(
        QString locationCompletionErrorText READ locationCompletionErrorText NOTIFY
            locationCompletionChanged
    )
    Q_PROPERTY(
        QVariantList locationReferenceLibraries READ locationReferenceLibraries NOTIFY
            locationReferenceChanged
    )
    Q_PROPERTY(
        bool locationReferenceBusy READ locationReferenceBusy NOTIFY locationReferenceChanged
    )
    Q_PROPERTY(
        QString locationReferenceErrorText READ locationReferenceErrorText NOTIFY
            locationReferenceChanged
    )
    Q_PROPERTY(
        QVariantMap librarySystemCollectionCounts READ librarySystemCollectionCounts NOTIFY
            libraryFacetsChanged
    )
    Q_PROPERTY(
        QString libraryAlbumId READ libraryAlbumId WRITE setLibraryAlbumId NOTIFY
            libraryAlbumChanged
    )
    Q_PROPERTY(QVariantList libraryAlbums READ libraryAlbums NOTIFY libraryAlbumsChanged)
    Q_PROPERTY(bool libraryAlbumsBusy READ libraryAlbumsBusy NOTIFY libraryAlbumsChanged)
    Q_PROPERTY(
        QVariantList librarySourceHealth READ librarySourceHealth NOTIFY librarySourceHealthChanged
    )
    Q_PROPERTY(
        bool librarySourceHealthBusy READ librarySourceHealthBusy NOTIFY librarySourceHealthChanged
    )
    Q_PROPERTY(
        bool librarySourceRemovalBusy READ librarySourceRemovalBusy NOTIFY
            librarySourceHealthChanged
    )
    Q_PROPERTY(
        bool librarySourceReconcileBusy READ librarySourceReconcileBusy NOTIFY
            librarySourceHealthChanged
    )
    Q_PROPERTY(QVariantMap libraryMetadata READ libraryMetadata NOTIFY libraryMetadataChanged)
    Q_PROPERTY(
        QVariantMap libraryCaptureTimePreview READ libraryCaptureTimePreview NOTIFY
            libraryMetadataChanged
    )
    Q_PROPERTY(
        QVariantMap libraryCoordinateBatchPreview READ libraryCoordinateBatchPreview NOTIFY
            libraryMetadataChanged
    )
    Q_PROPERTY(QVariantMap libraryGpxPreview READ libraryGpxPreview NOTIFY libraryMetadataChanged)
    Q_PROPERTY(
        QVariantMap libraryMetadataBatchReceipt READ libraryMetadataBatchReceipt NOTIFY
            libraryMetadataChanged
    )
    Q_PROPERTY(bool libraryMetadataBusy READ libraryMetadataBusy NOTIFY libraryMetadataChanged)
    Q_PROPERTY(
        QString libraryMetadataStatusCode READ libraryMetadataStatusCode NOTIFY
            libraryMetadataChanged
    )
    Q_PROPERTY(
        QString libraryMetadataErrorText READ libraryMetadataErrorText NOTIFY libraryMetadataChanged
    )
    Q_PROPERTY(
        QVariantList missingSourceLocations READ missingSourceLocations NOTIFY
            missingSourceLocationReviewChanged
    )
    Q_PROPERTY(
        QString missingSourceLocationScanId READ missingSourceLocationScanId NOTIFY
            missingSourceLocationReviewChanged
    )
    Q_PROPERTY(
        bool missingSourceLocationsBusy READ missingSourceLocationsBusy NOTIFY
            missingSourceLocationReviewChanged
    )
    Q_PROPERTY(
        bool missingSourceLocationsHasMore READ missingSourceLocationsHasMore NOTIFY
            missingSourceLocationReviewChanged
    )
    Q_PROPERTY(
        bool sourceRelinkBusy READ sourceRelinkBusy NOTIFY missingSourceLocationReviewChanged
    )
    Q_PROPERTY(
        QString sourceRelinkStatusText READ sourceRelinkStatusText NOTIFY
            missingSourceLocationReviewChanged
    )
    Q_PROPERTY(int filteredItemCount READ filteredItemCount NOTIFY filtersChanged)
    Q_PROPERTY(QVariantList sharedGradeNodes READ sharedGradeNodes NOTIFY sharedGradeNodesChanged)
    Q_PROPERTY(bool remoteLibraryBusy READ remoteLibraryBusy NOTIFY remoteLibraryChanged)
    Q_PROPERTY(QVariantList remoteLibraries READ remoteLibraries NOTIFY remoteLibraryChanged)
    Q_PROPERTY(bool remoteLibrarySyncing READ remoteLibrarySyncing NOTIFY remoteLibraryChanged)
    Q_PROPERTY(
        bool remoteLibraryMaterializing READ remoteLibraryMaterializing NOTIFY remoteLibraryChanged
    )
    Q_PROPERTY(
        bool remoteLibrarySecureStorageAvailable READ remoteLibrarySecureStorageAvailable NOTIFY
            remoteLibraryChanged
    )
    Q_PROPERTY(
        bool remoteLibraryTokenStored READ remoteLibraryTokenStored NOTIFY remoteLibraryChanged
    )
    Q_PROPERTY(
        QString remoteLibraryServerAddress READ remoteLibraryServerAddress NOTIFY
            remoteLibraryChanged
    )
    Q_PROPERTY(bool remoteLibraryConnected READ remoteLibraryConnected NOTIFY remoteLibraryChanged)
    Q_PROPERTY(
        QString remoteLibraryServerName READ remoteLibraryServerName NOTIFY remoteLibraryChanged
    )
    Q_PROPERTY(int remoteLibraryPhotoCount READ remoteLibraryPhotoCount NOTIFY remoteLibraryChanged)
    Q_PROPERTY(
        QString remoteLibraryStatusCode READ remoteLibraryStatusCode NOTIFY remoteLibraryChanged
    )
    Q_PROPERTY(
        QString remoteLibraryDiagnosticText READ remoteLibraryDiagnosticText NOTIFY
            remoteLibraryChanged
    )
    Q_PROPERTY(
        QString remoteLibraryMaterializingPhotoId READ remoteLibraryMaterializingPhotoId NOTIFY
            remoteLibraryChanged
    )
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT)

  public:
    explicit ReviewController(
        std::shared_ptr<DesktopBackend> backend,
        MapProviderPreferences* map_provider_preferences,
        const QString& isolated_settings_file = {},
        std::unique_ptr<SecretStore> remote_library_secret_store = {},
        QObject* parent = nullptr
    );
    ~ReviewController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool scanning() const noexcept;
    [[nodiscard]] bool refreshing() const noexcept;
    [[nodiscard]] bool loadingMore() const noexcept;
    [[nodiscard]] bool hasMore() const noexcept;
    [[nodiscard]] QString folderPath() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QVariantMap scanProgress() const;
    [[nodiscard]] int itemCount() const;
    [[nodiscard]] QVariantMap photoInspection() const;
    [[nodiscard]] bool photoInspectionBusy() const noexcept;
    [[nodiscard]] bool photoInspectionFailed() const noexcept;
    [[nodiscard]] QString focusDetailImageSource() const;
    [[nodiscard]] QString focusDetailStatusText() const;
    [[nodiscard]] bool focusDetailBusy() const noexcept;
    [[nodiscard]] bool focusDetailReady() const noexcept;
    [[nodiscard]] bool focusDetailFailed() const noexcept;
    [[nodiscard]] bool comparisonBusy() const noexcept;
    [[nodiscard]] bool canUndoComparison() const noexcept;
    [[nodiscard]] int sessionEvidenceCount() const noexcept;
    [[nodiscard]] QString comparisonStatusText() const;
    [[nodiscard]] bool decisionBusy() const noexcept;
    [[nodiscard]] bool canUndoDecision() const;
    [[nodiscard]] QString decisionStatusText() const;
    [[nodiscard]] QString filterFlag() const;
    [[nodiscard]] int filterMinimumRating() const noexcept;
    [[nodiscard]] QString filterColorLabel() const;
    [[nodiscard]] QString filterEditState() const;
    [[nodiscard]] QString filterLiked() const;
    [[nodiscard]] QString librarySortKey() const;
    [[nodiscard]] bool librarySortDescending() const noexcept;
    [[nodiscard]] QString filterExcludedFlag() const;
    [[nodiscard]] QString filterExcludedColorLabel() const;
    [[nodiscard]] QString filterCaptureMonth() const;
    [[nodiscard]] int filterChineseLunarMonth() const noexcept;
    [[nodiscard]] int filterChineseLunarDay() const noexcept;
    [[nodiscard]] QString filterChineseLunarMonthType() const;
    [[nodiscard]] QString filterCameraKey() const;
    [[nodiscard]] QString filterLensKey() const;
    [[nodiscard]] QString filterCountryKey() const;
    [[nodiscard]] QString filterLocalityKey() const;
    [[nodiscard]] bool travelFilterEnabled() const noexcept;
    [[nodiscard]] bool dailyFilterEnabled() const noexcept;
    [[nodiscard]] QStringList filterKeywordIdsAll() const;
    [[nodiscard]] QStringList filterExcludedKeywordIdsAny() const;
    [[nodiscard]] QVariantList libraryCaptureMonthFacets() const;
    [[nodiscard]] QVariantList libraryCameraFacets() const;
    [[nodiscard]] QVariantList libraryLensFacets() const;
    [[nodiscard]] QVariantList libraryCountryFacets() const;
    [[nodiscard]] QVariantList libraryCityFacets() const;
    [[nodiscard]] bool libraryFacetsBusy() const noexcept;
    [[nodiscard]] QVariantList livingPlaceCandidates() const;
    [[nodiscard]] QVariantList travelGroups() const;
    [[nodiscard]] qulonglong travelPhotoCount() const noexcept;
    [[nodiscard]] qulonglong dailyPhotoCount() const noexcept;
    [[nodiscard]] bool travelCollectionsBusy() const noexcept;
    [[nodiscard]] QString travelCollectionsErrorText() const;
    [[nodiscard]] bool libraryPlaceResolutionRunning() const noexcept;
    [[nodiscard]] QString libraryPlaceResolutionStatusCode() const;
    [[nodiscard]] QString libraryPlaceResolutionErrorText() const;
    [[nodiscard]] qulonglong libraryPlaceResolutionProcessedCount() const noexcept;
    [[nodiscard]] qulonglong libraryPlaceResolutionRecordedCount() const noexcept;
    [[nodiscard]] qulonglong libraryPlaceResolutionFailedCount() const noexcept;
    [[nodiscard]] QVariantList libraryKeywords() const;
    [[nodiscard]] QVariantList libraryPhotoKeywords() const;
    [[nodiscard]] bool libraryKeywordsBusy() const noexcept;
    [[nodiscard]] QVariantMap librarySystemCollectionCounts() const;
    [[nodiscard]] QVariantList libraryMapClusters() const;
    [[nodiscard]] qulonglong libraryMapPhotoCount() const noexcept;
    [[nodiscard]] bool libraryMapBusy() const noexcept;
    [[nodiscard]] bool libraryMapFailed() const noexcept;
    [[nodiscard]] QVariantList locationCompletionGroups() const;
    [[nodiscard]] bool locationCompletionBusy() const noexcept;
    [[nodiscard]] bool locationCompletionTruncated() const noexcept;
    [[nodiscard]] QString locationCompletionErrorText() const;
    [[nodiscard]] QVariantList locationReferenceLibraries() const;
    [[nodiscard]] bool locationReferenceBusy() const noexcept;
    [[nodiscard]] QString locationReferenceErrorText() const;
    [[nodiscard]] QString libraryAlbumId() const;
    [[nodiscard]] QVariantList libraryAlbums() const;
    [[nodiscard]] bool libraryAlbumsBusy() const noexcept;
    [[nodiscard]] QVariantList librarySourceHealth() const;
    [[nodiscard]] bool librarySourceHealthBusy() const noexcept;
    [[nodiscard]] bool librarySourceRemovalBusy() const noexcept;
    [[nodiscard]] bool librarySourceReconcileBusy() const noexcept;
    [[nodiscard]] QVariantMap libraryMetadata() const;
    [[nodiscard]] QVariantMap libraryCaptureTimePreview() const;
    [[nodiscard]] QVariantMap libraryCoordinateBatchPreview() const;
    [[nodiscard]] QVariantMap libraryGpxPreview() const;
    [[nodiscard]] QVariantMap libraryMetadataBatchReceipt() const;
    [[nodiscard]] bool libraryMetadataBusy() const noexcept;
    [[nodiscard]] QString libraryMetadataStatusCode() const;
    [[nodiscard]] QString libraryMetadataErrorText() const;
    [[nodiscard]] QVariantList missingSourceLocations() const;
    [[nodiscard]] QString missingSourceLocationScanId() const;
    [[nodiscard]] bool missingSourceLocationsBusy() const noexcept;
    [[nodiscard]] bool missingSourceLocationsHasMore() const noexcept;
    [[nodiscard]] bool sourceRelinkBusy() const noexcept;
    [[nodiscard]] QString sourceRelinkStatusText() const;
    [[nodiscard]] int filteredItemCount() const noexcept;
    [[nodiscard]] QVariantList sharedGradeNodes() const;
    [[nodiscard]] bool remoteLibraryBusy() const noexcept;
    [[nodiscard]] QVariantList remoteLibraries() const;
    [[nodiscard]] bool remoteLibrarySyncing() const noexcept;
    [[nodiscard]] bool remoteLibraryMaterializing() const noexcept;
    [[nodiscard]] bool remoteLibrarySecureStorageAvailable() const noexcept;
    [[nodiscard]] bool remoteLibraryTokenStored() const noexcept;
    [[nodiscard]] QString remoteLibraryServerAddress() const;
    [[nodiscard]] bool remoteLibraryConnected() const noexcept;
    [[nodiscard]] QString remoteLibraryServerName() const;
    [[nodiscard]] int remoteLibraryPhotoCount() const noexcept;
    [[nodiscard]] QString remoteLibraryStatusCode() const;
    [[nodiscard]] QString remoteLibraryDiagnosticText() const;
    [[nodiscard]] QString remoteLibraryMaterializingPhotoId() const;
    [[nodiscard]] QAbstractItemModel* model() noexcept;
    [[nodiscard]] ReviewModel* reviewModel() noexcept;
    [[nodiscard]] std::shared_ptr<ReviewFocusDetailStore> focusDetailStore() const noexcept;

    void setFilterFlag(const QString& filter);
    void setFilterMinimumRating(int rating);
    void setFilterColorLabel(const QString& color_label);
    void setFilterEditState(const QString& edit_state);
    void setFilterLiked(const QString& liked);
    void setLibrarySortKey(const QString& sort_key);
    void setLibrarySortDescending(bool descending);
    void setFilterExcludedFlag(const QString& flag);
    void setFilterExcludedColorLabel(const QString& color_label);
    void setFilterCaptureMonth(const QString& capture_month);
    void setFilterChineseLunarMonth(int month);
    void setFilterChineseLunarDay(int day);
    void setFilterChineseLunarMonthType(const QString& month_type);
    void setFilterCameraKey(const QString& camera_key);
    void setFilterLensKey(const QString& lens_key);
    void setFilterCountryKey(const QString& country_key);
    void setFilterLocalityKey(const QString& locality_key);
    void setTravelFilterEnabled(bool enabled);
    void setDailyFilterEnabled(bool enabled);
    void setTravelLivingPlaces(const QVariantList& living_places);
    void setFilterKeywordIdsAll(const QStringList& keyword_ids);
    void setFilterExcludedKeywordIdsAny(const QStringList& keyword_ids);
    void setSemanticRepresentationOrder(const QStringList& ranked_keys);
    void setSmartCategoryRepresentationKeys(const QStringList& member_keys);
    void setLibraryAlbumId(const QString& album_id);

    Q_INVOKABLE void scanFolder(const QUrl& folder_url);
    Q_INVOKABLE void cancelScan();
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void
    requestPhotoInspection(const QString& photo_id, const QString& representation_id);
    Q_INVOKABLE void retryPhotoInspection();
    Q_INVOKABLE void clearPhotoInspection();
    Q_INVOKABLE void requestFocusDetail(
        const QString& photo_id,
        const QString& source_path,
        double center_x,
        double center_y
    );
    Q_INVOKABLE void clearFocusDetail();
    /// Returns the inclusive, currently filtered Library range between two
    /// presentation identities. This keeps Shift selection stable even when a
    /// justified grid has virtualized most of its delegates.
    Q_INVOKABLE QVariantList selectionRangeTargets(
        const QString& anchor_photo_id,
        const QString& anchor_representation_id,
        const QString& photo_id,
        const QString& representation_id
    ) const;
    Q_INVOKABLE QVariantMap
    prepareComparison(const QString& left_visual_handle, const QString& right_visual_handle);
    Q_INVOKABLE bool confirmComparisonReady(
        const QString& presentation_id,
        const QString& left_request_ticket,
        const QString& right_request_ticket
    );
    Q_INVOKABLE void cancelComparison(const QString& presentation_id);
    Q_INVOKABLE void recordComparison(const QString& presentation_id, int outcome);
    Q_INVOKABLE void undoLastComparison();
    Q_INVOKABLE void setPhotoFlag(const QString& photo_id, const QString& flag);
    Q_INVOKABLE void setPhotoRating(const QString& photo_id, int rating);
    Q_INVOKABLE void setPhotoColorLabel(const QString& photo_id, const QString& color_label);
    Q_INVOKABLE void setPhotoLiked(const QString& photo_id, bool liked);
    Q_INVOKABLE bool
    saveRemoteLibraryConnection(const QString& server_address, const QString& token);
    Q_INVOKABLE QString saveRemoteLibraryConnection(
        const QString& connection_id,
        const QString& server_address,
        const QString& token
    );
    Q_INVOKABLE bool removeRemoteLibraryConnection();
    Q_INVOKABLE bool removeRemoteLibraryConnection(const QString& connection_id);
    Q_INVOKABLE void syncRemoteLibrary();
    Q_INVOKABLE void syncRemoteLibrary(const QString& connection_id);
    Q_INVOKABLE void syncAllRemoteLibraries();
    Q_INVOKABLE void materializeRemotePhoto(const QString& photo_id);
    Q_INVOKABLE void clearFilters();
    Q_INVOKABLE void refreshVisibleLibrary();
    Q_INVOKABLE void refreshLibraryFacets();
    Q_INVOKABLE void refreshTravelCollections();
    Q_INVOKABLE void retryLibraryPlaceResolution();
    Q_INVOKABLE void refreshLibraryKeywords();
    Q_INVOKABLE void requestLibraryKeywordsForPhoto(const QString& photo_id);
    Q_INVOKABLE void createLibraryKeyword(const QString& parent_id, const QString& name);
    Q_INVOKABLE void renameLibraryKeyword(const QString& keyword_id, const QString& name);
    Q_INVOKABLE void moveLibraryKeyword(const QString& keyword_id, const QString& parent_id);
    Q_INVOKABLE void deleteLibraryKeyword(const QString& keyword_id);
    Q_INVOKABLE void assignLibraryKeyword(const QString& keyword_id, const QVariantList& targets);
    Q_INVOKABLE void removeLibraryKeyword(const QString& keyword_id, const QVariantList& targets);
    Q_INVOKABLE void setLibraryFacet(const QString& kind, const QString& key);
    Q_INVOKABLE void clearLibraryFacet(const QString& kind);
    Q_INVOKABLE void requestLibraryMapViewport(
        double south_latitude,
        double west_longitude,
        double north_latitude,
        double east_longitude,
        int columns,
        int rows
    );
    Q_INVOKABLE void requestLocationCompletion(
        qlonglong capture_start_unix_seconds,
        qlonglong capture_end_unix_seconds
    );
    Q_INVOKABLE QString locationCompletionVisualSource(const QString& visual_handle) const;
    Q_INVOKABLE void refreshLocationReferenceLibraries();
    Q_INVOKABLE void addLocationReferenceLibrary(
        const QUrl& root_url,
        qlonglong clock_offset_seconds = 0
    );
    Q_INVOKABLE void removeLocationReferenceLibrary(const QString& id);
    Q_INVOKABLE void refreshLibraryAlbums();
    Q_INVOKABLE void refreshLibrarySourceHealth();
    Q_INVOKABLE void verifyLibrarySource(const QString& source_path);
    Q_INVOKABLE bool confirmLocalSourceAvailable(
        const QString& photo_id,
        const QString& location_id,
        const QString& source_path
    );
    Q_INVOKABLE void removeLibrarySource(const QString& source_id, const QString& source_path);
    Q_INVOKABLE void recoverLibrarySource(const QString& source_id, const QUrl& candidate_url);
    Q_INVOKABLE void
    reconcileMissingSourcePhotos(const QString& scan_session_id, const QString& source_path);
    Q_INVOKABLE void requestLibraryMetadata(const QString& photo_id);
    Q_INVOKABLE void clearLibraryMetadata();
    Q_INVOKABLE void setLibraryCaptureTime(
        const QString& photo_id,
        const QString& mode,
        qlonglong captured_at_unix_seconds
    );
    Q_INVOKABLE void setLibraryCoordinates(
        const QString& photo_id,
        const QString& mode,
        double latitude_degrees,
        double longitude_degrees,
        const QString& place_name
    );
    Q_INVOKABLE void previewLibraryCaptureTimeBatch(
        const QVariantList& targets,
        const QString& mode,
        qlonglong offset_seconds
    );
    Q_INVOKABLE void applyLibraryCaptureTimeBatch(const QString& preview_id);
    Q_INVOKABLE void previewLibraryCoordinateBatch(
        const QVariantList& targets,
        const QString& mode,
        double latitude_degrees,
        double longitude_degrees,
        const QString& place_name,
        const QString& source_label
    );
    Q_INVOKABLE void applyLibraryCoordinateBatch(const QString& preview_id);
    Q_INVOKABLE void previewLibraryGpxImport(
        const QUrl& gpx_url,
        const QVariantList& targets,
        qlonglong camera_clock_offset_seconds,
        int maximum_gap_seconds
    );
    Q_INVOKABLE void applyLibraryGpxImport(const QString& preview_id);
    Q_INVOKABLE void openMissingSourceLocationReview(const QString& scan_session_id);
    Q_INVOKABLE void closeMissingSourceLocationReview();
    Q_INVOKABLE void loadMoreMissingSourceLocations();
    Q_INVOKABLE void
    relinkMissingSourceLocation(const QString& location_id, const QUrl& candidate_url);
    Q_INVOKABLE void
    relinkUnavailableSourceLocation(const QString& location_id, const QUrl& candidate_url);
    Q_INVOKABLE void
    removeUnavailablePhotoFromLibrary(const QString& photo_id, const QString& title);
    Q_INVOKABLE void createManualLibraryAlbum(const QString& name);
    Q_INVOKABLE void createSmartLibraryAlbum(const QString& name);
    Q_INVOKABLE void renameLibraryAlbum(const QString& album_id, const QString& name);
    Q_INVOKABLE void deleteLibraryAlbum(const QString& album_id);
    Q_INVOKABLE void
    addPhotosToManualLibraryAlbum(const QString& album_id, const QVariantList& targets);
    Q_INVOKABLE void
    removePhotosFromManualLibraryAlbum(const QString& album_id, const QVariantList& targets);
    Q_INVOKABLE void refreshSharedGradeNodes();
    Q_INVOKABLE QVariantMap
    applySharedGradeNode(const QString& layer_id, const QVariantList& targets);
    Q_INVOKABLE void undoLastDecision();
    Q_INVOKABLE void retranslateUi();

  signals:
    void busyChanged();
    void scanningChanged();
    void refreshingChanged();
    void scanProgressChanged();
    void loadingMoreChanged();
    void hasMoreChanged();
    void folderPathChanged();
    void statusTextChanged();
    void itemCountChanged();
    void photoInspectionChanged();
    void focusDetailChanged();
    void comparisonStateChanged();
    void comparisonStatusTextChanged();
    void comparisonRecorded();
    void comparisonForgotten();
    void decisionStateChanged();
    void decisionStatusTextChanged();
    void decisionChanged(
        const QString& photoId,
        qulonglong headSequence,
        const QString& flag,
        int rating
    );
    void colorLabelChanged(const QString& photoId, const QString& colorLabel);
    void likedChanged(const QString& photoId, bool liked);
    void sourceAvailabilityChanged(const QString& photoId, bool available);
    void filtersChanged();
    void allFiltersCleared();
    void libraryOrderChanged();
    void libraryAlbumChanged();
    void libraryAlbumsChanged();
    void libraryFacetsChanged();
    void travelCollectionsChanged();
    void libraryPlaceResolutionChanged();
    void libraryKeywordsChanged();
    void libraryMapChanged();
    void locationCompletionChanged();
    void locationReferenceChanged();
    void librarySourceHealthChanged();
    void libraryMetadataChanged();
    void missingSourceLocationReviewChanged();
    void sharedGradeNodesChanged();
    void remoteLibraryChanged();
    void remotePhotoReady(
        const QString& photoId,
        const QString& representationId,
        const QString& sourcePath,
        const QString& title
    );
    void decisionUndone();

  private:
    void initializeCoordinatorWiring();
    void requestLibraryReset();
    void scheduleFilterQuery();
    void refreshRemoteLibraryPresentation();
    [[nodiscard]] bool remoteLibraryPresentationEligible() const;
    [[nodiscard]] int visibleRemotePhotoCount() const;
    [[nodiscard]] BackendLibraryPhotoFilter currentLibraryFilter() const;
    [[nodiscard]] BackendLibraryPhotoOrder currentLibraryOrder() const noexcept;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void setStatusMessage(LocalizedUiMessage status);
    void updateReadyStatus();
    void setDecisionStatusMessage(LocalizedUiMessage status);
    void projectDecisionState(const BackendReviewDecisionState& state);

    std::shared_ptr<DesktopBackend> backend_;
    MapProviderPreferences* map_provider_preferences_ = nullptr;
    ReviewPhotoInspectionCoordinator photo_inspection_coordinator_;
    std::shared_ptr<ReviewFocusDetailStore> focus_detail_store_;
    ReviewFocusDetailCoordinator focus_detail_coordinator_;
    ReviewSourceHealthCoordinator source_health_coordinator_;
    ReviewLibraryAlbumCoordinator album_coordinator_;
    ReviewLibraryFacetCoordinator facet_coordinator_;
    ReviewTravelCollectionCoordinator travel_collection_coordinator_;
    QVector<BackendLibraryLivingPlaceRule> travel_living_place_rules_;
    ReviewLibraryPlaceResolutionCoordinator place_resolution_coordinator_;
    ReviewLibraryKeywordCoordinator keyword_coordinator_;
    ReviewLibraryMapCoordinator map_coordinator_;
    ReviewLocationCompletionCoordinator location_completion_coordinator_;
    ReviewLocationReferenceCoordinator location_reference_coordinator_;
    ReviewLibraryMetadataCoordinator metadata_coordinator_;
    ReviewImportCoordinator import_coordinator_;
    LocalizedUiMessage status_message_{
        "ReviewController",
        QT_TRANSLATE_NOOP("ReviewController", "Choose a folder to build your Review library"),
    };
    LocalizedUiMessage decision_status_message_{
        "ReviewController",
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Flags and stars are explicit local library decisions"
        ),
    };
    ReviewModel model_;
    ReviewSourceAvailabilityMonitor source_availability_monitor_;
    ReviewRemoteLibraryCoordinator remote_library_coordinator_;
    ReviewFilterModel filtered_model_;
    QString library_sort_key_ = QStringLiteral("capture_time");
    bool library_sort_descending_ = true;
    ReviewLibraryQueryCoordinator query_coordinator_;
    ReviewLibraryOrganizationCoordinator organization_coordinator_;
    ReviewSharedGradeCoordinator shared_grade_coordinator_;
    ReviewComparisonCoordinator comparison_coordinator_;
    ReviewDecisionCoordinator decision_coordinator_;
};
