#include "adjustment_transfer_controller.hpp"
#include "ai_preferences.hpp"
#include "amap_place_search_service.hpp"
#include "cache_maintenance_controller.hpp"
#include "cache_preferences.hpp"
#include "catalog_startup_recovery.hpp"
#include "composition_controller.hpp"
#include "desktop_backend.hpp"
#include "desktop_smoke/storage_profiles.hpp"
#include "desktop_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "edit_interchange_controller.hpp"
#include "edit_neighbor_preheater.hpp"
#include "edit_preview_presentation_context.hpp"
#include "edit_preview_presentation_registry.hpp"
#include "edit_preview_provider.hpp"
#include "edit_preview_texture_item.hpp"
#include "export_controller.hpp"
#include "geonames_library_reverse_geocoder.hpp"
#include "history_coordinator.hpp"
#include "image_understanding_controller.hpp"
#include "justified_review_layout_model.hpp"
#include "lut_export_controller.hpp"
#include "lut_library.hpp"
#include "lut_preview_provider.hpp"
#include "map/library_web_map_controller.hpp"
#include "map_provider_preferences.hpp"
#include "optics_profile_library.hpp"
#include "people_analysis_controller.hpp"
#include "people_scope_controller.hpp"
#include "personal_location_search.hpp"
#include "personal_profile.hpp"
#include "pipeline_launch.hpp"
#include "pipeline_run_controller.hpp"
#include "review_clustering_controller.hpp"
#include "review_controller.hpp"
#include "review_diagnostics.hpp"
#include "review_focus_detail_provider.hpp"
#include "review_gallery_grouping_controller.hpp"
#include "semantic_search_controller.hpp"
#include "shared_grade_link_controller.hpp"
#include "smart_category_controller.hpp"
#include "storage_profile.hpp"
#include "storage_profile_controller.hpp"
#include "storage_profile_registry.hpp"
#include "storage_profile_startup.hpp"
#include "storage_profile_switch.hpp"
#include "thumbnail_provider.hpp"
#include "ui_preferences.hpp"

#if defined(Q_OS_MACOS)
#include "platform/macos/titlebar.hpp"
#endif

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QTranslator>
#include <QUrl>
#include <QVariant>
#include <QWindow>
#include <QtWebView/QtWebView>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace {

[[nodiscard]] QString initialScanFolder() {
    QString initial_folder = qEnvironmentVariable("SHADOW_DESKTOP_SCAN_FOLDER");
#if defined(SHADOW_DESKTOP_DEV_SAMPLE_FOLDER)
    if (initial_folder.isEmpty()
        && qEnvironmentVariableIntValue("SHADOW_DESKTOP_AUTO_SCAN_SAMPLES") == 1) {
        const QDir development_samples(QString::fromUtf8(SHADOW_DESKTOP_DEV_SAMPLE_FOLDER));
        if (development_samples.exists()) {
            initial_folder = development_samples.absolutePath();
        }
    }
#endif
    return initial_folder;
}

[[nodiscard]] QString requestedSettingsSection(const QStringList& arguments) {
    constexpr auto option = "--open-settings";
    constexpr auto option_prefix = "--open-settings=";
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QString& argument = arguments[index];
        if (argument == QString::fromLatin1(option)) {
            return index + 1 < arguments.size() ? arguments[index + 1].trimmed() : QString{};
        }
        if (argument.startsWith(QString::fromLatin1(option_prefix))) {
            return argument.sliced(QString::fromLatin1(option_prefix).size()).trimmed();
        }
    }
    return {};
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QtWebView::initialize();
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Shadow"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("shadow.dev"));
    QCoreApplication::setApplicationName(QStringLiteral("Shadow"));
    QString pipeline_error;
    const auto pipeline_request =
        parsePipelineLaunch(QCoreApplication::arguments(), &pipeline_error);
    if (!pipeline_error.isEmpty()) {
        qCritical().noquote() << "Invalid pipeline launch:" << pipeline_error;
        return EXIT_FAILURE;
    }
    if (pipeline_request.has_value()) {
        return runPipelineEdit(application, *pipeline_request);
    }
    StorageProfileSwitchEndpoint switch_endpoint(QCoreApplication::arguments());
    if (!switch_endpoint.valid()) {
        qCritical().noquote() << switch_endpoint.error();
        return EXIT_FAILURE;
    }
    QObject::connect(
        &switch_endpoint,
        &StorageProfileSwitchEndpoint::abandoned,
        &application,
        [&application] { application.exit(EXIT_FAILURE); }
    );
    const QString initial_settings_section =
        requestedSettingsSection(QCoreApplication::arguments());

    // Native RAW providers (including a locally installed vendor SDK) execute
    // behind a separate helper process. Keep the helper beside the desktop
    // executable so development and packaged builds share the same boundary.
    const QString decode_helper_path = QDir(QCoreApplication::applicationDirPath())
                                           .filePath(QStringLiteral("shadow-image-decode-helper"));
    // A missing helper must degrade to a reported unsupported decode rather
    // than loading a third-party decoder inside the desktop process.
    qputenv("SHADOW_DISABLE_PRIVATE_DECODER", QByteArrayLiteral("1"));
    if (QFileInfo(decode_helper_path).isExecutable()) {
        qputenv("SHADOW_DECODE_HELPER_PATH", decode_helper_path.toUtf8());
    } else {
        qWarning().noquote() << "Isolated RAW decode helper is unavailable:" << decode_helper_path;
    }

    QString profile_error;
    StorageProfileLaunch profile_launch;
    profile_launch.arguments = QCoreApplication::arguments();
    profile_launch.daily_root =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    profile_launch.legacy_brush_file = qEnvironmentVariable("SHADOW_PAINT_BRUSH_SETTINGS_FILE");
    if (profile_launch.legacy_brush_file.isEmpty()) {
        profile_launch.legacy_brush_file =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                .filePath(QStringLiteral("paint-brushes.ini"));
    }
    profile_launch.override_root = qEnvironmentVariable("SHADOW_DESKTOP_DATA_ROOT");
    profile_launch.override_root_set = qEnvironmentVariableIsSet("SHADOW_DESKTOP_DATA_ROOT");
