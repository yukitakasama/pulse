#ifdef PULSE_WITH_SELFTEST
#include "folder_sizes.h"
#include "../index/index_feed.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstdio>

namespace pulse::app {
namespace {
class FolderSizePipeFixture {
public:
    std::atomic<uint64_t> bytes{800};
    std::atomic<bool> available{true}, stall{false}, disconnect{false}, listening{false};
    std::atomic<unsigned> requests{0}, responses{0};
    explicit FolderSizePipeFixture(const wchar_t* pipe_name = nullptr) {
        wchar_t old[256]{};
        const auto count = GetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", old, 256);
        if (count && count < 256) previous_ = old;
        name_ = pipe_name ? pipe_name : L"\\\\.\\pipe\\PulseFolderSizeTest-" + std::to_wstring(GetCurrentProcessId());
        SetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", name_.c_str());
        thread_ = std::thread([this] { Run(); });
        WaitForSingleObject(ready_, 3000);
    }
    ~FolderSizePipeFixture() {
        SetEvent(stop_); CancelSynchronousIo(thread_.native_handle()); thread_.join();
        CloseHandle(ready_); CloseHandle(stop_);
        SetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", previous_.empty() ? nullptr : previous_.c_str());
    }
private:
    HANDLE ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread_;
    std::wstring previous_, name_;
    static bool Transfer(HANDLE pipe, void* data, DWORD count, bool write) {
        auto* bytes = static_cast<BYTE*>(data);
        while (count) {
            DWORD done = 0;
            const bool ok = (write ? WriteFile(pipe, bytes, count, &done, nullptr) : ReadFile(pipe, bytes, count, &done, nullptr)) != FALSE;
            if (!ok || !done) return false;
            count -= done; bytes += done;
        }
        return true;
    }
    void Run() {
        while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
            HANDLE pipe = CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
            SetEvent(ready_);
            if (pipe == INVALID_HANDLE_VALUE) return;
            listening = true;
            const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
            listening = false;
            while (connected && WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
                ipc::MsgHeader header{};
                if (!Transfer(pipe, &header, sizeof(header), false) || header.magic != index::kFeedMagic ||
                    header.type != index::kFolderSizeRequest || header.payload_size > 256 * 1024) break;
                std::vector<uint8_t> payload(header.payload_size);
                if (!Transfer(pipe, payload.data(), header.payload_size, false)) break;
                ipc::PayloadReader reader(payload.data(), payload.size()); uint32_t version = 0, count = 0;
                if (!reader.GetU32(version) || version != 1 || !reader.GetU32(count) || count > index::kFolderSizeBatch) break;
                std::vector<index::IndexedFolderSize> values;
                for (uint32_t i = 0; i < count; ++i) {
                    std::wstring path;
                    if (!reader.GetString(path)) break;
                    values.push_back({available.load(), path.ends_with(L"\\empty") ? 0 : bytes.load()});
                }
                ++requests;
                if (disconnect) break;
                while (stall && WaitForSingleObject(stop_, 5) != WAIT_OBJECT_0) {}
                if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) break;
                ipc::PayloadWriter writer; index::PutFolderSizes(writer, values);
                header.type = index::kFolderSizeResponse; header.payload_size = static_cast<uint32_t>(writer.data().size());
                if (!Transfer(pipe, &header, sizeof(header), true) ||
                    !Transfer(pipe, const_cast<uint8_t*>(writer.data().data()), header.payload_size, true)) break;
                ++responses;
            }
            DisconnectNamedPipe(pipe); CloseHandle(pipe);
        }
    }
};
class ScanStartGate {
public:
    ScanStartGate() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ScanStartGate() { if (event_) CloseHandle(event_); }
    void Hold(FolderSizes& sizes) {
        sizes.SetCachePath([this] {
            // Cache-path callbacks already run on the scan worker. Hold only
            // its startup until the independent real index request is observed.
            if (event_) WaitForSingleObject(event_, 10000);
            return std::wstring();
        });
    }
    void Release() { if (event_) SetEvent(event_); }
private:
    HANDLE event_;
};
}
int RunFolderSizeFakeIndexServer(const wchar_t* pipe_name) {
    FolderSizePipeFixture pipe(pipe_name);
    std::printf("[READY] isolated folder-size feed; estimate=800; lifetime=30s\n");
    std::fflush(stdout);
    Sleep(30000);
    std::printf("[INFO] fake index requests=%u\n", pipe.requests.load());
    return 0;
}
bool RunFolderSizeIndexClientTest(const std::filesystem::path& fixture, std::ofstream& log) {
    FolderSizePipeFixture pipe;
    bool ok = true;
    auto check = [&](bool passed, const char* label) { log << (passed ? "[PASS] " : "[FAIL] ") << label << std::endl; ok &= passed; };
    auto wait = [](const auto& predicate) {
        const auto until = GetTickCount64() + 5000;
        while (GetTickCount64() < until) { if (predicate()) return true; Sleep(10); }
        return predicate();
    };
    const auto nested = (fixture / L"nested").wstring(), empty = (fixture / L"empty").wstring();
    const auto estimate_root = fixture / L"index-estimate-workload";
    for (unsigned folder = 0; folder < 64; ++folder) {
        const auto directory = estimate_root / std::to_wstring(folder);
        std::filesystem::create_directories(directory);
        for (unsigned file = 0; file < 32; ++file)
            std::ofstream(directory / (std::to_wstring(file) + L".bin"), std::ios::binary) << "thirteen-byte";
    }
    {
        check(wait([&] { return pipe.listening.load(); }), "estimate fixture pipe is listening before client starts");
        ScanStartGate gate;
        FolderSizes sizes;
        gate.Hold(sizes);
        bool estimated = false, invalid_estimate = false;
        sizes.Sync({{estimate_root.wstring()}}, {});
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            const auto value = sizes.Get(estimate_root.wstring());
            if (value.state == FolderSizeState::Indexed) {
                estimated = true;
                invalid_estimate |= value.bytes != 800 || value.source != FolderSizeSource::Index || value.verified;
                gate.Release();
            }
            if (value.state == FolderSizeState::Ready) break;
            Sleep(1);
        }
        gate.Release();
        check(estimated && !invalid_estimate, "observed wrong index estimate is explicitly unverified while real scan runs");
        check(sizes.Get(estimate_root.wstring()).state == FolderSizeState::Ready &&
              sizes.Get(estimate_root.wstring()).bytes == 64 * 32 * 13 &&
              sizes.Get(estimate_root.wstring()).source == FolderSizeSource::Scan,
              "index estimate is automatically replaced by complete scan without clicking");
        sizes.Stop();
    }
    {
        check(wait([&] { return pipe.listening.load(); }), "exact-total fixture pipe is listening before client starts");
        FolderSizes sizes;
        sizes.Sync({{nested}, {empty}}, {});
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Ready; }) && sizes.Get(nested).bytes == 13567 &&
            sizes.Get(nested).source == FolderSizeSource::Scan,
            "automatic scan replaces any indexed estimate with actual logical size");
        check(wait([&] { return sizes.Get(empty).state == FolderSizeState::Ready; }) &&
            sizes.Get(empty).has_value && sizes.Get(empty).bytes == 0,
            "batched requests preserve a scanned empty folder as known zero");
        pipe.bytes = 1200;
        Sleep(1100);
        check(sizes.Get(nested).state == FolderSizeState::Ready && sizes.Get(nested).bytes == 13567,
            "later incorrect index estimates cannot overwrite an automatic scan");
        pipe.available = false;
        sizes.Invalidate(nested);
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Ready; }) && sizes.Get(nested).bytes == 13567,
            "journal gap or unavailable index falls back to actual directory scan");
        pipe.available = true;
        Sleep(1100);
        check(sizes.Get(nested).state == FolderSizeState::Ready && sizes.Get(nested).bytes == 13567,
            "index recovery does not replace a completed exact scan");
        sizes.Calculate(nested);
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Ready; }) && sizes.Get(nested).bytes == 13567,
            "clicking estimate requests exact statistics instead of accepting index exclusions");
        Sleep(1100);
        check(sizes.Get(nested).state == FolderSizeState::Ready && sizes.Get(nested).bytes == 13567,
            "later service polls cannot overwrite manually verified totals");
        sizes.Stop();
    }
    Sleep(20);
    {
        check(wait([&] { return pipe.listening.load(); }), "batch fixture pipe is listening before client starts");
        FolderSizes sizes;
        std::vector<FolderSizeRequest> visible;
        for (unsigned i = 0; i < 140; ++i) {
            const auto path = fixture / (L"batch-" + std::to_wstring(i));
            std::filesystem::create_directories(path);
            std::ofstream(path / L"one.bin", std::ios::binary) << 'x';
            visible.push_back({path.wstring()});
        }
        sizes.Sync(visible, {});
        check(wait([&] {
            for (const auto& row : visible) if (sizes.Get(row.path).state != FolderSizeState::Ready || sizes.Get(row.path).bytes != 1) return false;
            return true;
        }), "visible folders beyond one IPC batch all complete automatic scans");
        pipe.bytes = 2400;
        Sleep(1100);
        check([&] {
            for (const auto& row : visible) if (sizes.Get(row.path).bytes != 1) return false;
            return true;
        }(), "incorrect index batches cannot replace completed real totals");
        sizes.Stop();
    }
    Sleep(20);
    pipe.stall = true;
    {
        check(wait([&] { return pipe.listening.load(); }), "stalled fixture pipe is listening before client starts");
        const auto before = pipe.requests.load();
        ScanStartGate gate;
        FolderSizes sizes; gate.Hold(sizes); sizes.Sync({{estimate_root.wstring()}, {nested}}, {});
        check(wait([&] { return pipe.requests > before; }), "cancellation fixture reaches an in-flight service request");
        const auto scan_start = GetTickCount64();
        gate.Release();
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Ready; }) && sizes.Get(nested).bytes == 13567 &&
              GetTickCount64() - scan_start < 600,
              "stalled index response does not block a tiny real scan for its 750ms IPC timeout");
        const auto response_count = pipe.responses.load();
        pipe.stall = false;
        check(wait([&] { return pipe.responses > response_count; }), "late wrong index response is actually sent after scan completion");
        Sleep(100);
        check(sizes.Get(nested).state == FolderSizeState::Ready && sizes.Get(nested).source == FolderSizeSource::Scan &&
              sizes.Get(nested).bytes == 13567,
              "late wrong index response cannot replace the completed scan");
        sizes.Stop();
    }
    Sleep(20);
    pipe.stall = true;
    {
        check(wait([&] { return pipe.listening.load(); }), "shutdown fixture pipe is listening before client starts");
        const auto before = pipe.requests.load();
        ScanStartGate gate;
        FolderSizes sizes; gate.Hold(sizes); sizes.Sync({{estimate_root.wstring()}}, {});
        check(wait([&] { return pipe.requests > before; }), "shutdown fixture has a stalled index request");
        gate.Release();
        sizes.Sync({{empty}}, {});
        const auto start = GetTickCount64(); sizes.Stop();
        check(GetTickCount64() - start < 1000 && sizes.Get(empty).bytes != 2400,
            "navigation rejects stale responses and stop cancels stalled IPC promptly");
    }
    pipe.stall = false;
    pipe.disconnect = true;
    Sleep(20);
    {
        check(wait([&] { return pipe.listening.load(); }), "disconnected fixture pipe is listening before client starts");
        FolderSizes sizes; sizes.Sync({{nested}}, {});
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Ready; }) && sizes.Get(nested).bytes == 13567,
              "disconnected index service does not prevent exact local scan");
        sizes.Stop();
    }
    return ok;
}
}
#endif
