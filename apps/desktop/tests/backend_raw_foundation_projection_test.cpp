#include "backend/raw_foundation_projection.hpp"
#include "desktop_backend.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <array>
#include <concepts>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

static_assert(std::same_as<
              decltype(std::declval<const DesktopBackend&>().probeRawFoundationRuntime()),
              BackendRawFoundationRuntimeStatus>);
static_assert(std::same_as<
              decltype(std::declval<const DesktopBackend&>().assessRawFoundationNoise(
                  std::declval<const QString&>(),
                  std::declval<const QString&>()
              )),
              BackendRawFoundationNoiseAssessment>);

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "RAW foundation backend contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void phase_projection_is_total() {
    using FfiPhase = shadow::desktop::FfiRawFoundationJobPhase;
    constexpr std::array cases{
        std::pair{FfiPhase::Queued, BackendRawFoundationJobPhase::Queued},
        std::pair{FfiPhase::Planning, BackendRawFoundationJobPhase::Planning},
        std::pair{FfiPhase::Running, BackendRawFoundationJobPhase::Running},
        std::pair{FfiPhase::Ready, BackendRawFoundationJobPhase::Ready},
        std::pair{FfiPhase::Unavailable, BackendRawFoundationJobPhase::Unavailable},
        std::pair{FfiPhase::Cancelled, BackendRawFoundationJobPhase::Cancelled},
        std::pair{FfiPhase::Failed, BackendRawFoundationJobPhase::Failed},
    };
    for (const auto& [source, expected] : cases) {
        require(
            project_raw_foundation_job_phase(source) == expected,
            "every Rust phase has one exact Qt phase"
        );
    }
}

