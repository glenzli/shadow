#pragma once

#include "adjustment_transfer_operations.hpp"

#include <QVariantList>

// Presentation choices and their exact projection share one field inventory.
// The source is a frozen metadata snapshot, never a live editor binding.
namespace AdjustmentTransferSelection {
[[nodiscard]] QVariantList options(const BackendGradeStack& stack);
[[nodiscard]] AdjustmentTransferSource
select(const AdjustmentTransferSource& source, const QVariantList& choices);
} // namespace AdjustmentTransferSelection
