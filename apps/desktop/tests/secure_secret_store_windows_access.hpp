#pragma once

#include <QtCore/qt_windows.h>

#include <aclapi.h>
#include <cstdint>
#include <memory>
#include <vector>

// Inspect the persisted Windows ACL independently of the store's SDDL writer.
// QFile's Unix-style group flags do not describe which Windows principals can read it.
inline bool hasPrivateWindowsCredentialAccess(const QString& path) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(
            const_cast<wchar_t*>(reinterpret_cast<const wchar_t*>(path.utf16())),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner,
            nullptr,
            &dacl,
            nullptr,
            &descriptor
        )
        != ERROR_SUCCESS)
        return false;
    const std::unique_ptr<void, decltype(&LocalFree)> descriptor_owner(descriptor, &LocalFree);
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision)
        || !(control & SE_DACL_PROTECTED) || !owner || !dacl || dacl->AceCount != 2)
        return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD required = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    if (required < sizeof(TOKEN_USER) || required > 65'536) {
        CloseHandle(token);
        return false;
    }
    std::vector<std::uintptr_t> user(
        (required + sizeof(std::uintptr_t) - 1) / sizeof(std::uintptr_t)
    );
    const bool read_user = GetTokenInformation(token, TokenUser, user.data(), required, &required);
    CloseHandle(token);
    if (!read_user || !EqualSid(owner, reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid))
        return false;
    bool current_user_allowed = false, system_allowed = false;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* raw = nullptr;
        if (!GetAce(dacl, index, &raw))
            return false;
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Mask != FILE_ALL_ACCESS)
            return false;
        const auto sid = const_cast<DWORD*>(&ace->SidStart);
        if (EqualSid(sid, owner))
            current_user_allowed = true;
        else if (IsWellKnownSid(sid, WinLocalSystemSid))
            system_allowed = true;
        else
            return false;
    }
    return current_user_allowed && system_allowed;
}
