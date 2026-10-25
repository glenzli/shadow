#pragma once
#include "storage_profile_registry.hpp"

// Recovery chooser before any Catalog/cache/settings service is constructed.
// Selection is always deliberate; missing storage never falls back to Daily.
std::optional<StorageProfileInfo> chooseStorageProfileAtStartup(
    StorageProfileRegistry& registry,
    const QString& context,
    const QString& error
);