void complete_projection_preserves_every_field() {
    shadow::desktop::FfiRawFoundationRuntimeStatus runtime;
    runtime.available = true;
    runtime.model_id = rust::String("rawnind-bayer-5.6.0");
    runtime.runtime_version = rust::String("tract-0.21");
    runtime.diagnostic = rust::String("verified");
    const auto projected_runtime = project_raw_foundation_runtime_status(runtime);
    require(projected_runtime.available, "runtime availability");
    require(
        projected_runtime.model_id == QStringLiteral("rawnind-bayer-5.6.0"),
        "runtime model identity"
    );
    require(projected_runtime.runtime_version == QStringLiteral("tract-0.21"), "runtime version");
    require(projected_runtime.diagnostic == QStringLiteral("verified"), "runtime diagnostic");

    shadow::desktop::FfiRawFoundationNoiseAssessment noise;
    noise.level = shadow::desktop::FfiRawFoundationNoiseLevel::Moderate;
    noise.score_percent = 57;
    noise.confidence_percent = 83;
    noise.diagnostic = rust::String("staged Bayer residual v1");
    const auto projected_noise = project_raw_foundation_noise_assessment(noise);
    require(
        projected_noise.level == BackendRawFoundationNoiseLevel::Moderate,
        "noise level"
    );
    require(projected_noise.score_percent == 57, "noise score");
    require(projected_noise.confidence_percent == 83, "noise confidence");
    require(
        projected_noise.diagnostic == QStringLiteral("staged Bayer residual v1"),
        "noise diagnostic"
    );

    shadow::desktop::FfiRawFoundationJobStatus source;
    source.job_token = 17;
    source.request_id = rust::String("photo-generation-9");
    source.generation = 9;
    source.phase = shadow::desktop::FfiRawFoundationJobPhase::Ready;
    source.phase_code = rust::String("ready");
    source.completed_basis_points = 10'000;
    source.cancellation_requested = false;
    source.disposition = 2;
    source.cache_key_sha256 = rust::String("cache-identity");
    source.artifact_identity_sha256 = rust::String("artifact-identity");
    source.width = 8'256;
    source.height = 5'504;
    source.diagnostic = rust::String("published");

    const auto result = project_raw_foundation_job_status(source);
    require(result.job_token == 17, "job token");
    require(result.request_id == QStringLiteral("photo-generation-9"), "request identity");
    require(result.generation == 9, "generation");
    require(result.phase == BackendRawFoundationJobPhase::Ready, "phase");
    require(result.phase_code == QStringLiteral("ready"), "phase code");
    require(result.completed_basis_points == 10'000, "progress");
    require(!result.cancellation_requested, "cancellation flag");
    require(result.disposition == 2, "materialization disposition");
    require(result.cache_key_sha256 == QStringLiteral("cache-identity"), "cache identity");
    require(
        result.artifact_identity_sha256 == QStringLiteral("artifact-identity"),
        "artifact identity"
    );
    require(result.width == 8'256 && result.height == 5'504, "foundation extent");
    require(result.diagnostic == QStringLiteral("published"), "job diagnostic");
    require(result.terminal(), "ready is terminal");
}

void production_session_projects_pollable_cancellation() {
    QTemporaryDir root;
    require(root.isValid(), "temporary session root");
    const DesktopBackend backend{
        root.filePath(QStringLiteral("catalog.sqlite")),
        root.filePath(QStringLiteral("cache")),
    };

    const auto runtime = backend.probeRawFoundationRuntime();
    require(
        runtime.available || !runtime.diagnostic.isEmpty(),
        "unavailable runtime carries a diagnostic"
    );
    const auto token = backend.beginRawFoundationJob(QStringLiteral("projection-contract"), 23);
    const auto queued = backend.rawFoundationJobStatus(token);
    require(queued.job_token == token, "production queued token");
    require(queued.request_id == QStringLiteral("projection-contract"), "production request id");
    require(queued.generation == 23, "production generation");
    require(queued.phase == BackendRawFoundationJobPhase::Queued, "production queued phase");
    require(!queued.terminal(), "queued is non-terminal");

    backend.cancelRawFoundationJob(token);
    const auto cancelled = backend.rawFoundationJobStatus(token);
    require(cancelled.cancellation_requested, "production cancellation is pollable");

    const auto failed_token =
        backend.beginRawFoundationJob(QStringLiteral("invalid-source-contract"), 24);
    const auto failed = backend.executeRawFoundationJob(
        failed_token,
        QStringLiteral("not-a-photo-id"),
        QStringLiteral("/missing/source.nef")
    );
    require(
        failed.phase == BackendRawFoundationJobPhase::Failed && failed.terminal(),
        "source preflight failure is terminal"
    );
    require(!failed.diagnostic.isEmpty(), "source preflight diagnostic is preserved");
    backend.retireRawFoundationJob(failed_token);
}

void optional_real_raw_noise_acceptance() {
    const char* const configured = std::getenv("SHADOW_TEST_RAW_FOUNDATION_NOISE_SOURCE");
    if (configured == nullptr) {
        return;
    }
    const QFileInfo source{QString::fromUtf8(configured)};
    require(source.isFile(), "configured noise source is a file");
    QTemporaryDir root;
    require(root.isValid(), "real RAW acceptance root");
    const QString input_directory = root.filePath(QStringLiteral("input"));
    require(QDir{}.mkpath(input_directory), "real RAW input directory");
    const QString copied_source = QDir{input_directory}.filePath(source.fileName());
    require(QFile::copy(source.absoluteFilePath(), copied_source), "copy real RAW fixture");

    const DesktopBackend backend{
        root.filePath(QStringLiteral("catalog.sqlite")),
        root.filePath(QStringLiteral("cache")),
    };
    backend.beginFolderScan(1);
    const auto report = backend.scanFolder(input_directory, 1);
    require(
        report.supported_files == 1 && report.inserted == 1,
        "one real RAW enters the Catalog"
    );
    const auto page = backend.libraryPhotoPage(
        BackendLibraryPhotoFilter{},
        BackendLibraryPhotoOrder::FileNameAscending,
        BackendLibraryPhotoCursor{},
        8
    );
    require(page.items.size() == 1, "real RAW is queryable");
    const auto& item = page.items.front();
    const auto assessment = backend.assessRawFoundationNoise(item.photo_id, item.source_path);
    require(assessment.score_percent <= 100, "real RAW noise score is bounded");
    require(
        assessment.confidence_percent >= 20 && assessment.confidence_percent <= 100,
        "real RAW confidence is bounded"
    );
    require(!assessment.diagnostic.isEmpty(), "real RAW assessment is auditable");
    std::cout << "real RAW noise assessment: level="
              << static_cast<int>(assessment.level)
              << " score=" << static_cast<int>(assessment.score_percent)
              << " confidence=" << static_cast<int>(assessment.confidence_percent) << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        phase_projection_is_total();
        complete_projection_preserves_every_field();
        production_session_projects_pollable_cancellation();
        optional_real_raw_noise_acceptance();
    } catch (const std::exception& error) {
        std::cerr << "RAW foundation production path failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
