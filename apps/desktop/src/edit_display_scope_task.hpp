#pragma once

#include "edit_preview_contract.hpp"
#include "preview_diagnostics.hpp"

#include <QByteArray>
#include <QImage>

#include <optional>

struct EditDisplayScopeTaskInput final {
    EditPreviewKind kind = EditPreviewKind::Current;
    quint64 photo_generation = 0;
    quint64 preview_generation = 0;
    quint64 request_revision = 0;
    quint64 reference_epoch = 0;
    QByteArray encoded_preview;
    QImage decoded_preview;
    std::optional<PreviewScopeHueQualifier> qualifier;
    std::optional<PreviewScopeReferenceSelection> reference;
};

struct EditDisplayScopeTaskResult final {
    EditPreviewKind kind = EditPreviewKind::Current;
    quint64 photo_generation = 0;
    quint64 preview_generation = 0;
    quint64 request_revision = 0;
    quint64 reference_epoch = 0;
    QImage decoded_preview;
    PreviewDisplayScopeAnalysis scope;
    std::optional<PreviewScopeReferenceSelection> reference;
};

// Decode once on a worker. A frozen Point Color reference is session-only and
// may be reused across successive rendered previews while its qualifier matches.
[[nodiscard]] EditDisplayScopeTaskResult
run_edit_display_scope_task(EditDisplayScopeTaskInput input);
