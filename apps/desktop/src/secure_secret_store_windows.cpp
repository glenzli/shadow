#include "secure_secret_store_windows.hpp"

#include <QtCore/qt_windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {
struct CloseHandleDeleter {
    void operator()(void* handle) const noexcept {
        CloseHandle(handle);
    }
};
using OwnedHandle = std::unique_ptr<void, CloseHandleDeleter>;
using LocalAllocation = std::unique_ptr<void, decltype(&LocalFree)>;
} // namespace

bool restrictWindowsCredentialFile(const QString& path) {
    HANDLE opened = CreateFileW(
        reinterpret_cast<const wchar_t*>(path.utf16()),
        READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );
    if (opened == INVALID_HANDLE_VALUE)
        return false;
    const OwnedHandle file(opened);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.get(), &information)
        || (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)
        )
        || information.nNumberOfLinks != 1)
        return false;

    HANDLE opened_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &opened_token))
        return false;
    const OwnedHandle token(opened_token);
    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (bytes < sizeof(TOKEN_USER) || bytes > 65'536)
        return false;
    std::vector<std::uintptr_t> user((bytes + sizeof(std::uintptr_t) - 1) / sizeof(std::uintptr_t));
    if (!GetTokenInformation(token.get(), TokenUser, user.data(), bytes, &bytes))
        return false;
    const auto sid = reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(sid, &sid_text))
        return false;
    const LocalAllocation sid_owner(sid_text, &LocalFree);
    const std::wstring sddl = std::wstring(L"D:P(A;;FA;;;") + sid_text + L")(A;;FA;;;SY)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &descriptor,
            nullptr
        ))
        return false;
    const LocalAllocation descriptor_owner(descriptor, &LocalFree);
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present || !dacl)
        return false;
    return SetSecurityInfo(
               file.get(),
               SE_FILE_OBJECT,
               OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION
                   | PROTECTED_DACL_SECURITY_INFORMATION,
               sid,
               nullptr,
               dacl,
               nullptr
           )
           == ERROR_SUCCESS;
}
