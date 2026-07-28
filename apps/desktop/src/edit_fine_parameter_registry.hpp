#pragma once

#include "desktop_backend.hpp"

#include <QLatin1StringView>
#include <QStringView>

#include <span>

struct EditFineParameterDescriptor final {
    QLatin1StringView key;
    double BackendFineEditParameters::*member = nullptr;
    double minimum = 0.0;
    double maximum = 0.0;
    const char* label_source = nullptr;

    [[nodiscard]] bool writable() const noexcept {
        return label_source != nullptr;
    }
};

namespace EditFineParameterRegistry {

[[nodiscard]] std::span<const EditFineParameterDescriptor> all() noexcept;
[[nodiscard]] const EditFineParameterDescriptor* find(QStringView key) noexcept;

} // namespace EditFineParameterRegistry
