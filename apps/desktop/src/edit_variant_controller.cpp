#include "edit_controller.hpp"

#include <QVariantMap>

#include <exception>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage variant_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

} // namespace

// Variant authoring is intentionally a small Catalog transaction. Pixel work
// remains asynchronous, while these metadata-only operations complete before
// the controller accepts the returned active Recipe head.
void EditController::setPhotoVariants(
    QString active_variant_id,
    QVector<BackendPhotoVariant> variants
) {
    QVariantList projected;
    projected.reserve(variants.size());
    for (BackendPhotoVariant& variant : variants) {
        projected.push_back(QVariantMap{
            {QStringLiteral("variantId"), std::move(variant.variant_id)},
            {QStringLiteral("name"), std::move(variant.name)},
            {QStringLiteral("headCommitId"), std::move(variant.head_commit_id)},
            {QStringLiteral("hasHead"), variant.has_head},
            {QStringLiteral("isDefault"), variant.is_default},
            {QStringLiteral("isActive"), variant.is_active},
            {QStringLiteral("createdAtMs"), variant.created_at_ms},
            {QStringLiteral("updatedAtMs"), variant.updated_at_ms},
        });
    }
    if (active_variant_id_ == active_variant_id && photo_variants_ == projected) {
        return;
    }
    active_variant_id_ = std::move(active_variant_id);
    photo_variants_ = std::move(projected);
    emit photoVariantsChanged();
}

void EditController::createVariant(const QString& name) {
    const QString normalized = name.trimmed();
    if (!variantActionsEnabled() || normalized.isEmpty()) {
        return;
    }
    try {
        applyState(backend_->createPhotoVariant(photo_id_, source_path_, normalized));
        setStatusMessage(variant_message(
            QT_TRANSLATE_NOOP("EditController", "Variant “%1” created"), {normalized}
        ));
        schedulePreview(0);
    } catch (const std::exception& error) {
        setStatusMessage(variant_message(
            QT_TRANSLATE_NOOP("EditController", "Could not create Variant · %1"),
            {QString::fromUtf8(error.what())}
        ));
    }
}

void EditController::renameVariant(const QString& variant_id, const QString& name) {
    const QString normalized_id = variant_id.trimmed();
    const QString normalized_name = name.trimmed();
    if (!variantActionsEnabled() || normalized_id.isEmpty() || normalized_name.isEmpty()) {
        return;
    }
    try {
        applyState(backend_->renamePhotoVariant(
            photo_id_, source_path_, normalized_id, normalized_name
        ));
        setStatusMessage(variant_message(
            QT_TRANSLATE_NOOP("EditController", "Variant renamed to “%1”"), {normalized_name}
        ));
    } catch (const std::exception& error) {
        setStatusMessage(variant_message(
            QT_TRANSLATE_NOOP("EditController", "Could not rename Variant · %1"),
            {QString::fromUtf8(error.what())}
        ));
    }
}

void EditController::activateVariant(const QString& variant_id) {
    const QString normalized_id = variant_id.trimmed();
    if (!variantActionsEnabled() || normalized_id.isEmpty()
        || normalized_id == active_variant_id_) {
        return;
    }
    try {
        applyState(backend_->activatePhotoVariant(photo_id_, source_path_, normalized_id));
        setStatusMessage(variant_message(
            QT_TRANSLATE_NOOP("EditController", "Variant selected · rendering preview")
        ));
        schedulePreview(0);
    } catch (const std::exception& error) {
        setStatusMessage(variant_message(
            QT_TRANSLATE_NOOP("EditController", "Could not switch Variant · %1"),
            {QString::fromUtf8(error.what())}
        ));
    }
}

void EditController::removeVariant(const QString& variant_id) {
    const QString normalized_id = variant_id.trimmed();
    if (!variantActionsEnabled() || normalized_id.isEmpty()
        || normalized_id == active_variant_id_) {
        return;
    }
    try {
        applyState(backend_->removePhotoVariant(photo_id_, source_path_, normalized_id));
        setStatusMessage(
            variant_message(QT_TRANSLATE_NOOP("EditController", "Variant removed"))
        );
    } catch (const std::exception& error) {
        setStatusMessage(variant_message(
            QT_TRANSLATE_NOOP("EditController", "Could not remove Variant · %1"),
            {QString::fromUtf8(error.what())}
        ));
    }
}
