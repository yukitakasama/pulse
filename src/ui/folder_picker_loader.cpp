#include "../common/windows_compat.h"
#include "folder_picker_loader.h"

#include "../common/localization.h"

#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <cwctype>
#include <shobjidl.h>
#include <wrl/client.h>
#include <array>

namespace pulse::ui {
namespace {

bool IsShortcut(std::wstring_view name) {
    return name.size() >= 4 && CompareStringOrdinal(name.data() + name.size() - 4, 4, L".lnk", 4, TRUE) == CSTR_EQUAL;
}

std::wstring DriveTypeName(UINT type) {
    switch (type) {
    case DRIVE_FIXED: return l10n::Get(l10n::StringId::LocalDisk);
    case DRIVE_REMOVABLE: return l10n::Get(l10n::StringId::RemovableDisk);
    case DRIVE_REMOTE: return l10n::Get(l10n::StringId::NetworkDrive);
    case DRIVE_CDROM: return l10n::Get(l10n::StringId::CdDrive);
    default: return l10n::Get(l10n::StringId::Drive);
    }
}

void ReadDrives(PickerListing& listing, const std::function<bool()>& cancelled) {
    wchar_t roots[512]{};
    const DWORD length = GetLogicalDriveStringsW(ARRAYSIZE(roots) - 1, roots);
    if (length == 0 || length >= ARRAYSIZE(roots)) {
        listing.error = GetLastError();
        return;
    }
    // Empty card readers and optical drives must not raise "insert a disk".
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    for (const wchar_t* root = roots; *root; root += wcslen(root) + 1) {
        if (cancelled()) return;
        const UINT type = GetDriveTypeW(root);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
        PickerEntry entry;
        entry.kind = PickerEntryKind::Drive;
        entry.path = root;
        wchar_t label[MAX_PATH + 1]{};
        const bool ready = GetVolumeInformationW(root, label, ARRAYSIZE(label), nullptr,
                                                 nullptr, nullptr, nullptr, 0) != FALSE;
        if (ready) {
            ULARGE_INTEGER free_bytes{}, total{};
            if (GetDiskFreeSpaceExW(root, &free_bytes, &total, nullptr)) {
                entry.size = total.QuadPart;
                entry.free = free_bytes.QuadPart;
            }
        }
        const std::wstring letter(root, 2);
        entry.name = (label[0] ? std::wstring(label) : DriveTypeName(type)) +
                     L" (" + letter + L")";
        listing.entries.push_back(std::move(entry));
    }
    SetThreadErrorMode(old_mode, nullptr);
}

std::wstring NativePath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') return L"\\\\?\\" + path;
    return path;
}

void ReadFolder(PickerListing& listing, PickerMode mode, const PickerOptions& options,
                const std::function<bool()>& cancelled) {
    std::wstring pattern = NativePath(listing.path);
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        listing.error = GetLastError();
        // An empty drive root reports "no files" rather than an error.
        if (listing.error == ERROR_FILE_NOT_FOUND) {
            const DWORD attributes = GetFileAttributesW(NativePath(listing.path).c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY))
                listing.error = ERROR_SUCCESS;
        }
        return;
    }
    const std::wstring prefix = listing.path.back() == L'\\' ? listing.path
                                                              : listing.path + L'\\';
    do {
        if (cancelled()) break;
        if (!PickerShowsEntry(data.dwFileAttributes, data.cFileName, mode, options)) continue;
        PickerEntry entry;
        entry.name = data.cFileName;
        entry.path = prefix + entry.name;
        entry.kind = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ? PickerEntryKind::Folder : mode == PickerMode::Image && !IsShortcut(entry.name) ? PickerEntryKind::Image : PickerEntryKind::File;
        entry.modified = data.ftLastWriteTime;
        entry.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        listing.entries.push_back(std::move(entry));
    } while (FindNextFileW(find, &data));
    const DWORD error = GetLastError();
    if (!cancelled() && error != ERROR_NO_MORE_FILES) listing.error = error;
    FindClose(find);
}

PickerListing ReadListing(const std::wstring& path, PickerMode mode, const PickerOptions& options,
                          const std::function<bool()>& cancelled) {
    PickerListing listing;
    listing.path = path;
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    if (path.empty()) ReadDrives(listing, cancelled);
    else ReadFolder(listing, mode, options, cancelled);
    SetThreadErrorMode(old_mode, nullptr);
    if (!cancelled()) SortPickerEntries(listing.entries, options);
    return listing;
}

