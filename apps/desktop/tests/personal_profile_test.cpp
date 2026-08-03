#include "personal_profile.hpp"

#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>

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
        const QVariantList living_places{
            QVariantMap{
                {QStringLiteral("key"), QStringLiteral("CN\u001fShanghai\u001fShanghai")},
                {QStringLiteral("label"), QStringLiteral("Shanghai · China")},
                {QStringLiteral("startMonth"), QStringLiteral("2020-01")},
                {QStringLiteral("endMonth"), QString()},
            },
            QVariantMap{
                {QStringLiteral("key"), QStringLiteral("CN\u001fChengdu\u001fChengdu")},
                {QStringLiteral("label"), QStringLiteral("Chengdu · China")},
                {QStringLiteral("startMonth"), QString()},
                {QStringLiteral("endMonth"), QString()},
            },
        };
        if (!require(profile.replaceLivingPlaces(living_places), "living places are accepted")) {
            return EXIT_FAILURE;
        }
        if (!require(profile.nickname() == QStringLiteral("Glendon"), "nickname is normalized")
            || !require(
                profile.livingPlaces().front().toMap().value(QStringLiteral("key")).toString()
                    == QStringLiteral("cn\u001fshanghai\u001fshanghai"),
                "living-place identity is provider-independent and normalized"
            )
            || !require(profile.hasLivingPlaces(), "living-place configuration becomes active")
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
        || !require(reopened.livingPlaces().size() == 2, "living-place periods persist")
        || !require(!reopened.avatarUrl().isEmpty(), "normalized avatar persists")) {
        return EXIT_FAILURE;
    }
    if (!require(reopened.replaceLivingPlaces({}), "living places can be cleared")) {
        return EXIT_FAILURE;
    }
    reopened.clearAvatar();
    return require(!reopened.hasLivingPlaces(), "living places can be cleared")
                   && require(reopened.avatarUrl().isEmpty(), "avatar can be cleared")
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
