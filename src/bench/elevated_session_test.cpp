#include "../ops/elevated_transfer_client.h"
#include "../common/path_utils.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
std::string Read(const std::filesystem::path& path) { std::ifstream in(path); return {std::istreambuf_iterator<char>(in), {}}; }
void Write(const std::filesystem::path& path, const char* text) { std::ofstream out(path); out << text; }
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--orphan-child") {
        const std::filesystem::path fixture(argv[2]);
        const auto source = fixture / L"orphan_source", target = fixture / L"orphan_target";
        std::filesystem::create_directories(source); std::filesystem::create_directories(target);
        Write(source / L"item.txt", "orphan fixture");
        std::atomic<bool> cancel{false};
        const auto result = pulse::ops::TransferWithElevatedHelper({(source / L"item.txt").wstring()}, target.wstring(), false, nullptr, cancel);
        if (FAILED(result.hr)) return 3;
        { std::ofstream output(fixture / L"helper.pid"); output << pulse::ops::ElevatedHelperProcessIdForTesting(); }
        const auto started = GetTickCount64();
        while (!std::filesystem::exists(fixture / L"exit_parent") && GetTickCount64() - started < 15000) Sleep(25);
        ExitProcess(0); // Deliberately bypass explicit client/helper shutdown.
    }
    using namespace pulse::ops;
    int failed = 0;
    auto check = [&](bool ok, const char* name) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; if (!ok) ++failed; };
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 2;
    const auto root = std::filesystem::path(temp) / (L"pulse_session_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    std::atomic<bool> cancel{false};
    try {
        const auto source = root / L"source", destination = root / L"destination", moved = root / L"moved";
        std::filesystem::create_directories(source); std::filesystem::create_directories(destination); std::filesystem::create_directories(moved);
        Write(source / L"a.txt", "original");
        ElevatedTransferCallbacks callbacks;
        callbacks.verify_copies = true;
        std::vector<bool> authorization_events;
        callbacks.authorization = [&](bool awaiting) { authorization_events.push_back(awaiting); };
        auto invalid = TransferWithElevatedHelper(std::vector<std::wstring>(4097, (source / L"a.txt").wstring()), destination.wstring(), false, nullptr, cancel);
        check(FAILED(invalid.hr) && !invalid.error.empty() && ElevatedHelperProcessIdForTesting() == 0, "oversized selection rejected before helper launch");
        auto result = TransferWithElevatedHelper({(source / L"a.txt").wstring()}, destination.wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && result.sources.size() == 1 && Read(destination / L"a.txt") == "original", "authenticated helper copy reports exact completion");
        check(authorization_events == std::vector<bool>({true, false}), "new authorization lifecycle reported exactly once");
        const DWORD helper = ElevatedHelperProcessIdForTesting();
        unsigned conflicts = 0;
        callbacks.conflict = [&](const ConflictItemInfo& item) {
            ++conflicts;
            check(pulse::path::EqualInsensitive(pulse::path::StripExtendedPathPrefix(item.source), (source / L"a.txt").wstring()) &&
                  pulse::path::EqualInsensitive(pulse::path::StripExtendedPathPrefix(item.destination), (destination / L"a.txt").wstring()), "conflict names forwarded to Pulse");
            return ElevatedConflictAnswer{ConflictChoice::Skip, false};
        };
        Write(source / L"a.txt", "updated");
        result = TransferWithElevatedHelper({(source / L"a.txt").wstring()}, destination.wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && result.sources.empty() && conflicts == 1 && Read(destination / L"a.txt") == "original", "skip stays skipped with no false completion");
        check(helper != 0 && helper == ElevatedHelperProcessIdForTesting(), "same helper reused across requests");
        callbacks.conflict = [&](const ConflictItemInfo&) { return ElevatedConflictAnswer{ConflictChoice::Replace, false}; };
        result = TransferWithElevatedHelper({(source / L"a.txt").wstring()}, destination.wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && Read(destination / L"a.txt") == "updated", "Pulse replace choice applied in existing engine");
        result = TransferWithElevatedHelper({(source / L"a.txt").wstring()}, moved.wstring(), true, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && result.sources.size() == 1 && !std::filesystem::exists(source / L"a.txt") && Read(moved / L"a.txt") == "updated", "helper move reports actual paths");
        std::filesystem::create_directories(source / L"tree"); std::filesystem::create_directories(destination / L"tree");
        Write(source / L"tree" / L"same.txt", "new"); Write(source / L"tree" / L"fresh.txt", "fresh");
        Write(destination / L"tree" / L"same.txt", "old");
        conflicts = 0;
        callbacks.conflict = [&](const ConflictItemInfo&) { ++conflicts; return ElevatedConflictAnswer{ConflictChoice::KeepBoth, false}; };
        result = TransferWithElevatedHelper({(source / L"tree").wstring()}, destination.wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && conflicts == 1 && Read(destination / L"tree" / L"same.txt") == "old" && Read(destination / L"tree" / L"fresh.txt") == "fresh", "nested merge delegates collision without batch replay");
        bool renamed_new = false;
        for (size_t i = 0; i < result.sources.size(); ++i) {
            if (pulse::path::EqualInsensitive(pulse::path::StripExtendedPathPrefix(result.sources[i]), (source / L"tree" / L"same.txt").wstring()) &&
                !pulse::path::EqualInsensitive(pulse::path::StripExtendedPathPrefix(result.destinations[i]), (destination / L"tree" / L"same.txt").wstring()) &&
                Read(result.destinations[i]) == "new") renamed_new = true;
        }
        check(renamed_new, "nested keep-both returns actual renamed file containing new content");
        bool want_pause = false, saw_pause = false;
        ULONGLONG paused_at = 0;
        const auto pause_test_started = GetTickCount64();
        callbacks.conflict = [&](const ConflictItemInfo&) { want_pause = true; return ElevatedConflictAnswer{ConflictChoice::Replace, false}; };
        callbacks.paused = [&] { return want_pause && GetTickCount64() - pause_test_started < 5000 && (!paused_at || GetTickCount64() - paused_at < 200); };
        callbacks.progress = [&](const OpStatus& status) {
            if (status.phase == OpPhase::Paused) { saw_pause = true; if (!paused_at) paused_at = GetTickCount64(); }
        };
        result = TransferWithElevatedHelper({(source / L"tree" / L"same.txt").wstring()}, (destination / L"tree").wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && saw_pause && Read(destination / L"tree" / L"same.txt") == "new", "pause and resume propagate through authorized session");
        callbacks.paused = {}; callbacks.progress = {};

        callbacks.conflict = [&](const ConflictItemInfo&) { return ElevatedConflictAnswer{ConflictChoice::Cancel, false}; };
        result = TransferWithElevatedHelper({(source / L"tree").wstring()}, destination.wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(result.cancelled && FAILED(result.hr), "cancel choice round-trips without blocking");
        check(helper == ElevatedHelperProcessIdForTesting(), "cancelled request retains authorized session");
        cancel = true;
        result = TransferWithElevatedHelper({(source / L"tree").wstring()}, destination.wstring(), false, nullptr, cancel);
        check(result.cancelled && helper == ElevatedHelperProcessIdForTesting(), "pre-cancel launches no replacement helper");
        cancel = false;
        Write(source / L"delete.txt", "deletion fixture");
        result = DeleteWithElevatedHelper({(source / L"delete.txt").wstring()}, true, nullptr, cancel, callbacks);
        check(SUCCEEDED(result.hr) && result.sources.size() == 1 && result.destinations.empty() && !std::filesystem::exists(source / L"delete.txt"), "permanent delete opcode reports actual deleted source");
        check(helper == ElevatedHelperProcessIdForTesting() && authorization_events == std::vector<bool>({true, false}), "cached helper deletes without repeated authorization state");
        result = DeleteWithElevatedHelper({L"\\\\localhost\\nonexistent_pulse_recycle_test_share\\file.txt"}, false, nullptr, cancel, callbacks);
        check(FAILED(result.hr) && !result.cancelled && !result.mutated && result.sources.empty(), "recycle opcode refuses unsupported location without permanent fallback");
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, helper);
        ShutdownElevatedTransferHelper();
        check(process && WaitForSingleObject(process, 10000) == WAIT_OBJECT_0, "explicit shutdown ends helper");
        if (process) CloseHandle(process);
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --orphan-child \"" + root.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION child{};
        const bool launched = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE;
        bool orphan_closed = false;
        if (launched) {
            CloseHandle(child.hThread);
            const auto started = GetTickCount64();
            DWORD orphan_pid = 0;
            while (!orphan_pid && GetTickCount64() - started < 15000 && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) {
                { std::ifstream input(root / L"helper.pid"); input >> orphan_pid; }
                if (!orphan_pid) Sleep(25);
            }
            HANDLE orphan = orphan_pid ? OpenProcess(SYNCHRONIZE, FALSE, orphan_pid) : nullptr;
            Write(root / L"exit_parent", "exit");
            orphan_closed = WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0 && orphan && WaitForSingleObject(orphan, 10000) == WAIT_OBJECT_0;
            if (orphan) CloseHandle(orphan);
            CloseHandle(child.hProcess);
        }
        check(orphan_closed, "parent process exit ends its retained helper");
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n'; ++failed;
        ShutdownElevatedTransferHelper();
        std::error_code ignored; std::filesystem::remove_all(root, ignored);
    }
    return failed ? 1 : 0;
}
