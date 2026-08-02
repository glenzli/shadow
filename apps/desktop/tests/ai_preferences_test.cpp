#include "ai_preferences.hpp"

#include <QCoreApplication>
#include <QDir>
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
    const QString application_data = root.filePath(QStringLiteral("application-data"));

    {
        AiPreferences preferences(application_data, settings_path);
        preferences.setRawDenoiseExecutionAllowed(false);
        preferences.setSubjectMaskExecutionAllowed(false);
        preferences.setRawDenoiseDefaultAmount(68);
        if (!expect(!preferences.rawDenoiseExecutionAllowed())
            || !expect(!preferences.subjectMaskExecutionAllowed())
            || !expect(preferences.rawDenoiseDefaultAmount() == 68)
            || !expect(preferences.modelStoragePath().endsWith(QStringLiteral("models")))
            || !expect(QDir(preferences.modelStoragePath()).exists())
            || !expect(preferences.modelStorageUrl().isLocalFile())) {
            return EXIT_FAILURE;
        }
    }

    AiPreferences reopened(application_data, settings_path);
    reopened.setRawDenoiseDefaultAmount(500);
    return expect(!reopened.rawDenoiseExecutionAllowed())
                   && expect(!reopened.subjectMaskExecutionAllowed())
                   && expect(reopened.rawDenoiseDefaultAmount() == 100)
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
