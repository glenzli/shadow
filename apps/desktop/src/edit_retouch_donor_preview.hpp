#pragma once
#include "edit_retouch_donor_selection.hpp"
#include <QString>
#include <memory>
class EditPreviewStore;
// Materializes one bounded current preview on demand. Both settled JPEG and
// resident interactive frames feed the same donor-selection pixel contract.
std::optional<EditRetouchDonorSelection> preview_retouch_source_selection(
    const std::shared_ptr<EditPreviewStore>&,
    const QString&,
    QSize,
    std::span<const QPointF>,
    double,
    int
);
