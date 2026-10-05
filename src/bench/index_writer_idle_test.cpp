#include "../index/index_protocol.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <windows.h>
#define private public
#include "../index/index_client.h"
#undef private
#include "../index/index_client.cpp"
#include <cstdio>

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    if (!condition) ++failures;
}
uint64_t Time(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
bool CpuTime(HANDLE thread, uint64_t& time) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return false;
    time = Time(kernel) + Time(user);
    return true;
}
void CheckIdle(pulse::index::IndexClient& client, const char* name) {
    uint64_t before = 0, after = 0;
    const bool begin = CpuTime(client.writer_.native_handle(), before);
    const auto wall = GetTickCount64();
    Sleep(200);
    const bool end = CpuTime(client.writer_.native_handle(), after);
    const double cpu_ms = static_cast<double>(after - before) / 10000.0;
    std::printf("[BENCH] %s: wall=%llu ms writer CPU=%.3f ms\n", name,
                static_cast<unsigned long long>(GetTickCount64() - wall), cpu_ms);
    // Accommodate coarse Windows thread accounting; the former loop consumes
    // essentially the entire 200 ms interval when a core is available.
    Check(begin && end && after >= before && cpu_ms < 30.0, name);
}
bool ReadPacket(HANDLE pipe, pulse::ipc::MsgHeader& header, std::vector<uint8_t>& payload) {
    const auto deadline = GetTickCount64() + 2000;
    while (GetTickCount64() < deadline) {
        DWORD available = 0, peeked = 0;
        if (!PeekNamedPipe(pipe, &header, sizeof(header), &peeked, &available, nullptr)) return false;
        if (peeked == sizeof(header)) {
            if (header.magic != pulse::index::kIndexMagic || header.payload_size > 2048) return false;
            if (available >= sizeof(header) + header.payload_size) {
                if (!pulse::ipc::PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header))) return false;
                payload.resize(header.payload_size);
                return payload.empty() || pulse::ipc::PipeRead(pipe, payload.data(), header.payload_size);
            }
        }
        Sleep(5);
    }
    return false;
}
}

int main() {
    // Run the real writer only. Starting the normal client also launches its
    // connection worker, which would touch the installed service or a helper.
    pulse::index::IndexClient client;
    client.running_ = true;
    client.writer_ = std::thread([&] { client.Writer(); });
    CheckIdle(client, "disconnected volume refresh sleeps instead of spinning");
    {
        std::lock_guard lock(client.mu_);
        Check(client.volume_refresh_requested_, "offline volume refresh remains pending");
        client.volume_refresh_requested_ = false;
    }
    pulse::index::Query query;
    query.session_id = 741;
    query.needle = L"isolated-writer-test";
    query.path_prefix = L"C:\\isolated-writer-fixture";
    client.SearchAsync(query, 91);
    CheckIdle(client, "disconnected search sleeps instead of spinning");
    {
        std::lock_guard lock(client.mu_);
        const auto found = client.pending_searches_.find(query.session_id);
        Check(client.have_pending_ && found != client.pending_searches_.end() && found->second.first == 91 &&
              found->second.second.needle == query.needle, "offline search retains request and payload");
    }
    client.CancelSession(query.session_id);
    CheckIdle(client, "disconnected cancellation sleeps instead of spinning");
    {
        std::lock_guard lock(client.mu_);
        Check(!client.have_pending_ && client.pending_searches_.empty() &&
              client.cancelled_sessions_.size() == 1 && client.cancelled_sessions_[0] == query.session_id,
              "offline cancellation removes search and retains cancellation");
    }
    query.session_id = 742;
    client.SearchAsync(query, 92);
    client.RefreshVolumesAsync();

    HANDLE read_pipe = INVALID_HANDLE_VALUE, write_pipe = INVALID_HANDLE_VALUE;
    const bool created = CreatePipe(&read_pipe, &write_pipe, nullptr, 4096) != FALSE;
    Check(created, "create isolated in-process transport without index service");
    if (created) {
        { std::lock_guard lock(client.pipe_mu_); client.pipe_ = write_pipe; }
        client.connected_ = true;
        client.pending_cv_.notify_all();
        bool volume = false, search = false, cancel = false, framing = true;
        for (unsigned i = 0; i < 3; ++i) {
            pulse::ipc::MsgHeader header{};
            std::vector<uint8_t> payload;
            if (!ReadPacket(read_pipe, header, payload)) { framing = false; break; }
            pulse::ipc::PayloadReader reader(payload.data(), payload.size());
            if (header.type == pulse::index::REQ_IDX_VOLUMES)
                volume = header.request_id == 0 && payload.empty();
            else if (header.type == pulse::index::REQ_IDX_SEARCH) {
                uint32_t flags = 0, sort = 0, limit = 0, offset = 0;
                uint64_t session = 0;
                std::wstring needle, prefix;
                search = header.request_id == 92 && reader.GetU32(flags) && reader.GetU32(sort) &&
                    reader.GetU32(limit) && reader.GetU32(offset) && reader.GetString(needle) &&
                    reader.GetString(prefix) && reader.GetU64(session) && needle == query.needle &&
                    prefix == query.path_prefix && session == query.session_id;
            } else if (header.type == 8) {
                uint64_t session = 0;
                cancel = reader.GetU64(session) && session == 741;
            } else framing = false;
        }
        Check(framing && volume, "reconnection sends retained REQ_IDX_VOLUMES");
        Check(framing && search, "reconnection sends retained search with exact request and payload");
        Check(framing && cancel, "reconnection sends retained session cancellation");
        // Let the writer finish bookkeeping after its last successful write.
        const auto drained_deadline = GetTickCount64() + 1000;
        bool drained = false;
        do {
            { std::lock_guard lock(client.mu_);
              drained = !client.have_pending_ && client.pending_searches_.empty() &&
                        !client.volume_refresh_requested_ && client.cancelled_sessions_.empty(); }
            if (!drained) Sleep(1);
        } while (!drained && GetTickCount64() < drained_deadline);
        Check(drained, "successfully sent pending work is drained");
    }
    client.connected_ = false;
    client.running_ = false;
    client.pending_cv_.notify_all();
    const bool stopped = WaitForSingleObject(client.writer_.native_handle(), 1000) == WAIT_OBJECT_0;
    Check(stopped, "shutdown wakes disconnected writer and exits within one second");
    if (!stopped) {
        // Terminate only this isolated test; avoid an unbounded destructor join.
        TerminateProcess(GetCurrentProcess(), 124);
        return 124;
    }
    client.Stop();
    if (read_pipe != INVALID_HANDLE_VALUE) CloseHandle(read_pipe);
    std::printf("[SUMMARY] %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