std::wstring ResolvePath(const std::wstring& current, const std::wstring& input) {
    auto path = NormalizePickerInput(input);
    if (path.empty()) return {};
    if (path.rfind(L"\\", 0) != 0 && !(path.size() > 1 && path[1] == L':')) {
        if (current.empty()) return {};
        path = current + L"\\" + path;
    } else if (path[0] == L'\\' && path.rfind(L"\\\\", 0) != 0 && current.size() > 1 && current[1] == L':') {
        path = current.substr(0, 2) + path;
    }
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!needed) return {};
    std::wstring absolute(needed, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), needed, absolute.data(), nullptr);
    if (!written || written >= needed) return {};
    absolute.resize(written);
    return NormalizePickerInput(absolute);
}

DWORD ResolveShortcuts(std::wstring& path, DWORD& attributes, const std::function<bool()>& cancelled) {
    return FollowPickerShortcutChain(path, [&](std::wstring& current, bool& followed) -> DWORD {
        if (!IsShortcut(current) || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_SUCCESS;
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(initialized)) return ERROR_NOT_SUPPORTED;
        DWORD error = ERROR_SUCCESS;
        {
            Microsoft::WRL::ComPtr<IShellLinkW> link;
            Microsoft::WRL::ComPtr<IPersistFile> file;
            std::array<wchar_t, 32768> raw{};
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
                FAILED(link.As(&file)) || FAILED(file->Load(NativePath(current).c_str(), STGM_READ)) ||
                FAILED(link->GetPath(raw.data(), static_cast<int>(raw.size()), nullptr, SLGP_RAWPATH)) || !raw[0]) {
                error = ERROR_INVALID_DATA;
            } else {
                // Parsing only: never invoke Resolve, target tracking, UI or Shell execution.
                current = ResolvePath(PickerParent(current), raw.data());
                if (current.empty()) error = ERROR_INVALID_NAME;
            }
        }
        CoUninitialize();
        if (error != ERROR_SUCCESS) return error;
        if (cancelled()) return ERROR_CANCELLED;
        attributes = GetFileAttributesW(NativePath(current).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
        followed = true;
        return ERROR_SUCCESS;
    }, cancelled);
}

PickerValidation ValidatePaths(const std::wstring& current, const std::vector<std::wstring>& names,
                               PickerMode mode, const PickerOptions& options, const std::function<bool()>& cancelled) {
    PickerValidation result;
    if (names.empty()) { result.error = ERROR_INVALID_NAME; return result; }
    for (const auto& name : names) {
        if (cancelled()) { result.error = ERROR_CANCELLED; break; }
        auto path = ResolvePath(current, name);
        result.failed_path = path.empty() ? name : path;
        if (path.empty()) { result.error = ERROR_INVALID_NAME; break; }
        DWORD attributes = GetFileAttributesW(NativePath(path).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) { result.error = GetLastError(); break; }
        result.error = ResolveShortcuts(path, attributes, cancelled);
        if (result.error != ERROR_SUCCESS) break;
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (names.size() != 1) { result.error = ERROR_DIRECTORY; break; }
            if (mode == PickerMode::Folder) result.paths.push_back(path);
            else result.navigate = path;
        } else if (!PickerMatchesFilter(path.substr(path.find_last_of(L'\\') + 1), mode, options)) {
            result.error = ERROR_UNSUPPORTED_TYPE;
            break;
        } else result.paths.push_back(path);
    }
    if (result.error != ERROR_SUCCESS) { result.paths.clear(); result.navigate.clear(); }
    else result.failed_path.clear();
    return result;
}

PickerValidation MakeFolder(const std::wstring& current, const std::wstring& name) {
    PickerValidation result;
    result.failed_path = name;
    bool invalid = current.empty() || name.empty() || name == L"." || name == L".." ||
        name.back() == L'.' || std::iswspace(name.back()) || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos;
    for (wchar_t c : name) invalid = invalid || c < 32;
    auto stem = name.substr(0, name.find(L'.'));
    for (auto& c : stem) c = static_cast<wchar_t>(std::towupper(c));
    invalid = invalid || stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") && stem[3] >= L'0' && stem[3] <= L'9');
    if (invalid) { result.error = ERROR_INVALID_NAME; return result; }
    const auto path = ResolvePath(current, name);
    if (path.empty()) { result.error = ERROR_INVALID_NAME; return result; }
    if (!CreateDirectoryW(NativePath(path).c_str(), nullptr)) result.error = GetLastError();
    else { result.navigate = path; result.failed_path.clear(); }
    return result;
}
} // namespace

