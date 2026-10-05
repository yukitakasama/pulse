#include "../common/windows_compat.h"
#include "../ui/folder_picker_loader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <shobjidl.h>
#include <map>
#include <wrl/client.h>

using namespace pulse::ui;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!ok) ++failures;
}
bool CreateShortcut(const std::filesystem::path& path, const std::filesystem::path& target) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return false;
    bool saved = false;
    {
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> file;
        saved = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
            SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(path.c_str(), TRUE));
    }
    CoUninitialize();
    return saved;
}
constexpr UINT kListing = WM_APP + 73;
constexpr UINT kValidation = WM_APP + 74;
std::unique_ptr<PickerValidation> WaitValidation(HWND window) {
    const auto end = GetTickCount64() + 5000;
    MSG message{};
    while (GetTickCount64() < end) {
        if (PeekMessageW(&message, window, kValidation, kValidation, PM_REMOVE)) return PickerLoader::TakeValidation(message.lParam);
        Sleep(1);
    }
    return {};
}
}
int main() {
    PickerOptions filter;
    filter.filters = {{L"Pictures", L"*.PNG;*.jpg"}};
    Check(PickerMatchesFilter(L"A.PnG", PickerMode::File, filter) && !PickerMatchesFilter(L"a.png.exe", PickerMode::File, filter), "case insensitive exact suffix filter");
    filter.filters = {{L"All", L"*.*"}};
    Check(PickerMatchesFilter(L"LICENSE", PickerMode::File, filter) && PickerMatchesFilter(L"a.unknown", PickerMode::File, filter), "all files includes unknown and absent extensions");
    Check(!PickerMatchesFilter(L"a.svg", PickerMode::Image, filter), "image mode cannot broaden its supported formats");
    Check(!PickerShowsEntry(FILE_ATTRIBUTE_HIDDEN, L"a.txt", PickerMode::File), "hidden defaults off");
    filter.show_hidden = true;
    Check(PickerShowsEntry(FILE_ATTRIBUTE_HIDDEN, L"a.txt", PickerMode::File, filter), "hidden option");
    filter.search = L"DOC";
    Check(PickerShowsEntry(FILE_ATTRIBUTE_DIRECTORY, L"Documents", PickerMode::File, filter) && !PickerShowsEntry(FILE_ATTRIBUTE_DIRECTORY, L"Pictures", PickerMode::File, filter), "search filters folders case insensitively");
    std::vector<PickerEntry> entries{{L"file2", L"", PickerEntryKind::File}, {L"folder", L"", PickerEntryKind::Folder}, {L"file10", L"", PickerEntryKind::File}};
    SortPickerEntries(entries);
    Check(entries[0].name == L"folder" && entries[1].name == L"file2", "folder first and natural name order");
    PickerOptions descending; descending.descending = true;
    SortPickerEntries(entries, descending);
    Check(entries[0].name == L"folder" && entries[1].name == L"file10", "descending keeps folders first");
    entries[1].size = 1; entries[2].size = 10; descending.sort = PickerSort::Size;
    SortPickerEntries(entries, descending);
    Check(entries[1].size == 10, "size sort");
    entries[1].modified.dwLowDateTime = 1; entries[2].modified.dwLowDateTime = 10;
    descending.sort = PickerSort::Modified;
    SortPickerEntries(entries, descending);
    Check(entries[1].modified.dwLowDateTime == 10, "modified sort");
    entries[1].name = L"a.txt"; entries[2].name = L"z.png";
    descending.sort = PickerSort::Type; descending.descending = false;
    SortPickerEntries(entries, descending);
    Check(entries[1].name == L"z.png", "extension type sort");
    PickerHistory history;
    history.Navigate(L"C:\\one");
    Check(history.Back(L"C:\\two") == L"C:\\one" && history.CanGoForward(), "history back stores forward");
    Check(history.Forward(L"C:\\one") == L"C:\\two", "history forward");
    history.Back(L"C:\\two"); history.Navigate(L"C:\\one");
    Check(!history.CanGoForward(), "navigation invalidates forward");
    std::vector<std::wstring> names;
    Check(ParsePickerNames(L"  a file.txt  ", names) && names.size() == 1 && names[0] == L"a file.txt", "unquoted spaces are one filename");
    Check(ParsePickerNames(L"\"a file.txt\" \"b.txt\"", names) && names.size() == 2, "quoted multiple filenames");
    Check(!ParsePickerNames(L"\"unfinished", names) && names.empty(), "unmatched quote rejected");
    Check(NormalizePickerInput(L"\\\\?\\UNC\\server\\share\\file") == L"\\\\server\\share\\file", "UNC extended prefix normalization");
    Check(PickerParent(L"\\\\server\\share") == L"", "UNC share root parent");

    const auto base = std::filesystem::temp_directory_path() / (L"pulse-picker-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(base / L"folder");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{std::filesystem::path(L"\\\\?\\" + base.wstring())};
    std::ofstream(base / L"a file.txt") << "hello";
    std::ofstream(base / L"b.PNG") << "image";
    std::ofstream(base / L"LICENSE") << "license";
    HWND window = CreateWindowExW(0, L"STATIC", L"picker test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "test result window");
    if (!window) return 1;
    {
        PickerLoader loader(window, kListing, kValidation);
        loader.Validate(1, base.wstring(), {L"a file.txt", L"LICENSE"}, PickerMode::File);
        auto result = WaitValidation(window);
        Check(result && result->error == ERROR_SUCCESS && result->paths.size() == 2 && result->paths[0] == (base / L"a file.txt").wstring(), "relative multi-file validation");
        loader.Validate(201, base.wstring(), {L"folder"}, PickerMode::Folder);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1 && result->navigate.empty(), "folder mode confirms directory");
        SetEnvironmentVariableW(L"PULSE_PICKER_TEST_ROOT", base.c_str());
        loader.Validate(202, L"", {L"%PULSE_PICKER_TEST_ROOT%\\LICENSE"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1, "environment path expansion");
        SetEnvironmentVariableW(L"PULSE_PICKER_TEST_ROOT", nullptr);
        auto long_path = base;
        for (int part = 0; part < 6; ++part) {
            long_path /= std::wstring(48, L'x') + std::to_wstring(part);
            const std::wstring native = L"\\\\?\\" + long_path.wstring();
            Check(CreateDirectoryW(native.c_str(), nullptr) != FALSE, "long fixture directory");
        }
        const auto long_file = long_path / L"long.unknown";
        const auto native_file = L"\\\\?\\" + long_file.wstring();
        HANDLE file = CreateFileW(native_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "long fixture file");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        loader.Validate(203, L"", {native_file}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1 && result->paths[0] == long_file.wstring(), "existing extended long file validation");
        loader.Validate(2, base.wstring(), {L"folder"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->navigate == (base / L"folder").wstring(), "directory input navigates");
        loader.Validate(3, base.wstring(), {L"folder", L"LICENSE"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_DIRECTORY && result->paths.empty(), "mixed file and folder rejected atomically");
        loader.Validate(4, base.wstring(), {L"missing"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_FILE_NOT_FOUND, "missing file error");
        loader.Validate(5, base.wstring(), {L"LICENSE"}, PickerMode::Image);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_UNSUPPORTED_TYPE, "unsupported type error");
        loader.CreateFolder(6, base.wstring(), L"created");
        result = WaitValidation(window);
        Check(result && result->error == ERROR_SUCCESS && std::filesystem::is_directory(base / L"created"), "create folder");
        loader.CreateFolder(7, base.wstring(), L"created");
        result = WaitValidation(window);
        Check(result && result->error == ERROR_ALREADY_EXISTS, "create collision never overwrites");
        loader.CreateFolder(8, base.wstring(), L"..\\escape");
        result = WaitValidation(window);
        Check(result && result->error == ERROR_INVALID_NAME, "folder traversal rejected");
        for (uint64_t generation = 10; generation < 200; ++generation) loader.Load(generation, base.wstring(), PickerMode::File);
        loader.Validate(200, base.wstring(), {L"LICENSE"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->generation == 200 && result->paths.size() == 1, "latest request survives rapid navigation");
        Check(CreateShortcut(base / L"file.lnk", base / L"LICENSE"), "file shortcut fixture");
        Check(CreateShortcut(base / L"folder.lnk", base / L"folder"), "directory shortcut fixture");
        Check(CreateShortcut(base / L"image.lnk", base / L"b.PNG"), "image shortcut fixture");
        Check(CreateShortcut(base / L"dead.lnk", base / L"missing-target"), "dead shortcut fixture");
        loader.Validate(300, base.wstring(), {L"file.lnk"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1 && result->paths[0] == (base / L"LICENSE").wstring(), "file shortcut returns actual target");
        loader.Validate(301, base.wstring(), {L"folder.lnk"}, PickerMode::Image);
        result = WaitValidation(window);
        Check(result && result->navigate == (base / L"folder").wstring(), "directory shortcut navigates in image mode");
        loader.Validate(310, base.wstring(), {L"folder.lnk"}, PickerMode::Folder);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1 && result->paths[0] == (base / L"folder").wstring(), "folder shortcut confirms actual directory");
        loader.Validate(311, base.wstring(), {L"file.lnk"}, PickerMode::Folder);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_UNSUPPORTED_TYPE, "folder mode rejects shortcut to file");
        PickerEntry folder_shortcut{L"folder.lnk", (base / L"folder.lnk").wstring(), PickerEntryKind::File};
        Check(PickerChosenPath(PickerMode::Folder, base.wstring(), &folder_shortcut) == folder_shortcut.path, "folder primary validates shortcut rather than selecting current directory");
        Check(PickerShowsEntry(FILE_ATTRIBUTE_NORMAL, L"folder.lnk", PickerMode::Folder), "folder mode shows shortcuts for validation");
        loader.Validate(302, base.wstring(), {L"image.lnk"}, PickerMode::Image);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1 && result->paths[0] == (base / L"b.PNG").wstring(), "image shortcut validates destination type");
        loader.Validate(303, base.wstring(), {L"file.lnk"}, PickerMode::Image);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_UNSUPPORTED_TYPE, "shortcut cannot bypass image restriction");
        loader.Validate(304, base.wstring(), {L"dead.lnk"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_FILE_NOT_FOUND, "dead shortcut fails without target tracking");
        // Shell refuses cyclic/deep shortcut fixtures on this system. Exercise
        // our actual traversal with deterministic targets, retaining COM-backed
        // file, directory, image and dead-link integration cases above.
        std::map<std::wstring, std::wstring> graph{{L"a.lnk", L"b.lnk"}, {L"b.lnk", L"a.lnk"}};
        auto read_next = [&](std::wstring& current, bool& followed) -> DWORD {
            const auto found = graph.find(current);
            followed = found != graph.end();
            if (followed) current = found->second;
            return ERROR_SUCCESS;
        };
        std::wstring chain_path = L"a.lnk";
        Check(FollowPickerShortcutChain(chain_path, read_next) == ERROR_CANT_RESOLVE_FILENAME, "shortcut traversal rejects deterministic cycle");
        graph.clear();
        for (int hop = 0; hop < 16; ++hop) graph[std::to_wstring(hop)] = std::to_wstring(hop + 1);
        chain_path = L"0";
        Check(FollowPickerShortcutChain(chain_path, read_next) == ERROR_SUCCESS && chain_path == L"16", "shortcut traversal accepts sixteen hops");
        graph[L"16"] = L"17";
        chain_path = L"0";
        Check(FollowPickerShortcutChain(chain_path, read_next) == ERROR_CANT_RESOLVE_FILENAME, "shortcut traversal rejects seventeenth hop");
        chain_path = L"0";
        Check(FollowPickerShortcutChain(chain_path, read_next, [] { return true; }) == ERROR_CANCELLED, "shortcut traversal cancels before reader");
        const auto listing = ReadPickerListing(base.wstring(), PickerMode::Image);
        bool only_images = listing.error == ERROR_SUCCESS;
        for (const auto& entry : listing.entries) if (entry.kind != PickerEntryKind::Folder && entry.name != L"b.PNG" && entry.name.find(L".lnk") == std::wstring::npos) only_images = false;
        Check(only_images, "image listing keeps navigable shortcuts and filters unrelated files");
    }
    MSG leftover{};
    Check(!PeekMessageW(&leftover, window, kListing, kValidation, PM_REMOVE), "close drains posted payloads");
    const auto before_close = GetTickCount64();
    {
        PickerLoader closing(window, kListing, kValidation);
        for (uint64_t generation = 0; generation < 100; ++generation) closing.Load(generation, base.wstring(), PickerMode::File);
    }
    Check(GetTickCount64() - before_close < 1000, "close never joins background worker");
    DestroyWindow(window);
    std::error_code cleanup_error;
    std::filesystem::remove_all(cleanup.path, cleanup_error);
    Check(!cleanup_error && !std::filesystem::exists(base), "isolated fixture cleanup");
    std::cout << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
