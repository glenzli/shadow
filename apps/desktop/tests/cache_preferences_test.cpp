#include "cache_preferences.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstdlib>

namespace {

[[nodiscard]] bool expect(const bool condition) {
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) {
        return EXIT_FAILURE;
    }
    const QString settings_path = root.filePath(QStringLiteral("preferences.ini"));
    const QString cache_root = root.filePath(QStringLiteral("cache"));

    {
        CachePreferences preferences(cache_root, settings_path);
        preferences.setDiskLimitGiB(64);
        preferences.setAutomaticCleanupAllowed(true);
        if (!expect(preferences.diskLimitGiB() == 64)
            || !expect(preferences.diskLimitBytes() == 64ULL * 1024ULL * 1024ULL * 1024ULL)
            || !expect(preferences.automaticCleanupAllowed())
            || !expect(preferences.cacheRootPath() == cache_root)
            || !expect(preferences.cacheRootUrl().isLocalFile())) {
            return EXIT_FAILURE;
        }
    }

    CachePreferences reopened(cache_root, settings_path);
    if (!expect(reopened.diskLimitGiB() == 64) || !expect(reopened.automaticCleanupAllowed())) {
        return EXIT_FAILURE;
    }
    reopened.setDiskLimitGiB(-20);
    return expect(reopened.diskLimitGiB() == 0) && expect(reopened.diskLimitBytes() == 0)
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
