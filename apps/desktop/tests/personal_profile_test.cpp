#include "personal_profile.hpp"

#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Personal profile contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    if (!require(root.isValid(), "temporary profile root is available")) {
        return EXIT_FAILURE;
    }
    const QString settings = root.filePath(QStringLiteral("preferences.ini"));
    const QString source = root.filePath(QStringLiteral("source.png"));
    QImage image(32, 20, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::cyan);
    if (!require(image.save(source), "avatar fixture is written")) {
        return EXIT_FAILURE;
    }

    {
        PersonalProfile profile(root.path(), settings);
        profile.setNickname(QStringLiteral("  Glendon  "));
        profile.setHomeLocality(
            QStringLiteral("CN\u001fShanghai\u001fShanghai"),
            QStringLiteral("Shanghai · China")
        );
        if (!require(profile.nickname() == QStringLiteral("Glendon"), "nickname is normalized")
            || !require(
                profile.homeLocalityKey() == QStringLiteral("cn\u001fshanghai\u001fshanghai"),
                "home identity is provider-independent and normalized"
            )
            || !require(profile.homeConfigured(), "home configuration becomes active")
            || !require(
                profile.importAvatar(QUrl::fromLocalFile(source)),
                "avatar is normalized into local profile storage"
            )
            || !require(profile.avatarUrl().isLocalFile(), "avatar publishes a local URL")) {
            return EXIT_FAILURE;
        }
    }

    PersonalProfile reopened(root.path(), settings);
    if (!require(reopened.nickname() == QStringLiteral("Glendon"), "nickname persists")
        || !require(reopened.homeConfigured(), "home locality persists")
        || !require(!reopened.avatarUrl().isEmpty(), "normalized avatar persists")) {
        return EXIT_FAILURE;
    }
    reopened.clearHomeLocality();
    reopened.clearAvatar();
    return require(!reopened.homeConfigured(), "home locality can be cleared")
                   && require(reopened.avatarUrl().isEmpty(), "avatar can be cleared")
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
