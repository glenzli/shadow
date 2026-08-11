#include "ai_preferences.hpp"
#include "amap_place_search_service.hpp"
#include "cache_maintenance_controller.hpp"
#include "cache_preferences.hpp"
#include "desktop_backend.hpp"
#include "desktop_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "edit_preview_presentation_context.hpp"
#include "edit_preview_presentation_registry.hpp"
#include "edit_preview_provider.hpp"
#include "edit_preview_texture_item.hpp"
#include "export_controller.hpp"
#include "geonames_library_reverse_geocoder.hpp"
#include "history_coordinator.hpp"
#include "image_understanding_controller.hpp"
#include "justified_review_layout_model.hpp"
#include "lut_library.hpp"
#include "lut_preview_provider.hpp"
#include "map/library_web_map_controller.hpp"
#include "map_provider_preferences.hpp"
#include "optics_profile_library.hpp"
#include "people_analysis_controller.hpp"
#include "personal_location_search.hpp"
#include "personal_profile.hpp"
#include "review_controller.hpp"
#include "review_focus_detail_provider.hpp"
#include "review_gallery_grouping_controller.hpp"
#include "semantic_search_controller.hpp"
#include "smart_category_controller.hpp"
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
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QWindow>
#include <QtWebView/QtWebView>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace {

