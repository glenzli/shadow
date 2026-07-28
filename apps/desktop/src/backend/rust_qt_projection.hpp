#pragma once

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace desktop_backend_projection {

[[nodiscard]] inline QString qstring(const rust::String& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(length));
}

[[nodiscard]] inline QByteArray qbytes(
    const rust::Vec<std::uint8_t>& value
) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QByteArray(
        reinterpret_cast<const char*>(value.data()),
        static_cast<qsizetype>(length)
    );
}

[[nodiscard]] inline qsizetype checked_qt_vector_size(
    const std::size_t size,
    const char* const field
) {
    if (size
        > static_cast<std::size_t>(
            std::numeric_limits<qsizetype>::max()
        )) {
        throw std::length_error(
            std::string("desktop bridge vector is too large: ") + field
        );
    }
    return static_cast<qsizetype>(size);
}

[[nodiscard]] inline QVector<std::uint64_t> qcounts(
    const rust::Vec<std::uint64_t>& value,
    const char* const field
) {
    QVector<std::uint64_t> result;
    result.reserve(checked_qt_vector_size(value.size(), field));
    for (const auto count : value) {
        result.push_back(count);
    }
    return result;
}

} // namespace desktop_backend_projection
