#include "../ops/elevated_transfer.h"
#include <shlobj.h>
#include <shellapi.h>
#include <aclapi.h>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
bool CheckDeniedDirectoryProbe(const std::filesystem::path& directory) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> storage(bytes);
    const BOOL queried = GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes);
    CloseHandle(token);
    if (!queried) return false;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL original = nullptr;
    if (GetNamedSecurityInfoW(directory.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                             nullptr, nullptr, &original, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    struct RestoreAcl {
        const std::filesystem::path& directory;
        PSECURITY_DESCRIPTOR descriptor;
        PACL original;
        ~RestoreAcl() {
            SetNamedSecurityInfoW(const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION, nullptr, nullptr, original, nullptr);
            LocalFree(descriptor);
        }
    } restore{directory, descriptor, original};
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
    entry.grfAccessMode = DENY_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid);
    PACL denied = nullptr;
    if (SetEntriesInAclW(1, &entry, original, &denied) != ERROR_SUCCESS) return false;
    const DWORD applied = SetNamedSecurityInfoW(const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, denied, nullptr);
    LocalFree(denied);
    if (applied != ERROR_SUCCESS) return false;
    return pulse::ops::TargetNeedsElevation(directory.wstring()) &&
           pulse::ops::TargetNeedsElevation((directory / L"absent" / L"nested").wstring());
}
}

int wmain() {
    using namespace pulse::ops;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failures;
    };
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) { CoUninitialize(); return 2; }
    const auto root = std::filesystem::path(temporary) /
        (L"pulse_shell_transfer_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    try {
        const auto source = root / L"source";
        const auto copied = root / L"copied";
        const auto moved = root / L"moved";
        std::filesystem::create_directories(source);
        std::filesystem::create_directories(copied);
        std::filesystem::create_directories(moved);
        const auto denied = root / L"probe_only";
        std::filesystem::create_directories(denied);
        check(CheckDeniedDirectoryProbe(denied), "read-only access probe detects denied directory and missing descendants");
        const auto file = source / L"sample.txt";
        { std::ofstream output(file); output << "shell transfer regression"; }
        check(!TargetNeedsElevation(copied.wstring()), "writable temporary destination probe");
        check(!NeedsShellTransfer({file.wstring()}, moved.wstring(), true), "ordinary move access probe");
        std::atomic<bool> cancel{true};
        auto result = TransferWithShell({file.wstring()}, copied.wstring(), false, nullptr, cancel);
        check(result.cancelled && result.sources.empty() && !std::filesystem::exists(copied / file.filename()), "pre-cancel executes nothing");
        cancel = false;
        result = TransferWithShell({file.wstring()}, copied.wstring(), false, nullptr, cancel);
        check(SUCCEEDED(result.hr) && !result.cancelled && result.sources.size() == 1 &&
              result.destinations.size() == 1 && std::filesystem::exists(file) &&
              std::filesystem::exists(copied / file.filename()), "copy reports actual successful paths");
        result = TransferWithShell({file.wstring()}, copied.wstring(), false, nullptr, cancel,
                                   ShellCollisionPolicy::KeepBoth);
        check(SUCCEEDED(result.hr) && result.destinations.size() == 1 &&
              std::filesystem::path(result.destinations.front()).filename() != file.filename() &&
              std::filesystem::exists(result.destinations.front()) && !result.undo_safe,
              "keep-both reports renamed destination without overwrite");
        result = TransferWithShell({file.wstring()}, moved.wstring(), false, nullptr, cancel,
            ShellCollisionPolicy::System, [&](const std::wstring&, float) { cancel = true; });
        check(result.cancelled && result.sources.empty() && !std::filesystem::exists(moved / file.filename()),
              "progress cancellation prevents transfer");
        cancel = false;
        result = TransferWithShell({file.wstring()}, moved.wstring(), true, nullptr, cancel);
        const bool move_ok = SUCCEEDED(result.hr) && !result.cancelled && result.sources.size() == 1 &&
            result.destinations.size() == 1 && !std::filesystem::exists(file) &&
            std::filesystem::exists(moved / file.filename());
        if (!move_ok) {
            std::cerr << "move diagnostics: hr=0x" << std::hex << static_cast<unsigned long>(result.hr)
                      << std::dec << " cancelled=" << result.cancelled << " mutated=" << result.mutated
                      << " sources=" << result.sources.size() << " destinations=" << result.destinations.size()
                      << " source_exists=" << std::filesystem::exists(file)
                      << " destination_exists=" << std::filesystem::exists(moved / file.filename()) << '\n';
        }
        check(move_ok, "move reports actual successful paths");
        check((ShellTransferFlags(ShellCollisionPolicy::System) & FOF_NOCONFIRMATION) == 0,
              "system collisions never silently overwrite");
        check((ShellTransferFlags(ShellCollisionPolicy::System) & FOFX_SHOWELEVATIONPROMPT) != 0,
              "shell elevation prompt enabled");
        check((ShellTransferFlags(ShellCollisionPolicy::KeepBoth) & FOF_RENAMEONCOLLISION) != 0,
              "keep-both requests collision rename");
        check((ShellTransferFlags(ShellCollisionPolicy::Replace) & FOF_NOCONFIRMATION) != 0,
              "explicit replace preserves request policy");
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        ++failures;
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