[[nodiscard]] bool reset_local_development_catalog(
    const QString& catalog_path,
    const QString& cache_root,
    QString* const error_message
) {
    const QStringList catalog_files{
        catalog_path,
        catalog_path + QStringLiteral("-wal"),
        catalog_path + QStringLiteral("-shm"),
    };
    for (const QString& path : catalog_files) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            *error_message = QObject::tr("Could not remove %1.").arg(path);
            return false;
        }
    }

    QDir cache_directory(cache_root);
    if (cache_directory.exists()) {
        // Remote Library proxies have their own persisted mirror identity and
        // remain valid across a local Catalog schema reset. Removing them while
        // retaining that mirror produces offline white cards until the server
        // next reconnects. Clear only Catalog/runtime-owned cache entries.
        constexpr auto REMOTE_LIBRARY_PREVIEWS = "remote-library-previews";
        const QFileInfoList entries = cache_directory.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System
        );
        for (const QFileInfo& entry : entries) {
            if (entry.fileName() == QString::fromLatin1(REMOTE_LIBRARY_PREVIEWS)) {
                continue;
            }
            const bool removed = entry.isDir() && !entry.isSymLink()
                                     ? QDir(entry.absoluteFilePath()).removeRecursively()
                                     : QFile::remove(entry.absoluteFilePath());
            if (!removed) {
                *error_message = QObject::tr("Could not remove the local preview cache.");
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool is_development_catalog_reset_error(const std::exception& error) {
    return QString::fromUtf8(error.what())
        .contains(QStringLiteral("development catalog reset required"));
}

[[nodiscard]] QMessageBox::StandardButton
offer_development_catalog_reset(const std::exception& error) {
    const QString detail = QString::fromUtf8(error.what());
    const bool incompatible = is_development_catalog_reset_error(error);
    const QString explanation =
        incompatible
            ? QObject::tr(
                  "This local catalog belongs to an incompatible development build. "
                  "Shadow does not migrate development schemas.\n\n"
                  "Resetting removes the local photo index, edit history, and preview cache. "
                  "Your original photo files, LUT library, and UI preferences are not changed."
              )
            : QObject::tr(
                  "Shadow could not open its local development catalog. You can reset it "
                  "and start again with a fresh catalog v1.\n\n"
                  "Resetting removes the local photo index, edit history, and preview cache. "
                  "Your original photo files, LUT library, and UI preferences are not changed.\n\n"
                  "Technical detail: %1"
              )
                  .arg(detail);
    return QMessageBox::warning(
        nullptr,
        QObject::tr("Reset local development catalog?"),
        explanation,
        QMessageBox::Reset | QMessageBox::Cancel,
        QMessageBox::Reset
    );
}

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

    QString application_data = qEnvironmentVariable("SHADOW_DESKTOP_DATA_ROOT");
    if (application_data.isEmpty()) {
        application_data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    }
    QDir().mkpath(application_data);
    const QString catalog_path = QDir(application_data).filePath(QStringLiteral("catalog.sqlite"));
    const QString cache_root = QDir(application_data).filePath(QStringLiteral("cache"));
    const bool headless_startup_smoke = qEnvironmentVariableIsSet("SHADOW_DESKTOP_SMOKE_TEST");
    const QString isolated_settings_file =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_DATA_ROOT")
            ? QDir(application_data).filePath(QStringLiteral("ui-preferences.ini"))
            : QString{};
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

    std::shared_ptr<DesktopBackend> backend;
    while (!backend) {
        try {
            backend = std::make_shared<DesktopBackend>(catalog_path, cache_root);
        } catch (const std::exception& error) {
            qCritical() << "Cannot start Shadow's local backend:" << error.what();
            const bool reset_for_smoke =
                headless_startup_smoke && is_development_catalog_reset_error(error);
            if (!reset_for_smoke && headless_startup_smoke) {
                return EXIT_FAILURE;
            }
            if (!reset_for_smoke && offer_development_catalog_reset(error) != QMessageBox::Reset) {
                return EXIT_FAILURE;
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
    CacheMaintenanceController cache_maintenance_controller(backend, &cache_preferences);
    QString infer_base_url = qEnvironmentVariable("SHADOW_INFER_BASE_URL");
    QString infer_credential_file = qEnvironmentVariable("SHADOW_INFER_CREDENTIAL_FILE");
    if (infer_credential_file.isEmpty()) {
        // Infer Runtime provisions App credentials beside, rather than inside,
        // Shadow's application-specific Catalog/cache directory. Using the
        // generic data root also avoids duplicating the organization and app
        // names (for example, Shadow/Shadow/credentials on macOS).
        infer_credential_file =
            QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                .filePath(QStringLiteral("Shadow/credentials/infer-runtime-shadow.token"));
    }
    PeopleAnalysisController people_analysis_controller(
        [backend, infer_base_url, infer_credential_file]() {
            return backend->analyzePeople(infer_base_url, infer_credential_file);
        }
    );
    SemanticSearchController semantic_search_controller(
        [backend, infer_base_url, infer_credential_file](
            const QString& query,
            const QString& query_revision,
            const QString& language
        ) {
            return backend->searchSemantics(
                infer_base_url,
                infer_credential_file,
                query,
                query_revision,
                language
            );
        }
    );
    SmartCategoryController smart_category_controller(
        [backend, infer_base_url, infer_credential_file](
            const QVector<BackendSmartCategoryDefinition>& definitions,
            const QString& config_revision,
            const QString& generation,
            const bool start_new,
            const bool clear_embeddings
        ) {
            return backend->classifySmartCategoriesBatch(
                infer_base_url,
                infer_credential_file,
                definitions,
                config_revision,
                generation,
                start_new,
                clear_embeddings
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
        [backend](const QString& generation) { backend->pauseSmartClassification(generation); }
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
            return backend->acceptAdvancedClassificationReview(
                photo_id,
                representation_id,
                source_revision
            );
        },
        [backend](
            const QString& photo_id,
            const QString& representation_id,
            const QString& source_revision
        ) {
            backend->dismissAdvancedClassificationReview(
                photo_id,
                representation_id,
                source_revision
            );
        },
        [backend](const QString& photo_id, const QString& representation_id) {
            return backend->imageUnderstandingProposal(photo_id, representation_id);
        },
        [backend](
            const QString& photo_id,
            const QString& representation_id,
            const QString& source_revision
        ) {
            backend->applyImageUnderstandingKeywords(
                photo_id,
                representation_id,
                source_revision
            );
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
        &image_understanding_controller,
        &ImageUnderstandingController::retranslateUi
    );
    QObject::connect(
        &preferences,
        &UiPreferences::effectiveLanguageChanged,
        &review_gallery_grouping,
        &ReviewGalleryGroupingController::retranslateUi
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
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {
            QStringLiteral("justifiedReviewLayout"),
            QVariant::fromValue(&justified_review_layout),
        },
        {
            QStringLiteral("reviewGalleryGrouping"),
            QVariant::fromValue(&review_gallery_grouping),
        },
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
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
            QStringLiteral("semanticSearchController"),
            QVariant::fromValue(&semantic_search_controller),
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
        return EXIT_FAILURE;
    }
    auto* const root_window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (root_window == nullptr) {
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

    installDesktopSmokeHarness(
        application,
        engine,
        controller,
        editor,
        thumbnail_provider,
        edit_preview_provider,
        preferences,
        initialScanFolder()
    );
    return application.exec();
}
