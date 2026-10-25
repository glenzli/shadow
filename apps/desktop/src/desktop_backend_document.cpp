#include "backend/desktop_backend_private.hpp"
#include "backend/edit_settings_projection.hpp"
#include "backend/native_path_input.hpp"

#include <stdexcept>

namespace {
QString text(const rust::String& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
DocumentHistoryEntry entry(const shadow::desktop::FfiDocumentHistoryEntry& value) {
    return {
        {desktop_backend_projection::grade_stack(value.before), text(value.before_base)},
        {desktop_backend_projection::grade_stack(value.after), text(value.after_base)},
        text(value.key).toStdString()
    };
}
} // namespace

QVector<DesktopBackend::PipelineInput>
DesktopBackend::unpackDocument(const QString& path, const QString& root) {
    QVector<PipelineInput> result;
    for (const auto& value : shadow::desktop::unpack_editable_document(
             native_path_input::path(path),
             native_path_input::path(root)
         ))
        result.push_back(
            {text(value.photo_id),
             text(value.representation_id),
             text(value.source_path),
             text(value.title)}
        );
    return result;
}

QString DesktopBackend::packDocument(
    const QString& root,
    const QString& path,
    const QString& expected_digest
) {
    return text(
        shadow::desktop::pack_editable_document(
            native_path_input::path(root),
            native_path_input::path(path),
            expected_digest.toStdString()
        )
    );
}

void DesktopBackend::recordDocumentHistory(
    const QString& photo,
    const QString& source,
    const unsigned action,
    DocumentHistoryEntry value
) const {
    const auto* session = &*impl_->session;
    impl_->document_history.append([session, photo, source, action, value = std::move(value)] {
        const auto& before = action == 1 ? value.after : value.before;
        const auto& after = action == 1 ? value.before : value.after;
        const shadow::desktop::FfiDocumentHistoryEntry entry{
            .key = value.key,
            .before_base = before.base_commit_id.toStdString(),
            .before = desktop_backend_projection::ffi_grade_stack(before.stack),
            .after_base = after.base_commit_id.toStdString(),
            .after = desktop_backend_projection::ffi_grade_stack(after.stack),
        };
        session->record_document_history(
            photo.toStdString(),
            source.toStdString(),
            static_cast<std::uint8_t>(action),
            entry
        );
    });
}

void DesktopBackend::flushDocumentHistory() const {
    impl_->document_history.flush();
}

DocumentUndo DesktopBackend::documentUndo(const QString& photo) const {
    flushDocumentHistory();
    const auto value = impl_->session->document_undo(photo.toStdString());
    DocumentUndo result;
    for (const auto& item : value.undo)
        result.undo.push_back(entry(item));
    for (const auto& item : value.redo)
        result.redo.push_back(entry(item));
    return result;
}

QString DesktopBackend::documentHistoryPage(const QString& photo, qint64 before) const {
    flushDocumentHistory();
    return text(impl_->session->document_history_page(photo.toStdString(), before));
}

DocumentHistoryEntry DesktopBackend::documentHistoryStep(const QString& photo, qint64 step) const {
    flushDocumentHistory();
    return entry(impl_->session->checkout_document_history(photo.toStdString(), step));
}
