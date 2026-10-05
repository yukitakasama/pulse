#include "preview_host_client.h"
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <fcntl.h>
#include <io.h>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
struct Sample {
    uint64_t private_bytes = 0, working_bytes = 0;
};
Sample Memory(HANDLE process) {
    PROCESS_MEMORY_COUNTERS_EX value{};
    value.cb = sizeof(value);
    if (!GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&value), sizeof(value))) return {};
    return {value.PrivateUsage, value.WorkingSetSize};
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || (wcscmp(argv[1], L"--file") && wcscmp(argv[1], L"--full") && wcscmp(argv[1], L"--directory"))) return 2;
    _setmode(_fileno(stdout), _O_U8TEXT);
    const bool full = !wcscmp(argv[1], L"--full");
    std::vector<fs::path> files;
    if (wcscmp(argv[1], L"--directory")) {
        for (int i = 2; i < argc; ++i) files.emplace_back(argv[i]);
    }
    else {
        for (const auto& entry : fs::directory_iterator(argv[2])) {
            const DWORD attrs = GetFileAttributesW(entry.path().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) continue;
            auto ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (ext == L".lnk" || ext == L".exe" || ext == L".dll" || ext == L".bat" || ext == L".ps1" || ext == L".csv") continue;
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
    }
    HANDLE main_thread = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS)) return 2;
    pulse_test::Host host;
    bool started = false;
    int exit_code = 0;
    for (const auto& file : files) {
        if (!started) started = host.Start();
        if (!started) { std::wprintf(L"[FAIL] host start\n"); exit_code = 2; break; }
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, host.ProcessId());
        if (!process) { std::wprintf(L"[FAIL] host memory access\n"); exit_code = 2; break; }
        Sample peak = Memory(process);
        std::mutex mutex;
        std::condition_variable wake;
        bool done = false;
        std::atomic<bool> cancelled = false;
        const auto begin = GetTickCount64();
        std::wprintf(L"[BEGIN] pid=%lu path=%ls\n", host.ProcessId(), file.c_str());
        std::fflush(stdout);
        std::thread monitor([&] {
            std::unique_lock lock(mutex);
            while (!wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return done; })) {
                const auto sample = Memory(process);
                peak.private_bytes = std::max(peak.private_bytes, sample.private_bytes);
                peak.working_bytes = std::max(peak.working_bytes, sample.working_bytes);
                if (GetTickCount64() - begin >= 8000 || sample.private_bytes >= 768ull * 1024 * 1024) {
                    cancelled = true;
                    CancelSynchronousIo(main_thread);
                    // This process belongs to the probe, never to the running app.
                    const HANDLE kill = OpenProcess(PROCESS_TERMINATE, FALSE, host.ProcessId());
                    if (kill) { TerminateProcess(kill, 124); CloseHandle(kill); }
                    break;
                }
            }
        });
        pulse_test::Result result;
        const bool ok = host.Request(file.wstring(), result, MAXDWORD, full ? 1024u : 256u,
            pulse::ipc::PreviewRequestKind::Content, full ? 0u : pulse::ipc::kPreviewRequestFlagGrid);
        { std::lock_guard lock(mutex); done = true; }
        wake.notify_one(); monitor.join();
        const auto end = Memory(process);
        peak.private_bytes = std::max(peak.private_bytes, end.private_bytes);
        peak.working_bytes = std::max(peak.working_bytes, end.working_bytes);
        std::wprintf(L"[RESULT] ok=%d cancelled=%d status=%d kind=%u elapsed=%llu private_peak=%llu working_peak=%llu private_end=%llu path=%ls error=%ls\n",
            ok, cancelled.load(), result.response.status, static_cast<unsigned>(result.response.kind),
            static_cast<unsigned long long>(GetTickCount64() - begin),
            static_cast<unsigned long long>(peak.private_bytes), static_cast<unsigned long long>(peak.working_bytes),
            static_cast<unsigned long long>(end.private_bytes), file.c_str(), result.error.c_str());
        std::fflush(stdout);
        CloseHandle(process);
        if (!ok) { host.Stop(); started = false; }
    }
    host.Stop(); CloseHandle(main_thread);
    return exit_code;
}
