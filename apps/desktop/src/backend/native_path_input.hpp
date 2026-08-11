#pragma once

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace native_path_input {

[[nodiscard]] inline std::size_t checked_size(const qsizetype size) {
    if (size < 0) {
        throw std::length_error("Qt reported a negative native path length");
    }
    return static_cast<std::size_t>(size);
}

[[nodiscard]] inline qsizetype checked_qt_size(const std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
        throw std::length_error("native path exceeds Qt's addressable string size");
    }
    return static_cast<qsizetype>(size);
}

/// Projects a Qt-owned filesystem path without routing reopen identity through
/// UTF-8. Windows supplies exact UTF-16 code units; Unix supplies QFile's
/// native byte representation. The UTF-8 field is descriptive only.
[[nodiscard]] inline shadow::desktop::FfiNativePath path(const QString& value) {
    shadow::desktop::FfiNativePath result;
    result.display_path = value.toUtf8().toStdString();
#if defined(Q_OS_WIN)
    result.platform = shadow::desktop::FfiNativePathPlatform::Windows;
    const auto size = checked_size(value.size());
    result.windows_units.reserve(size);
    const char16_t* const units = value.utf16();
    for (std::size_t index = 0; index < size; ++index) {
        result.windows_units.push_back(static_cast<std::uint16_t>(units[index]));
    }
#else
#if defined(Q_OS_MACOS)
    result.platform = shadow::desktop::FfiNativePathPlatform::MacOs;
#else
    result.platform = shadow::desktop::FfiNativePathPlatform::OtherUnix;
#endif
    const QByteArray bytes = QFile::encodeName(value);
    const auto size = checked_size(bytes.size());
    result.unix_bytes.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        result.unix_bytes.push_back(
            static_cast<std::uint8_t>(
                static_cast<unsigned char>(bytes[static_cast<qsizetype>(index)])
            )
        );
    }
#endif
    return result;
}

/// Reconstructs a Qt path from the native payload. The display string is
/// deliberately ignored so durable export destinations cannot reopen through
/// presentation text.
[[nodiscard]] inline QString qstring(const shadow::desktop::FfiNativePath& value) {
#if defined(Q_OS_WIN)
    if (value.platform != shadow::desktop::FfiNativePathPlatform::Windows) {
        throw std::invalid_argument("foreign native path cannot be reopened on Windows");
    }
#elif defined(Q_OS_MACOS)
    if (value.platform != shadow::desktop::FfiNativePathPlatform::MacOs) {
        throw std::invalid_argument("foreign native path cannot be reopened on macOS");
    }
#else
    if (value.platform != shadow::desktop::FfiNativePathPlatform::OtherUnix) {
        throw std::invalid_argument("foreign native path cannot be reopened on this Unix host");
    }
#endif
    switch (value.platform) {
    case shadow::desktop::FfiNativePathPlatform::Windows:
        if (!value.unix_bytes.empty()) {
            throw std::invalid_argument("Windows native path contains Unix bytes");
        }
        return QString::fromUtf16(
            reinterpret_cast<const char16_t*>(value.windows_units.data()),
            checked_qt_size(value.windows_units.size())
        );
    case shadow::desktop::FfiNativePathPlatform::MacOs:
    case shadow::desktop::FfiNativePathPlatform::OtherUnix: {
        if (!value.windows_units.empty()) {
            throw std::invalid_argument("Unix native path contains Windows UTF-16 units");
        }
        const QByteArray bytes = QByteArray::fromRawData(
            reinterpret_cast<const char*>(value.unix_bytes.data()),
            checked_qt_size(value.unix_bytes.size())
        );
        return QFile::decodeName(bytes);
    }
    }
    throw std::invalid_argument("unknown native path platform");
}

} // namespace native_path_input
