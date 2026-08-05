#include "secure_secret_store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtGlobal>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Secure secret-store contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main() {
    auto volatile_store = makeVolatileSecretStore();
    const QString service = QStringLiteral("dev.shadow.photo.test");
    const QString account = QStringLiteral("contract");
    const QString secret = QStringLiteral("not-a-real-credential");

    if (!require(
            volatile_store != nullptr && volatile_store->available(),
            "the isolated store is available in-process"
        )
        || !require(
            volatile_store->read(service, account).status == SecretStoreStatus::NotFound,
            "an unknown identity is absent"
        )
        || !require(
            volatile_store->write(service, account, secret).succeeded(),
            "an isolated secret can be written"
        )
        || !require(
            volatile_store->read(service, account).value == secret,
            "an isolated secret is read only through the native interface"
        )
        || !require(
            volatile_store->remove(service, account).succeeded()
                && volatile_store->read(service, account).status == SecretStoreStatus::NotFound,
            "removal clears the isolated identity"
        )) {
        return EXIT_FAILURE;
    }

    QTemporaryDir fixture;
    if (!require(fixture.isValid(), "the local-store fixture is available")) {
        return EXIT_FAILURE;
    }
    const QString local_path = QDir(fixture.path()).filePath(QStringLiteral("credentials.ini"));
    auto local_store = makeLocalSecretStore(local_path);
    if (!require(local_store != nullptr && local_store->available(), "local storage is available")
        || !require(
            local_store->write(service, account, secret).succeeded(),
            "a local credential can be written"
        )) {
        return EXIT_FAILURE;
    }
    local_store.reset();
    local_store = makeLocalSecretStore(local_path);
    if (!require(
            local_store->read(service, account).value == secret,
            "a local credential survives a store restart"
        )) {
        return EXIT_FAILURE;
    }
    const QFileDevice::Permissions permissions = QFileInfo(local_path).permissions();
    return require(
               permissions.testFlag(QFileDevice::ReadOwner)
                   && permissions.testFlag(QFileDevice::WriteOwner)
                   && !permissions.testFlag(QFileDevice::ReadGroup)
                   && !permissions.testFlag(QFileDevice::ReadOther),
               "the local credential file is private to its user"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
