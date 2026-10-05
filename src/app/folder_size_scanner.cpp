#include "folder_size_scanner.h"
#include <utility>

namespace pulse::app::folder_size {
namespace {
std::wstring ChildPath(const std::wstring& parent, const wchar_t* name) {
    return parent + (parent.back() == L'\\' ? L"" : L"\\") + name;
}
bool AddBytes(uint64_t& total, uint64_t bytes) {
    if (bytes > UINT64_MAX - total) return false;
    total += bytes;
    return true;
}
}
Scan::Scan(std::wstring input, uint64_t input_revision, uint64_t input_request_epoch)
    : path(std::move(input)), revision(input_revision), request_epoch(input_request_epoch) {
    stack_.push_back(Frame{path});
}
Scan::~Scan() {
    for (auto& frame : stack_) if (frame.find != INVALID_HANDLE_VALUE) FindClose(frame.find);
}
void Scan::FinishFrame() {
    auto frame = std::move(stack_.back()); stack_.pop_back();
    if (frame.find != INVALID_HANDLE_VALUE) FindClose(frame.find);
    FolderSizeValue value;
    value.bytes = frame.bytes;
    value.has_value = frame.read_any;
    value.partial = frame.partial && frame.read_any;
    value.source = FolderSizeSource::Scan;
    value.state = !frame.read_any ? FolderSizeState::Unavailable :
        frame.partial ? FolderSizeState::Partial : FolderSizeState::Ready;
    // Unmonitored children cannot be reused; only keep the root snapshot.
    // Watched subtrees stay staged until the job's revision is validated.
    if (stack_.empty() || frame.watch_generation != 0) {
        if (completed.size() == 128) completed.pop_front();
        completed.push_back({std::move(frame.path), value, frame.watch_generation});
    }
    if (stack_.empty()) { result = value; done = true; }
    else {
        auto& parent = stack_.back();
        const bool added = AddBytes(parent.bytes, frame.bytes);
        parent.partial |= frame.partial || !added;
    }
}
void Scan::Step(const std::atomic<bool>& stopping, const Lookup& lookup, const Coverage& coverage) {
    const auto deadline = GetTickCount64() + 12;
    unsigned processed = 0;
    while (!done && !stopping && GetTickCount64() < deadline && processed++ < 1024) {
        auto& frame = stack_.back();
        bool available = false;
        if (!frame.started) {
            frame.started = true;
            frame.watch_generation = coverage(frame.path);
            const DWORD attrs = GetFileAttributesW(frame.path.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
                (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE))) {
                frame.partial = true; FinishFrame(); continue;
            }
            frame.find = FindFirstFileExW(ChildPath(frame.path, L"*").c_str(), FindExInfoBasic,
                &frame.data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            available = frame.find != INVALID_HANDLE_VALUE;
            if (!available) {
                const auto error = GetLastError();
                frame.read_any = error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES;
                frame.partial = !frame.read_any;
                FinishFrame(); continue;
            }
            frame.read_any = true;
        } else {
            available = FindNextFileW(frame.find, &frame.data) != FALSE;
            if (!available) {
                frame.partial |= GetLastError() != ERROR_NO_MORE_FILES;
                FinishFrame(); continue;
            }
        }
        const auto& data = frame.data;
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        ++entries_scanned;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if ((data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) || stack_.size() >= 128) {
                frame.partial = true; continue;
            }
            const auto child = ChildPath(frame.path, data.cFileName);
            std::optional<uint64_t> cached;
            if (frame.watch_generation != 0) cached = lookup(child);
            if (cached) {
                frame.partial |= !AddBytes(frame.bytes, *cached);
                ++subtree_hits;
            } else stack_.push_back(Frame{child});
        } else {
            const auto bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            frame.partial |= !AddBytes(frame.bytes, bytes);
        }
    }
}
} // namespace pulse::app::folder_size
