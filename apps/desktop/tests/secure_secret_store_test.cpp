#include "secure_secret_store.hpp"

#include <QtGlobal>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Secure secret-store contract failed: "
                  << message << '\n';
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
            volatile_store->read(service, account).status
                == SecretStoreStatus::NotFound,
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
                && volatile_store->read(service, account).status
                    == SecretStoreStatus::NotFound,
            "removal clears the isolated identity"
        )) {
        return EXIT_FAILURE;
    }

    const auto system_store = makeSystemSecretStore();
    if (!require(system_store != nullptr, "the platform factory is total")) {
        return EXIT_FAILURE;
    }
#if defined(Q_OS_MACOS)
    return require(
               system_store->available(),
               "macOS publishes the Keychain-backed implementation"
           )
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
#else
    return require(
               !system_store->available(),
               "unsupported platforms fail closed without plaintext storage"
           )
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
#endif
}
