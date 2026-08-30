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
        if (!expect(!preferences.peopleAnalysisConsentDecided())
            || !expect(!preferences.peopleAnalysisExecutionAllowed())
            || !expect(!preferences.imageCompletionExecutionAllowed())
            || !expect(preferences.peopleAnalysisConsentState() == QStringLiteral("not_asked"))) {
            return EXIT_FAILURE;
        }
        preferences.grantPeopleAnalysisConsent();
        preferences.setRawDenoiseExecutionAllowed(false);
        preferences.setSubjectMaskExecutionAllowed(false);
        preferences.setImageCompletionExecutionAllowed(true);
        preferences.setImageUnderstandingExecutionAllowed(false);
        preferences.setImageUnderstandingBackgroundEnabled(true);
        preferences.setImageUnderstandingScanScope(QStringLiteral("liked_or_minimum_rating"));
        preferences.setImageUnderstandingMinimumRating(4);
        preferences.setImageUnderstandingAutoApplyKeywords(true);
        preferences.setRawDenoiseDefaultAmount(68);
        if (!expect(!preferences.rawDenoiseExecutionAllowed())
            || !expect(!preferences.subjectMaskExecutionAllowed())
            || !expect(preferences.imageCompletionExecutionAllowed())
            || !expect(preferences.peopleAnalysisConsentDecided())
            || !expect(preferences.peopleAnalysisExecutionAllowed())
            || !expect(!preferences.imageUnderstandingExecutionAllowed())
            || !expect(preferences.imageUnderstandingBackgroundEnabled())
            || !expect(
                preferences.imageUnderstandingScanScope()
                == QStringLiteral("liked_or_minimum_rating")
            )
            || !expect(preferences.imageUnderstandingMinimumRating() == 4)
            || !expect(preferences.imageUnderstandingAutoApplyKeywords())
            || !expect(preferences.rawDenoiseDefaultAmount() == 68)
            || !expect(preferences.modelStoragePath().endsWith(QStringLiteral("models")))
            || !expect(QDir(preferences.modelStoragePath()).exists())
            || !expect(preferences.modelStorageUrl().isLocalFile())) {
            return EXIT_FAILURE;
        }
    }

    AiPreferences reopened(application_data, settings_path);
    if (!expect(reopened.peopleAnalysisExecutionAllowed())) {
        return EXIT_FAILURE;
    }
    reopened.revokePeopleAnalysisConsent();
    reopened.setRawDenoiseDefaultAmount(500);
    reopened.setImageUnderstandingMinimumRating(500);
    reopened.setImageUnderstandingScanScope(QStringLiteral("invalid"));
    return expect(!reopened.rawDenoiseExecutionAllowed())
                   && expect(!reopened.subjectMaskExecutionAllowed())
                   && expect(reopened.imageCompletionExecutionAllowed())
                   && expect(reopened.peopleAnalysisConsentDecided())
                   && expect(!reopened.peopleAnalysisExecutionAllowed())
                   && expect(reopened.peopleAnalysisConsentState() == QStringLiteral("denied"))
                   && expect(!reopened.imageUnderstandingExecutionAllowed())
                   && expect(reopened.imageUnderstandingBackgroundEnabled())
                   && expect(reopened.imageUnderstandingScanScope() == QStringLiteral("liked"))
                   && expect(reopened.imageUnderstandingMinimumRating() == 5)
                   && expect(reopened.imageUnderstandingAutoApplyKeywords())
                   && expect(reopened.rawDenoiseDefaultAmount() == 100)
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