#ifndef NDEBUG
    profile_launch.debug_build = true;
#endif
    const bool headless_startup_smoke = qEnvironmentVariableIsSet("SHADOW_DESKTOP_SMOKE_TEST");
    const QString registry_root = qEnvironmentVariableIsSet("SHADOW_PROFILE_REGISTRY_ROOT")
                                      ? qEnvironmentVariable("SHADOW_PROFILE_REGISTRY_ROOT")
                                      : QDir(QFileInfo(profile_launch.daily_root).absolutePath())
                                            .filePath("library-profiles");
    StorageProfileRegistry profile_registry(profile_launch.daily_root, registry_root);
    const QString launch_context =
        StorageProfileRegistry::launchContext(profile_launch, &profile_error);
    if (launch_context.isEmpty()) {
        switch_endpoint.reportFailure(profile_error);
        qCritical().noquote() << profile_error;
        return EXIT_FAILURE;
    }
    // Recovery owns no Catalog or preference services, and never silently
    // recreates a missing remembered profile or falls back to Daily.
    QTranslator startup_translator;
    const auto locale = QLocale::system();
    if (locale.language() == QLocale::Chinese && locale.script() == QLocale::SimplifiedHanScript
        && startup_translator.load(":/i18n/shadow_zh_CN.qm"))
        application.installTranslator(&startup_translator);
    auto resolved_launch =
        profile_registry.resolveLaunch(profile_launch, launch_context, &profile_error);
    auto storage_profile =
        resolved_launch ? StorageProfile::open(*resolved_launch, &profile_error) : nullptr;
    while (!storage_profile && !switch_endpoint.requested() && !headless_startup_smoke) {
        const auto selected =
            chooseStorageProfileAtStartup(profile_registry, launch_context, profile_error);
        if (!selected)
            return EXIT_SUCCESS;
        profile_error.clear();
        profile_launch.arguments = {"Shadow"};
        profile_launch.arguments.append(profile_registry.argumentsFor(*selected));
        profile_launch.override_root_set = false;
        storage_profile = StorageProfile::open(profile_launch, &profile_error);
    }
    application.removeTranslator(&startup_translator);
    if (!storage_profile || !storage_profile->bindProcessEnvironment()) {
        switch_endpoint.reportFailure(profile_error);
        qCritical().noquote() << "Cannot open storage profile:" << profile_error;
        return EXIT_FAILURE;
    }
    const QString application_data = storage_profile->root();
    qInfo().noquote() << "Storage profile:" << storage_profile->kind() << storage_profile->id()
                      << application_data;
    if (!ReviewDiagnostics::instance().configureLogDirectory(application_data)) {
        qWarning() << "Review diagnostic log is unavailable; summaries remain on the console";
    }
    const QString catalog_path = storage_profile->catalogPath();
    const QString cache_root = storage_profile->cacheRoot();
    const QString isolated_settings_file = storage_profile->settingsFile();
    UiPreferences preferences(application, isolated_settings_file);
    AiPreferences ai_preferences(application_data, isolated_settings_file);
    CachePreferences cache_preferences(cache_root, isolated_settings_file);
    MapProviderPreferences map_provider_preferences(
        isolated_settings_file,
        headless_startup_smoke
            ? makeVolatileSecretStore()
            : makeLocalSecretStore(
                  QDir(application_data).filePath(QStringLiteral("map-provider-credentials.ini"))
              )
    );
    PersonalProfile personal_profile(application_data, isolated_settings_file);
    PersonalLocationSearch personal_location_search(defaultGeoNamesCityIndexPath());
    LibraryWebMapController library_web_map_controller(&map_provider_preferences);
    AmapPlaceSearchService amap_place_search_service(&map_provider_preferences);
    LutLibrary lut_library(
        isolated_settings_file,
        QDir(application_data).filePath(QStringLiteral("lut-store"))
    );
    OpticsProfileLibrary optics_profile_library(
        QDir(application_data).filePath(QStringLiteral("profiles/optics"))
    );

    QString infer_base_url = qEnvironmentVariable("SHADOW_INFER_BASE_URL");
    QString infer_credential_file = qEnvironmentVariable("SHADOW_INFER_CREDENTIAL_FILE");
    if (infer_credential_file.isEmpty()) {
        infer_credential_file =
            QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                .filePath(QStringLiteral("Shadow/credentials/infer-runtime-shadow.token"));
    }
    // Bind the read-only service identity before Rust constructs its consumers.
    // Request state, checkpoints and generated artifacts remain profile-local.
    qputenv("SHADOW_INFER_CREDENTIAL_FILE", infer_credential_file.toUtf8());

    std::shared_ptr<DesktopBackend> backend;
    while (!backend) {
        try {
            backend = std::make_shared<DesktopBackend>(catalog_path, cache_root);
        } catch (const std::exception& error) {
            qCritical() << "Cannot start Shadow's local backend:" << error.what();
            if (switch_endpoint.requested()) {
                switch_endpoint.reportFailure(QString::fromUtf8(error.what()));
                return EXIT_FAILURE;
            }
            const bool reset_for_smoke = headless_startup_smoke && !storage_profile->isDaily()
                                         && is_development_catalog_reset_error(error);
            if (!reset_for_smoke && headless_startup_smoke) {
                return EXIT_FAILURE;
            }
            if (!reset_for_smoke) {
                const auto action = offer_development_catalog_reset(error, catalog_path);
                if (action == QMessageBox::Retry) {
                    continue;
                }
                if (action != QMessageBox::Reset) {
                    return EXIT_FAILURE;
                }
            }

            QString reset_error;
            if (!reset_local_development_catalog(catalog_path, cache_root, &reset_error)) {
                if (headless_startup_smoke) {
                    qCritical() << "Catalog reset failed:" << reset_error;
                    return EXIT_FAILURE;
                }
                QMessageBox::critical(nullptr, QObject::tr("Catalog reset failed"), reset_error);
                return EXIT_FAILURE;
            }
        }
    }

    ReviewController controller(
        backend,
        &map_provider_preferences,
        isolated_settings_file,
        headless_startup_smoke
            ? makeVolatileSecretStore()
            : makeLocalSecretStore(
                  QDir(application_data).filePath(QStringLiteral("remote-library-credentials.ini"))
              )
    );
    controller.setTravelLivingPlaces(personal_profile.livingPlaces());
    QObject::connect(
        &personal_profile,
        &PersonalProfile::profileChanged,
        &controller,
        [&controller, &personal_profile]() {
            controller.setTravelLivingPlaces(personal_profile.livingPlaces());
        }
    );
    ExportController export_controller(backend, isolated_settings_file);
    CompositionController composition_controller;
    QObject::connect(
        &composition_controller,
        &CompositionController::saved,
        &controller,
        &ReviewController::scanFolder
    );
    CacheMaintenanceController cache_maintenance_controller(backend, &cache_preferences);
    PeopleAnalysisController people_analysis_controller(
        PeopleAnalysisController::Operations{
            .load = [backend]() { return backend->peopleLibrarySnapshot(); },
            .begin = [backend](
                         const bool authorized
                     ) { return backend->beginPeopleAnalysisJob(authorized); },
            .execute =
                [backend,
                 infer_base_url,
                 infer_credential_file](const std::uint64_t job_token, const bool authorized) {
                    return backend->executePeopleAnalysisJob(
                        job_token,
                        infer_base_url,
                        infer_credential_file,
                        authorized
                    );
                },
            .progress = [backend](
                            const std::uint64_t job_token
                        ) { return backend->peopleAnalysisJobStatus(job_token); },
            .cancel = [backend](
                          const std::uint64_t job_token
                      ) { return backend->cancelPeopleAnalysisJob(job_token); },
            .retire = [backend](
                          const std::uint64_t job_token
                      ) { backend->retirePeopleAnalysisJob(job_token); },
            .merge = [backend](
                         const QStringList& person_ids
                     ) { return backend->mergePeople(person_ids); },
            .rename =
                [backend](const QString& person_id, const QString& display_name) {
                    return backend->renamePerson(person_id, display_name);
                },
            .undo_merge = [backend]() { return backend->undoPeopleMerge(); },
            .clear = [backend]() { backend->clearPeopleData(); },
            .split =
                [backend](const QString& id, const QStringList& photos) {
                    return backend->splitPerson(id, photos);
                },
            .reset_scan = [backend]() { backend->resetPeopleAnalysisProgress(); },
        },
        &ai_preferences
    );
    PeopleScopeController people_scope_controller(
        [backend](const BackendLibraryPhotoFilter& filter) {
            return backend->libraryPhotoCount(filter);
        },
        [backend](
            const BackendLibraryPhotoFilter& filter,
            const BackendLibraryPhotoCursor& cursor,
            std::uint32_t limit
        ) {
            return backend->libraryPhotoPage(
                filter,
                BackendLibraryPhotoOrder::CaptureTimeDescending,
                cursor,
                limit
            );
        },
        [&controller]() { return controller.peopleScopeSnapshot(); },
        &people_analysis_controller
    );
    QObject::connect(
        &controller,
        &ReviewController::filtersChanged,
        &people_scope_controller,
        &PeopleScopeController::invalidate
    );
    QObject::connect(
        &controller,
        &ReviewController::libraryAlbumChanged,
        &people_scope_controller,
        &PeopleScopeController::invalidate
    );
    QObject::connect(
        &controller,
        &ReviewController::libraryAlbumsChanged,
        &people_scope_controller,
        &PeopleScopeController::invalidate
    );
    QObject::connect(
        &controller,
        &ReviewController::decisionCommitted,
        &people_scope_controller,
        &PeopleScopeController::invalidate
    );
    QObject::connect(
        &controller,
        &ReviewController::decisionUndone,
        &people_scope_controller,
        &PeopleScopeController::invalidate
    );
    QObject::connect(
        &controller,
        &ReviewController::colorLabelChanged,
        &people_scope_controller,
        &PeopleScopeController::invalidate
    );
    QObject::connect(
        &controller,
        &ReviewController::likedChanged,
        &people_scope_controller,
        &PeopleScopeController::invalidate
    );
    QObject::connect(
        &controller,
        &ReviewController::sourceAvailabilityChanged,
        &people_scope_controller,
        &PeopleScopeController::invalidate
    );
    SemanticSearchController semantic_search_controller(
        [backend, infer_base_url, infer_credential_file](
            const QString& query,
            const QString& query_revision,
            const QString& language,
            std::uint64_t token
        ) {
            return backend->searchSemantics(
                infer_base_url,
                infer_credential_file,
                query,
                query_revision,
                language,
                token
            );
        },
        [backend]() { return backend->beginSemanticSearch(); },
        [backend](std::uint64_t token) { backend->cancelSemanticSearch(token); }
    );
    SemanticSearchController similar_review_controller(
        SemanticSearchController::ImageRunner{[backend, infer_base_url, infer_credential_file](
                                                  const QString& photo_id,
                                                  const QString& representation_id,
                                                  std::uint64_t token
                                              ) {
            return backend->suggestSimilarReview(
                infer_base_url,
                infer_credential_file,
                photo_id,
                representation_id,
                token
            );
        }},
        [backend]() { return backend->beginSimilarReview(); },
        [backend](std::uint64_t token) { backend->cancelSimilarReview(token); }
    );
    ReviewClusteringController review_clustering_controller(
        [backend, infer_base_url, infer_credential_file](
            const QString& photo_id,
            const QString& representation_id,
            std::uint64_t token
        ) {
            return backend->suggestSimilarReview(
                infer_base_url,
                infer_credential_file,
                photo_id,
                representation_id,
                token
            );
        },
        [backend]() { return backend->beginSimilarReview(); },
        [backend](std::uint64_t token) { backend->cancelSimilarReview(token); },
        [backend](
            const QString& album_id,
            const BackendLibraryPhotoCursor& cursor,
            std::uint32_t limit
        ) {
            BackendLibraryPhotoFilter filter;
            filter.album_id = album_id;
            return backend->libraryPhotoPage(
                filter,
                BackendLibraryPhotoOrder::CaptureTimeDescending,
                cursor,
                limit
            );
        }
    );
    SmartCategoryController smart_category_controller(
        [backend, infer_base_url, infer_credential_file](
            const QVector<BackendSmartCategoryDefinition>& definitions,
            const QString& config_revision,
            const QString& generation,
            const bool start_new,
            const bool clear_embeddings,
            const std::uint64_t token
        ) {
            return backend->classifySmartCategoriesBatch(
                infer_base_url,
                infer_credential_file,
                definitions,
                config_revision,
                generation,
                start_new,
                clear_embeddings,
                token
            );
        },
        [backend]() { return backend->smartClassificationSnapshot(); },
        [backend](const QString& category_id) {
            return backend->smartCategoryMembers(category_id);
        },
        [backend]() { return backend->smartCategoryReviewQueue(); },
        [backend](
            const QString& photo_id,
            const QString& representation_id,
            const QString& category_id,
            const std::int8_t decision
        ) {
            backend->setSmartCategoryFeedback(photo_id, representation_id, category_id, decision);
        },
        [backend](
            const QString& photo_id,
            const QString& representation_id,
            const QVector<BackendSmartCategoryFeedbackDecision>& decisions
        ) { backend->completeSmartCategoryReview(photo_id, representation_id, decisions); },
        [backend](const QString& generation) { backend->pauseSmartClassification(generation); },
        isolated_settings_file,
        {
            [backend]() { return backend->beginSmartClassification(); },
            [backend](const std::uint64_t token) { backend->cancelSmartClassification(token); },
        }
    );
    ImageUnderstandingController image_understanding_controller(
        &ai_preferences,
        [backend, infer_base_url, infer_credential_file](
            const QString& scan_scope,
            const int minimum_rating,
            const QString& generation,
            const bool start_new,
            const bool auto_apply_keywords
        ) {
            return backend->processImageUnderstandingBatch(
                infer_base_url,
                infer_credential_file,
                scan_scope,
                static_cast<std::uint8_t>(std::clamp(minimum_rating, 1, 5)),
                generation,
                start_new,
                auto_apply_keywords
            );
        },
        [backend]() { return backend->imageUnderstandingSnapshot(); },
        [backend](const QString& generation) {
            return backend->pauseImageUnderstanding(generation);
        },
        [backend, infer_base_url, infer_credential_file](
            const QString& photo_id,
            const QString& representation_id,
            const QString& taxonomy_revision,
            const QVector<BackendClassificationReviewCategory>& categories
        ) {
            return backend->reviewSmartClassificationWithModel(
                infer_base_url,
                infer_credential_file,
                photo_id,
                representation_id,
                taxonomy_revision,
                categories
            );
        },
        [backend](
            const QString& photo_id,
            const QString& representation_id,
            const QString& source_revision
        ) {
            return backend
                ->acceptAdvancedClassificationReview(photo_id, representation_id, source_revision);
        },
        [backend](
            const QString& photo_id,
            const QString& representation_id,
            const QString& source_revision
        ) {
            backend
                ->dismissAdvancedClassificationReview(photo_id, representation_id, source_revision);
        },
        [backend](const QString& photo_id, const QString& representation_id) {
            return backend->imageUnderstandingProposal(photo_id, representation_id);
        },
        [backend](
            const QString& photo_id,
            const QString& representation_id,
            const QString& source_revision
        ) {
            backend->applyImageUnderstandingKeywords(photo_id, representation_id, source_revision);
        },
        [&smart_category_controller]() {
            return smart_category_controller.enabledReviewCategories();
        },
        [&smart_category_controller]() {
            return smart_category_controller.reviewTaxonomyRevision();
        }
    );
    QObject::connect(
        &image_understanding_controller,
        &ImageUnderstandingController::advancedReviewAccepted,
        &smart_category_controller,
        &SmartCategoryController::reloadAfterExternalFeedback
    );
    QObject::connect(
        &semantic_search_controller,
        &SemanticSearchController::resultsChanged,
        &controller,
        [&controller, &semantic_search_controller]() {
            controller.setSemanticRepresentationOrder(
                semantic_search_controller.rankedRepresentationKeys()
            );
        }
    );
    QObject::connect(
        &controller,
        &ReviewController::allFiltersCleared,
        &semantic_search_controller,
        &SemanticSearchController::clearSessionResults
    );
    QObject::connect(
        &controller,
        &ReviewController::allFiltersCleared,
        &similar_review_controller,
        &SemanticSearchController::clearSessionResults
    );
    QObject::connect(
        &smart_category_controller,
        &SmartCategoryController::selectionChanged,
        &controller,
        [&controller, &smart_category_controller]() {
            controller.setSmartCategoryRepresentationKeys(
                smart_category_controller.selectedRepresentationKeys()
            );
        }
    );
    QObject::connect(
        &controller,
        &ReviewController::allFiltersCleared,
        &smart_category_controller,
        &SmartCategoryController::clearSelection
    );
    QTimer smart_category_refresh_timer;
    smart_category_refresh_timer.setSingleShot(true);
    smart_category_refresh_timer.setInterval(700);
    QObject::connect(
        &smart_category_refresh_timer,
        &QTimer::timeout,
        &smart_category_controller,
        [&controller, &smart_category_controller]() {
            if (!controller.scanning() && !controller.refreshing() && controller.itemCount() > 0) {
                smart_category_controller.ensureCurrent();
            }
        }
    );
    const auto schedule_smart_categories = [&smart_category_refresh_timer]() {
        smart_category_refresh_timer.start();
    };
    QObject::connect(
        &controller,
        &ReviewController::scanningChanged,
        &smart_category_controller,
        [&controller, &smart_category_controller]() {
            if (!controller.scanning() && controller.itemCount() > 0) {
                smart_category_controller.refresh();
            }
        }
    );
    QObject::connect(
        &controller,
        &ReviewController::scanningChanged,
        &image_understanding_controller,
        [&controller, &image_understanding_controller]() {
            if (!controller.scanning() && controller.itemCount() > 0)
                image_understanding_controller.notifyLibraryChanged();
        }
    );
    QTimer::singleShot(0, &smart_category_controller, schedule_smart_categories);
    JustifiedReviewLayoutModel justified_review_layout;
    justified_review_layout.setSourceModel(controller.model());
    ReviewGalleryGroupingController review_gallery_grouping;
    review_gallery_grouping.setSourceModel(controller.model());
    auto edit_preview_store = std::make_shared<EditPreviewStore>();
    auto edit_preview_presentation_context = std::make_shared<EditPreviewPresentationContext>();
    EditPreviewPresentationRegistry edit_preview_presentation(
        edit_preview_store,
        edit_preview_presentation_context
    );
    EditController
        editor(backend, edit_preview_store, edit_preview_presentation_context, &ai_preferences);
    EditNeighborPreheater edit_neighbor_preheater(backend, editor, justified_review_layout);
    EditInterchangeController edit_interchange_controller(*backend, editor);
    AdjustmentTransferController adjustment_transfer_controller(
        adjustmentTransferOperations(*backend),
        adjustmentTransferEditor(editor)
    );
    SharedGradeLinkController shared_grade_link_controller(
        sharedGradeLinkOperations(*backend),
        sharedGradeLinkEditor(editor)
    );
    QObject::connect(
        &preferences,
        &UiPreferences::effectiveLanguageChanged,
        &shared_grade_link_controller,
        &SharedGradeLinkController::retranslateUi
    );
    QObject::connect(
        &editor,
        &EditController::busyChanged,
        &shared_grade_link_controller,
        &SharedGradeLinkController::foregroundWorkChanged
    );
    QObject::connect(
        &shared_grade_link_controller,
        &SharedGradeLinkController::sharedNodesChanged,
        &editor,
        &EditController::refreshSharedGradeNodes
    );
    QObject::connect(
        &shared_grade_link_controller,
        &SharedGradeLinkController::sharedNodesChanged,
        &controller,
        &ReviewController::refreshSharedGradeNodes
    );
    QObject::connect(
        &editor,
        &EditController::busyChanged,
        &adjustment_transfer_controller,
        &AdjustmentTransferController::foregroundWorkChanged
    );
    QTimer adjustment_transfer_refresh;
    adjustment_transfer_refresh.setSingleShot(true);
    adjustment_transfer_refresh.setInterval(250);
    QObject::connect(
        &adjustment_transfer_controller,
        &AdjustmentTransferController::photosChanged,
        &adjustment_transfer_refresh,
        qOverload<>(&QTimer::start)
    );
    QObject::connect(
        &shared_grade_link_controller,
        &SharedGradeLinkController::photosChanged,
        &adjustment_transfer_refresh,
        qOverload<>(&QTimer::start)
    );
    QObject::connect(
        &adjustment_transfer_refresh,
        &QTimer::timeout,
        &controller,
        &ReviewController::refreshVisibleLibrary
    );
    LutExportController lut_export_controller([&](bool selected_only) {
        if (!editor.active() || editor.stateBusy()
            || (selected_only && editor.selectedRecipeNodeKind() != QStringLiteral("grade"))) {
            return BackendLutExportSnapshot{};
        }
        auto snapshot = backend->prepareLutExport(
            editor.gradeStackForInterchange(),
            selected_only ? editor.selectedGradeNodeId() : QString{}
        );
        const auto display_nodes = editor.gradeNodes();
        const auto display_label = [&display_nodes](const QString& raw_label) {
            for (const auto& value : display_nodes) {
                const auto node = value.toMap();
                if (node.value(QStringLiteral("rawLabel")).toString() == raw_label) {
                    return node.value(QStringLiteral("label")).toString();
                }
            }
            return raw_label;
        };
        for (auto& label : snapshot.included_nodes) {
            label = display_label(label);
        }
        for (auto& omission : snapshot.omissions) {
            omission.node_label = display_label(omission.node_label);
        }
        return snapshot;
    });
    HistoryCoordinator history({
        .photo_page = [backend](
                          const QString& photo_id,
                          const BackendHistoryCursor& after,
                          const std::uint32_t limit
                      ) { return backend->photoHistoryPage(photo_id, after, limit); },
        .library_page =
            [backend](const BackendHistoryCursor& after, const std::uint32_t limit) {
                return backend->libraryHistoryPage(after, limit);
            },
        .library_ref_page =
            [backend](const QString& after_name, const std::uint32_t limit) {
                return backend->libraryHistoryRefPage(after_name, limit);
            },
    });
    QObject::connect(
        &preferences,
        &UiPreferences::effectiveLanguageChanged,
        &history,
        &HistoryCoordinator::retranslateUi
    );
    QObject::connect(
        &preferences,
        &UiPreferences::effectiveLanguageChanged,
        &people_analysis_controller,
        &PeopleAnalysisController::retranslateUi
    );
    QObject::connect(
        &preferences,
        &UiPreferences::effectiveLanguageChanged,
        &semantic_search_controller,
        &SemanticSearchController::retranslateUi
    );
    QObject::connect(
        &preferences,
        &UiPreferences::effectiveLanguageChanged,
        &similar_review_controller,
        &SemanticSearchController::retranslateUi
    );
    QObject::connect(
        &preferences,
        &UiPreferences::effectiveLanguageChanged,
        &review_clustering_controller,
        &ReviewClusteringController::retranslateUi
    );
    QObject::connect(
        &preferences,
        &UiPreferences::effectiveLanguageChanged,
        &image_understanding_controller,
        &ImageUnderstandingController::retranslateUi
    );
    QObject::connect(
        &preferences,
        &UiPreferences::effectiveLanguageChanged,
        &review_gallery_grouping,
        &ReviewGalleryGroupingController::retranslateUi
    );
    StorageProfileArchive profile_archive;
    StorageProfileController profile_controller(
        *storage_profile,
        profile_registry,
        launch_context,
        StorageProfileController::SwitchHooks{
            .blocker =
                [&composition_controller] {
                    return composition_controller.ready()
                                   && composition_controller.savedPath().isEmpty()
                               ? QCoreApplication::translate(
                                     "StorageProfileController",
                                     "Save the composed image before switching libraries."
                                 )
                               : QString{};
                },
            .suspend_background =
                [&](bool pending) {
                    export_controller.setProfileSwitchPending(pending);
                    image_understanding_controller.setProfileSwitchPending(pending);
                    smart_category_controller.setProfileSwitchPending(pending);
                    adjustment_transfer_controller.setProfileSwitchPending(pending);
                    shared_grade_link_controller.setProfileSwitchPending(pending);
                },
            .jobs_quiet =
                [&] {
                    return !editor.busy() && !controller.scanning() && !export_controller.busy()
                           && !controller.decisionBusy() && !controller.comparisonBusy()
                           && !controller.libraryKeywordsBusy() && !controller.libraryAlbumsBusy()
                           && !controller.libraryMetadataBusy()
                           && !controller.librarySourceRemovalBusy()
                           && !controller.librarySourceReconcileBusy()
                           && !controller.sourceRelinkBusy() && !controller.remoteLibrarySyncing()
                           && !controller.remoteLibraryMaterializing()
                           && !composition_controller.busy() && !people_analysis_controller.busy()
                           && !people_scope_controller.busy()
                           && !cache_maintenance_controller.busy()
                           && !smart_category_controller.busy()
                           && !image_understanding_controller.busy()
                           && !image_understanding_controller.advancedReviewBusy()
                           && !lut_export_controller.busy()
                           && !edit_interchange_controller.recipeReading()
                           && !edit_interchange_controller.recipeExportBusy()
                           && !edit_interchange_controller.recipeAdaptationRunning()
                           && !adjustment_transfer_controller.busy()
                           && !shared_grade_link_controller.busy();
                },
            .prepare_editor = [&editor] { return editor.prepareToClose(); },
            .restart = {},
            .archive_blocker = {},
            .archive = [&profile_archive](
                           const StorageProfileArchiveRequest& request
                       ) { profile_archive.start(request); },
            .cancel_archive = [&profile_archive] { profile_archive.cancel(); },
        }
    );
    QObject::connect(
        &profile_archive,
        &StorageProfileArchive::completed,
        &profile_controller,
        &StorageProfileController::archiveCompleted
    );
    QObject::connect(
        &profile_archive,
        &StorageProfileArchive::failed,
        &profile_controller,
        &StorageProfileController::archiveFailed
    );
    QObject::connect(
        &profile_archive,
        &StorageProfileArchive::progress,
        &profile_controller,
        &StorageProfileController::archiveProgress
    );
    QObject::connect(
        &editor,
        &EditController::closeReady,
        &profile_controller,
        &StorageProfileController::editorReady
    );
    QObject::connect(&editor, &EditController::closeSaveFailed, &profile_controller, [&] {
        profile_controller.editorSaveFailed(editor.autosaveErrorText());
    });
    QObject::connect(
        &profile_controller,
        &StorageProfileController::switched,
        &application,
        &QCoreApplication::quit
    );
    QQmlApplicationEngine engine;
    preferences.attachEngine(engine);

    auto* const thumbnail_provider = new ThumbnailProvider(backend, controller.reviewModel());
    engine.addImageProvider(QStringLiteral("shadow"), thumbnail_provider);
    engine.addImageProvider(
        QStringLiteral("shadow-review-detail"),
        new ReviewFocusDetailProvider(controller.focusDetailStore())
    );
    auto* const edit_preview_provider =
        new EditPreviewProvider(edit_preview_store, edit_preview_presentation_context);
    engine.addImageProvider(QStringLiteral("shadow-edit"), edit_preview_provider);
    engine.addImageProvider(
        QStringLiteral("shadow-lut"),
        new LutPreviewProvider(
            QDir(application_data).filePath(QStringLiteral("lut-store")),
            QDir(cache_root).filePath(QStringLiteral("lut-previews"))
        )
    );
    engine.setInitialProperties({
        {QStringLiteral("adjustmentTransferController"),
         QVariant::fromValue(&adjustment_transfer_controller)},
        {QStringLiteral("sharedGradeLinkController"),
         QVariant::fromValue(&shared_grade_link_controller)},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("lutExportController"), QVariant::fromValue(&lut_export_controller)},
        {QStringLiteral("compositionController"), QVariant::fromValue(&composition_controller)},
        {
            QStringLiteral("justifiedReviewLayout"),
            QVariant::fromValue(&justified_review_layout),
        },
        {
            QStringLiteral("reviewGalleryGrouping"),
            QVariant::fromValue(&review_gallery_grouping),
        },
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("editNeighborPreheater"), QVariant::fromValue(&edit_neighbor_preheater)},
        {
            QStringLiteral("editInterchangeController"),
            QVariant::fromValue(&edit_interchange_controller),
        },
        {
            QStringLiteral("editPreviewPresentation"),
            QVariant::fromValue(&edit_preview_presentation),
        },
        {
            QStringLiteral("exportController"),
            QVariant::fromValue(&export_controller),
        },
        {
            QStringLiteral("cacheMaintenanceController"),
            QVariant::fromValue(&cache_maintenance_controller),
        },
        {
            QStringLiteral("peopleAnalysisController"),
            QVariant::fromValue(&people_analysis_controller),
        },
        {
            QStringLiteral("peopleScopeController"),
            QVariant::fromValue(&people_scope_controller),
        },
        {
            QStringLiteral("semanticSearchController"),
            QVariant::fromValue(&semantic_search_controller),
        },
        {
            QStringLiteral("similarReviewController"),
            QVariant::fromValue(&similar_review_controller),
        },
        {
            QStringLiteral("reviewClusteringController"),
            QVariant::fromValue(&review_clustering_controller),
        },
        {
            QStringLiteral("smartCategoryController"),
            QVariant::fromValue(&smart_category_controller),
        },
        {
            QStringLiteral("imageUnderstandingController"),
            QVariant::fromValue(&image_understanding_controller),
        },
        {QStringLiteral("historyController"), QVariant::fromValue(&history)},
        {QStringLiteral("preferences"), QVariant::fromValue(&preferences)},
        {QStringLiteral("storageProfileController"), QVariant::fromValue(&profile_controller)},
        {QStringLiteral("profileRestartPending"), switch_endpoint.requested()},
        {QStringLiteral("personalProfile"), QVariant::fromValue(&personal_profile)},
        {
            QStringLiteral("personalLocationSearch"),
            QVariant::fromValue(&personal_location_search),
        },
        {QStringLiteral("aiPreferences"), QVariant::fromValue(&ai_preferences)},
        {QStringLiteral("cachePreferences"), QVariant::fromValue(&cache_preferences)},
        {
            QStringLiteral("mapProviderPreferences"),
            QVariant::fromValue(&map_provider_preferences),
        },
        {
            QStringLiteral("libraryWebMapController"),
            QVariant::fromValue(&library_web_map_controller),
        },
        {
            QStringLiteral("amapPlaceSearchService"),
            QVariant::fromValue(&amap_place_search_service),
        },
        {QStringLiteral("lutLibrary"), QVariant::fromValue(&lut_library)},
        {
            QStringLiteral("opticsProfileLibrary"),
            QVariant::fromValue(&optics_profile_library),
        },
        {
            QStringLiteral("initialSettingsSection"),
            initial_settings_section,
        },
    });
    engine.loadFromModule("Shadow.App", "Main");
    if (engine.rootObjects().isEmpty()) {
        switch_endpoint.reportFailure(
            QCoreApplication::translate(
                "StorageProfileController",
                "Could not load the target library window."
            )
        );
        return EXIT_FAILURE;
    }
    auto* const root_window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (root_window == nullptr) {
        switch_endpoint.reportFailure(
            QCoreApplication::translate(
                "StorageProfileController",
                "Could not load the target library window."
            )
        );
        qCritical() << "Shadow.App root must be a QQuickWindow";
        return EXIT_FAILURE;
    }
    edit_preview_presentation_context->attach(root_window);

