#include "secure_secret_store.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <array>
#include <memory>

namespace {

template<typename Value>
class CfOwner final {
public:
    explicit CfOwner(Value value = nullptr) noexcept : value_(value) {}

    ~CfOwner() {
        if (value_ != nullptr) {
            CFRelease(value_);
        }
    }

    CfOwner(const CfOwner&) = delete;
    CfOwner& operator=(const CfOwner&) = delete;

    [[nodiscard]] Value get() const noexcept {
        return value_;
    }

    [[nodiscard]] Value release() noexcept {
        const Value value = value_;
        value_ = nullptr;
        return value;
    }

private:
    Value value_;
};

[[nodiscard]] CfOwner<CFStringRef> makeString(const QString& value) {
    return CfOwner<CFStringRef>{CFStringCreateWithCharacters(
        kCFAllocatorDefault,
        reinterpret_cast<const UniChar*>(value.utf16()),
        static_cast<CFIndex>(value.size())
    )};
}

[[nodiscard]] CfOwner<CFDataRef> makeData(const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    return CfOwner<CFDataRef>{CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(utf8.constData()),
        static_cast<CFIndex>(utf8.size())
    )};
}

[[nodiscard]] QString statusMessage(const OSStatus status) {
    CfOwner<CFStringRef> message{SecCopyErrorMessageString(status, nullptr)};
    if (message.get() == nullptr) {
        return QStringLiteral("macOS Keychain error %1").arg(status);
    }
    const CFIndex length = CFStringGetLength(message.get());
    QString result(static_cast<qsizetype>(length), Qt::Uninitialized);
    CFStringGetCharacters(
        message.get(),
        CFRangeMake(0, length),
        reinterpret_cast<UniChar*>(result.data())
    );
    return result;
}

[[nodiscard]] SecretStoreResult resultForStatus(const OSStatus status) {
    if (status == errSecSuccess) {
        return {.status = SecretStoreStatus::Success};
    }
    if (status == errSecItemNotFound) {
        return {.status = SecretStoreStatus::NotFound};
    }
    if (status == errSecNotAvailable || status == errSecInteractionNotAllowed) {
        return {
            .status = SecretStoreStatus::Unavailable,
            .diagnostic = statusMessage(status),
        };
    }
    return {
        .status = SecretStoreStatus::Failure,
        .diagnostic = statusMessage(status),
    };
}

[[nodiscard]] CfOwner<CFDictionaryRef> makeIdentityQuery(
    CFStringRef service,
    CFStringRef account
) {
    std::array<const void*, 3> keys{
        kSecClass,
        kSecAttrService,
        kSecAttrAccount,
    };
    std::array<const void*, 3> values{
        kSecClassGenericPassword,
        service,
        account,
    };
    return CfOwner<CFDictionaryRef>{CFDictionaryCreate(
        kCFAllocatorDefault,
        keys.data(),
        values.data(),
        static_cast<CFIndex>(keys.size()),
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    )};
}

class MacKeychainSecretStore final : public SecretStore {
public:
    [[nodiscard]] bool available() const noexcept override {
        return true;
    }

    [[nodiscard]] SecretStoreResult read(
        const QString& service,
        const QString& account
    ) const override {
        const auto service_value = makeString(service);
        const auto account_value = makeString(account);
        std::array<const void*, 5> keys{
            kSecClass,
            kSecAttrService,
            kSecAttrAccount,
            kSecReturnData,
            kSecMatchLimit,
        };
        std::array<const void*, 5> values{
            kSecClassGenericPassword,
            service_value.get(),
            account_value.get(),
            kCFBooleanTrue,
            kSecMatchLimitOne,
        };
        CfOwner<CFDictionaryRef> query{CFDictionaryCreate(
            kCFAllocatorDefault,
            keys.data(),
            values.data(),
            static_cast<CFIndex>(keys.size()),
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks
        )};

        CFTypeRef raw_result = nullptr;
        const OSStatus status = SecItemCopyMatching(query.get(), &raw_result);
        if (status != errSecSuccess) {
            return resultForStatus(status);
        }
        CfOwner<CFTypeRef> result{raw_result};
        if (CFGetTypeID(result.get()) != CFDataGetTypeID()) {
            return {
                .status = SecretStoreStatus::Failure,
                .diagnostic = QStringLiteral(
                    "macOS Keychain returned an unexpected value type."
                ),
            };
        }
        const auto data = static_cast<CFDataRef>(result.get());
        return {
            .status = SecretStoreStatus::Success,
            .value = QString::fromUtf8(
                reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                static_cast<qsizetype>(CFDataGetLength(data))
            ),
        };
    }

    [[nodiscard]] SecretStoreResult write(
        const QString& service,
        const QString& account,
        const QString& value
    ) override {
        const auto service_value = makeString(service);
        const auto account_value = makeString(account);
        const auto secret_data = makeData(value);
        const auto query = makeIdentityQuery(
            service_value.get(),
            account_value.get()
        );
        std::array<const void*, 1> update_keys{kSecValueData};
        std::array<const void*, 1> update_values{secret_data.get()};
        CfOwner<CFDictionaryRef> updates{CFDictionaryCreate(
            kCFAllocatorDefault,
            update_keys.data(),
            update_values.data(),
            static_cast<CFIndex>(update_keys.size()),
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks
        )};
        OSStatus status = SecItemUpdate(query.get(), updates.get());
        if (status != errSecItemNotFound) {
            return resultForStatus(status);
        }

        std::array<const void*, 5> add_keys{
            kSecClass,
            kSecAttrService,
            kSecAttrAccount,
            kSecValueData,
            kSecAttrAccessible,
        };
        std::array<const void*, 5> add_values{
            kSecClassGenericPassword,
            service_value.get(),
            account_value.get(),
            secret_data.get(),
            kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
        };
        CfOwner<CFDictionaryRef> addition{CFDictionaryCreate(
            kCFAllocatorDefault,
            add_keys.data(),
            add_values.data(),
            static_cast<CFIndex>(add_keys.size()),
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks
        )};
        status = SecItemAdd(addition.get(), nullptr);
        return resultForStatus(status);
    }

    [[nodiscard]] SecretStoreResult remove(
        const QString& service,
        const QString& account
    ) override {
        const auto service_value = makeString(service);
        const auto account_value = makeString(account);
        const auto query = makeIdentityQuery(
            service_value.get(),
            account_value.get()
        );
        return resultForStatus(SecItemDelete(query.get()));
    }
};

} // namespace

std::unique_ptr<SecretStore> makeSystemSecretStore() {
    return std::make_unique<MacKeychainSecretStore>();
}
