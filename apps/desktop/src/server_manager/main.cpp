#include "library_server_controller.hpp"
#include "library_server_host.hpp"
#include "secure_secret_store.hpp"
#include "storage_profile.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTimer>
#include <QTranslator>
#include <QVariant>

#include <algorithm>
#include <memory>

namespace {

[[nodiscard]] bool prefersDarkAppearance(const QApplication& application) {
    return application.styleHints()->colorScheme() != Qt::ColorScheme::Light;
}

void installSystemTranslation(QApplication& application, QTranslator& translator) {
    const QStringList languages = QLocale::system().uiLanguages();
    const bool simplified_chinese =
        std::any_of(languages.cbegin(), languages.cend(), [](const QString& language) {
            return language.startsWith(QStringLiteral("zh"), Qt::CaseInsensitive);
        });
    if (simplified_chinese && translator.load(QStringLiteral(":/i18n/shadow_zh_CN.qm"))) {
        application.installTranslator(&translator);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Shadow"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("shadow.dev"));
    QCoreApplication::setApplicationName(QStringLiteral("Shadow Server"));
    QTranslator translator;
    installSystemTranslation(application, translator);

    const QString decode_helper_path = QDir(QCoreApplication::applicationDirPath())
                                           .filePath(QStringLiteral("shadow-image-decode-helper"));
    qputenv("SHADOW_DISABLE_PRIVATE_DECODER", QByteArrayLiteral("1"));
    if (QFileInfo(decode_helper_path).isExecutable()) {
        qputenv("SHADOW_DECODE_HELPER_PATH", decode_helper_path.toUtf8());
    }

    StorageProfileLaunch launch;
    launch.arguments = QCoreApplication::arguments();
    launch.daily_root = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                            .filePath(QStringLiteral("Shadow/Shadow"));
    launch.override_root = qEnvironmentVariable("SHADOW_SERVER_MANAGER_DATA_ROOT");
    launch.override_root_set = qEnvironmentVariableIsSet("SHADOW_SERVER_MANAGER_DATA_ROOT");
    launch.process = StorageProfileProcess::LibraryServer;
#ifndef NDEBUG
    launch.debug_build = true;
#endif
    QString profile_error;
    auto storage_profile = StorageProfile::open(launch, &profile_error);
    if (!storage_profile) {
        if (!qEnvironmentVariableIsSet("SHADOW_SERVER_MANAGER_SMOKE_TEST")) {
            QMessageBox::information(nullptr, QObject::tr("Could not open library"), profile_error);
        }
        qCritical().noquote() << "Cannot open server storage profile:" << profile_error;
        return EXIT_FAILURE;
    }
    const QString data_root = storage_profile->root();
    qInfo().noquote() << "Server storage profile:" << storage_profile->kind()
                      << storage_profile->id() << data_root;
    LibraryServerHost host(QDir(data_root).filePath(QStringLiteral("library-server")));
    const bool smoke_test = qEnvironmentVariableIsSet("SHADOW_SERVER_MANAGER_SMOKE_TEST");
    LibraryServerController controller(
        host.operations(),
        std::make_unique<QSettings>(storage_profile->settingsFile(), QSettings::IniFormat),
        smoke_test ? makeVolatileSecretStore()
                   : makeLocalSecretStore(
                         QDir(data_root).filePath(QStringLiteral("library-server-credentials.ini"))
                     )
    );

    QQmlApplicationEngine engine;
    engine.setInitialProperties({
        {
            QStringLiteral("libraryServerController"),
            QVariant::fromValue(&controller),
        },
        {
            QStringLiteral("initialDarkAppearance"),
            prefersDarkAppearance(application),
        },
        {QStringLiteral("storageProfileKind"), storage_profile->kind()},
        {QStringLiteral("storageProfileName"), storage_profile->info().name()},
    });
    engine.loadFromModule("Shadow.ServerManager", "ServerManagerMain");
    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }
    if (smoke_test) {
        QTimer::singleShot(250, &application, &QCoreApplication::quit);
    }
    return application.exec();
}
