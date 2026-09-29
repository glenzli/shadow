#include "edit_performance_diagnostics.hpp"

#include <QDebug>
#include <QString>

#include <cstdint>
#include <optional>

#if defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <mach/task_info.h>
#endif

namespace {

[[nodiscard]] std::optional<std::uint64_t> physical_footprint_bytes() noexcept {
#if defined(Q_OS_MACOS)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(
            mach_task_self(),
            TASK_VM_INFO,
            reinterpret_cast<task_info_t>(&info),
            &count
        ) == KERN_SUCCESS) {
        return info.phys_footprint;
    }
#endif
    return std::nullopt;
}

} // namespace

void log_edit_performance_checkpoint(
    const char* const stage,
    const quint64 photo_generation,
    const quint64 recipe_revision,
    const quint64 retained_source_bytes
) {
    if (qEnvironmentVariable("SHADOW_INTERACTIVE_TIMING") != QStringLiteral("1")) {
        return;
    }
    const auto footprint = physical_footprint_bytes();
    qInfo().noquote()
        << QStringLiteral("shadow.edit-checkpoint stage=%1 photo_generation=%2 recipe_revision=%3 retained_source_bytes=%4 physical_footprint_bytes=%5")
               .arg(QString::fromLatin1(stage))
               .arg(photo_generation)
               .arg(recipe_revision)
               .arg(retained_source_bytes)
               .arg(footprint.has_value() ? QString::number(*footprint)
                                          : QStringLiteral("unavailable"));
}