#if defined(Q_OS_MACOS)
    QObject* const root_object = engine.rootObjects().constFirst();
    QObject* const title_toolbar = root_object->findChild<QObject*>(QStringLiteral("titleToolBar"));
    const int title_bar_height =
        title_toolbar == nullptr ? 44 : qRound(title_toolbar->property("height").toReal());
    installMacTitleBarAlignment(qobject_cast<QWindow*>(root_object), title_bar_height);
#endif

    QObject::connect(&switch_endpoint, &StorageProfileSwitchEndpoint::activate, &application, [&] {
        profile_controller.rememberCurrent();
        root_window->setProperty("profileRestartPending", false);
        switch_endpoint.confirmActivated();
    });
    if (switch_endpoint.requested())
        switch_endpoint.ready(storage_profile->info());
    else if (!headless_startup_smoke)
        profile_controller.rememberCurrent();

    auto profile_switch_smoke = DesktopSmoke::installStorageProfileSwitch(
        application,
        engine,
        backend,
        editor,
        profile_controller
    );
    installDesktopSmokeHarness(
        application,
        engine,
        controller,
        editor,
        edit_neighbor_preheater,
        thumbnail_provider,
        edit_preview_provider,
        preferences,
        initialScanFolder()
    );
    return application.exec();
}
