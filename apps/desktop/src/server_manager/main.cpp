#include "library_server_controller.hpp"
#include "library_server_host.hpp"
#include "secure_secret_store.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QLockFile>
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

[[nodiscard]] QString sharedApplicationDataRoot() {
    QString override = qEnvironmentVariable("SHADOW_SERVER_MANAGER_DATA_ROOT").trimmed();
    if (!override.isEmpty()) {
        return override;
    }
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("Shadow/Shadow"));
}

[[nodiscard]] std::unique_ptr<QSettings> sharedServerSettings(const QString& data_root) {
    if (qEnvironmentVariableIsSet("SHADOW_SERVER_MANAGER_DATA_ROOT")) {
        return std::make_unique<QSettings>(
            QDir(data_root).filePath(QStringLiteral("server-manager-smoke.ini")),
            QSettings::IniFormat
        );
    }
    return std::make_unique<QSettings>(
        QSettings::NativeFormat,
        QSettings::UserScope,
        QStringLiteral("Shadow"),
        QStringLiteral("Shadow")
    );
}

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

    const QString decode_helper_path = QDir(QCoreApplication::applicationDirPath())
                                           .filePath(QStringLiteral("shadow-image-decode-helper"));
    qputenv("SHADOW_DISABLE_PRIVATE_DECODER", QByteArrayLiteral("1"));
    if (QFileInfo(decode_helper_path).isExecutable()) {
        qputenv("SHADOW_DECODE_HELPER_PATH", decode_helper_path.toUtf8());
    }

    const QString data_root = sharedApplicationDataRoot();
    QDir().mkpath(data_root);
    QLockFile manager_lock(QDir(data_root).filePath(QStringLiteral("server-manager.lock")));
    manager_lock.setStaleLockTime(0);
    if (!manager_lock.tryLock(100)) {
        if (!qEnvironmentVariableIsSet("SHADOW_SERVER_MANAGER_SMOKE_TEST")) {
            QMessageBox::information(
                nullptr,
                QObject::tr("Shadow Server is already running"),
                QObject::tr("Use the existing Shadow Server window to manage Library sharing.")
            );
        }
        return EXIT_FAILURE;
    }
    LibraryServerHost host(QDir(data_root).filePath(QStringLiteral("library-server")));
    const bool smoke_test = qEnvironmentVariableIsSet("SHADOW_SERVER_MANAGER_SMOKE_TEST");
    LibraryServerController controller(
        host.operations(),
        sharedServerSettings(data_root),
        smoke_test ? makeVolatileSecretStore() : makeSystemSecretStore()
    );

    QTranslator translator;
    installSystemTranslation(application, translator);

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
