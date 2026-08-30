#include "backend/desktop_backend_private.hpp"
#include "backend/rust_qt_projection.hpp"

#include <utility>

namespace {

[[nodiscard]] QStringList project_photo_ids(const rust::Vec<rust::String>& source) {
    QStringList projected;
    projected.reserve(
        desktop_backend_projection::checked_qt_vector_size(source.size(), "people group photo ids")
    );
    for (const auto& value : source) {
        projected.push_back(desktop_backend_projection::qstring(value));
    }
    return projected;
}

[[nodiscard]] BackendPeopleAnalysisReport
project_report(const shadow::desktop::FfiPeopleAnalysisReport& source) {
    QVector<BackendPeopleGroup> groups;
    groups.reserve(
        desktop_backend_projection::checked_qt_vector_size(source.groups.size(), "people groups")
    );
    for (const auto& group : source.groups) {
        groups.push_back({
            .group_id = desktop_backend_projection::qstring(group.group_id),
            .display_name = desktop_backend_projection::qstring(group.display_name),
            .member_count = group.member_count,
            .photo_ids = project_photo_ids(group.photo_ids),
            .thumbnail_jpeg = desktop_backend_projection::qbytes(group.thumbnail_jpeg),
            .manually_merged = group.manually_merged,
        });
    }
    return {
        .has_data = source.has_data,
        .analyzed_photos = source.analyzed_photos,
        .detected_faces = source.detected_faces,
        .embedded_faces = source.embedded_faces,
        .skipped_items = source.skipped_items,
        .ungrouped_faces = source.ungrouped_faces,
        .truncated = source.truncated,
        .groups = std::move(groups),
        .can_undo_merge = source.can_undo_merge,
    };
}

[[nodiscard]] rust::Vec<rust::String> rust_strings(const QStringList& source) {
    rust::Vec<rust::String> projected;
    projected.reserve(static_cast<std::size_t>(source.size()));
    for (const QString& value : source) {
        projected.push_back(value.toStdString());
    }
    return projected;
}

} // namespace

BackendPeopleAnalysisReport DesktopBackend::peopleLibrarySnapshot() const {
    return project_report(impl_->session->people_library_snapshot());
}

std::uint64_t DesktopBackend::beginPeopleAnalysisJob(const bool authorized) const {
    return impl_->session->begin_people_analysis_job(authorized);
}

BackendPeopleAnalysisProgress
DesktopBackend::peopleAnalysisJobStatus(const std::uint64_t job_token) const {
    const auto source = impl_->session->people_analysis_job_status(job_token);
    return {
        .job_token = source.job_token,
        .phase = desktop_backend_projection::qstring(source.phase),
        .analyzed_photos = source.analyzed_photos,
        .maximum_photos = source.maximum_photos,
        .detected_faces = source.detected_faces,
        .compared_faces = source.compared_faces,
        .cancellation_requested = source.cancellation_requested,
        .terminal = source.terminal,
    };
}

bool DesktopBackend::cancelPeopleAnalysisJob(const std::uint64_t job_token) const {
    return impl_->session->cancel_people_analysis_job(job_token);
}

BackendPeopleAnalysisExecution DesktopBackend::executePeopleAnalysisJob(
    const std::uint64_t job_token,
    const QString& infer_base_url,
    const QString& credential_file,
    const bool authorized
) const {
    const auto source = impl_->session->execute_people_analysis_job(
        job_token,
        infer_base_url.toStdString(),
        credential_file.toStdString(),
        authorized
    );
    return {
        .job_token = source.job_token,
        .cancelled = source.cancelled,
        .diagnostic = desktop_backend_projection::qstring(source.diagnostic),
        .report = project_report(source.report),
    };
}

void DesktopBackend::retirePeopleAnalysisJob(const std::uint64_t job_token) const {
    impl_->session->retire_people_analysis_job(job_token);
}

BackendPeopleAnalysisReport DesktopBackend::mergePeople(const QStringList& person_ids) const {
    return project_report(impl_->session->merge_people(rust_strings(person_ids)));
}

BackendPeopleAnalysisReport
DesktopBackend::renamePerson(const QString& person_id, const QString& display_name) const {
    return project_report(
        impl_->session->rename_person(person_id.toStdString(), display_name.toStdString())
    );
}

BackendPeopleAnalysisReport DesktopBackend::undoPeopleMerge() const {
    return project_report(impl_->session->undo_people_merge());
}

void DesktopBackend::clearPeopleData() const {
    impl_->session->clear_people_data();
}