struct PickerLoader::Target {
    std::mutex lock;
    std::condition_variable changed;
    HWND hwnd = nullptr;
    UINT message = 0;
    UINT validation_message = 0;
    std::atomic<uint64_t> serial{0};
    bool closed = false;
    std::function<void(uint64_t)> pending;
};

PickerListing ReadPickerListing(const std::wstring& path, PickerMode mode, const PickerOptions& options) {
    return ReadListing(path, mode, options, [] { return false; });
}

PickerLoader::PickerLoader(HWND hwnd, UINT message, UINT validation_message) : target_(std::make_shared<Target>()) {
    target_->hwnd = hwnd;
    target_->message = message;
    target_->validation_message = validation_message;
    for (int worker = 0; worker < 2; ++worker) std::thread([target = target_] {
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        std::unique_lock lock(target->lock);
        for (;;) {
            target->changed.wait(lock, [&] { return target->closed || !!target->pending; });
            if (target->closed) return;
            auto request = std::move(target->pending);
            target->pending = {};
            const uint64_t serial = target->serial.load();
            lock.unlock();
            request(serial);
            lock.lock();
        }
    }).detach();
}

PickerLoader::~PickerLoader() {
    std::lock_guard guard(target_->lock);
    target_->closed = true;
    ++target_->serial;
    target_->pending = {};
    // Remove already-posted owned payloads before the window can be destroyed.
    MSG message{};
    if (target_->hwnd) {
        while (PeekMessageW(&message, target_->hwnd, target_->message, target_->message, PM_REMOVE)) Take(message.lParam);
        if (target_->validation_message)
            while (PeekMessageW(&message, target_->hwnd, target_->validation_message, target_->validation_message, PM_REMOVE)) TakeValidation(message.lParam);
    }
    target_->hwnd = nullptr;
    target_->changed.notify_all();
}

void PickerLoader::Load(uint64_t generation, std::wstring path, PickerMode mode, PickerOptions options) {
    std::lock_guard guard(target_->lock);
    ++target_->serial;
    target_->pending = [target = target_.get(), generation, path = std::move(path), mode, options = std::move(options)](uint64_t serial) {
        auto cancelled = [&] { return target->serial.load() != serial; };
        auto result = std::make_unique<PickerListing>(ReadListing(path, mode, options, cancelled));
        result->generation = generation;
        std::lock_guard guard(target->lock);
        if (!target->closed && !cancelled() && PostMessageW(target->hwnd, target->message, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    };
    target_->changed.notify_one();
}

void PickerLoader::Validate(uint64_t generation, std::wstring current, std::vector<std::wstring> names,
                            PickerMode mode, PickerOptions options) {
    std::lock_guard guard(target_->lock);
    ++target_->serial;
    target_->pending = [target = target_.get(), generation, current = std::move(current), names = std::move(names), mode, options = std::move(options)](uint64_t serial) {
        auto cancelled = [&] { return target->serial.load() != serial; };
        auto result = std::make_unique<PickerValidation>(ValidatePaths(current, names, mode, options, cancelled));
        result->generation = generation;
        std::lock_guard guard(target->lock);
        if (!target->closed && !cancelled() && target->validation_message && PostMessageW(target->hwnd, target->validation_message, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    };
    target_->changed.notify_one();
}

void PickerLoader::CreateFolder(uint64_t generation, std::wstring current, std::wstring name) {
    std::lock_guard guard(target_->lock);
    ++target_->serial;
    target_->pending = [target = target_.get(), generation, current = std::move(current), name = std::move(name)](uint64_t serial) {
        if (target->serial.load() != serial) return;
        auto result = std::make_unique<PickerValidation>(MakeFolder(current, name));
        result->generation = generation;
        std::lock_guard guard(target->lock);
        if (!target->closed && target->serial.load() == serial && target->validation_message && PostMessageW(target->hwnd, target->validation_message, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    };
    target_->changed.notify_one();
}

std::unique_ptr<PickerListing> PickerLoader::Take(LPARAM lparam) {
    return std::unique_ptr<PickerListing>(reinterpret_cast<PickerListing*>(lparam));
}
std::unique_ptr<PickerValidation> PickerLoader::TakeValidation(LPARAM lparam) {
    return std::unique_ptr<PickerValidation>(reinterpret_cast<PickerValidation*>(lparam));
}

} // namespace pulse::ui
