#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

struct BackendLibraryServerConfig final {
    QString bind_address;
    QString authorization;
    QString display_name;
    QStringList share_roots;
    bool serves_originals = true;
};

struct BackendLibraryServerSnapshot final {
    bool running = false;
    QString local_address;
    QString display_name;
    QString provider_mode;
    std::uint64_t photo_count = 0;
    std::uint64_t cache_byte_len = 0;
    std::uint64_t shared_root_count = 0;
    bool serves_originals = true;
};
