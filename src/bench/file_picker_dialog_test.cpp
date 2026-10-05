// Explicitly opt in to short-lived, process-owned picker windows with --interaction.
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../ui/folder_picker_dialog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {
using namespace pulse::ui;
constexpr int kPathId = 101;
constexpr int kFilenameId = 102;
constexpr int kSearchId = 103;

HWND OwnPicker() {
    HWND found = nullptr;
    EnumWindows([](HWND hwnd, LPARAM context) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(hwnd, &process);
        wchar_t name[80]{};
        GetClassNameW(hwnd, name, 80);
        if (process == GetCurrentProcessId() &&
            std::wstring_view(name) == L"PulseFolderPickerWindow") {
            *reinterpret_cast<HWND*>(context) = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool Send(HWND hwnd, UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process != GetCurrentProcessId()) return false;
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(hwnd, message, wparam, lparam,
                              SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result) != 0;
}

std::wstring Text(HWND hwnd) {
    wchar_t text[32768]{};
    Send(hwnd, WM_GETTEXT, std::size(text), reinterpret_cast<LPARAM>(text));
    return text;
}

bool SetText(HWND hwnd, const std::wstring& text) {
    return Send(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

bool Until(const std::function<bool()>& ready, const std::atomic<bool>& done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (!done.load() && std::chrono::steady_clock::now() < end) {
        if (ready()) return true;
        // Poll a condition, rather than assuming an asynchronous operation has finished.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

struct Outcome { bool picked = false; bool driven = false; FilePickerResult result; };
using Driver = std::function<bool(HWND, const std::atomic<bool>&)>;

Outcome Run(const FolderPickerSpec& spec, const Driver& drive, bool legacy = false) {
    Outcome outcome;
    std::atomic<bool> done = false;
    std::thread driver([&] {
        HWND hwnd = nullptr;
        if (Until([&] {
                hwnd = OwnPicker();
                return hwnd && IsWindowVisible(hwnd) && GetDlgItem(hwnd, kSearchId) &&
                    (spec.mode == PickerMode::Folder ||
                     (GetDlgItem(hwnd, kFilenameId) && IsWindowEnabled(GetDlgItem(hwnd, kFilenameId))));
            }, done)) {
            outcome.driven = drive(hwnd, done);
            if (outcome.driven) Until([&] { return !IsWindow(hwnd); }, done);
        }
        if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    });
    if (legacy) {
        std::wstring path;
        outcome.picked = ShowFolderPicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, path);
        if (outcome.picked) outcome.result.paths.push_back(std::move(path));
    } else {
        outcome.picked = ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f},
                                        outcome.result);
    }
    done = true;
    driver.join();
    return outcome;
}

struct Fixture {
    std::filesystem::path root;
    Fixture() {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        root = std::filesystem::path(temp) /
            (L"PulsePickerInteraction-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root / L"nested");
        for (const auto* name : {L"alpha.txt", L"two words.txt", L"blocked.bin", L"background.png",
                                 L"nested\\inside.txt"}) {
            const auto path = root / name;
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("fixture creation");
            CloseHandle(file);
        }
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};

bool HasPaths(const Outcome& outcome, const std::vector<std::wstring>& expected) {
    if (!outcome.driven || !outcome.picked || outcome.result.paths.size() != expected.size())
        return false;
    for (size_t i = 0; i < expected.size(); ++i)
        if (!SamePickerPath(outcome.result.paths[i], expected[i])) return false;
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || std::wstring_view(argv[1]) != L"--interaction") {
        std::puts("Use --interaction to run process-owned file picker UI regression cases.");
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    const auto check = [&](const char* name, bool pass) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
        std::fflush(stdout);
        if (!pass) ++failures;
    };
    try {
        Fixture fixture;
        const auto path = [&](const wchar_t* name) { return (fixture.root / name).wstring(); };
        FolderPickerSpec spec;
        spec.mode = PickerMode::File;
        spec.initial_path = fixture.root.wstring();
        spec.filename = L"alpha.txt";
        spec.filters = {{L"Text files", L"*.txt"}};
        auto outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
            return Until([&] { return SamePickerPath(Text(GetDlgItem(hwnd, kPathId)),
                                                     spec.initial_path); }, done) &&
                Text(GetDlgItem(hwnd, kFilenameId)) == spec.filename &&
                Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
        });
        check("initial directory and filename", HasPaths(outcome, {path(L"alpha.txt")}));

        spec.allow_multiselect = true;
        spec.filename = L"\"alpha.txt\" \"two words.txt\"";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            return Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
        });
        check("quoted multiple filenames", HasPaths(outcome,
              {path(L"alpha.txt"), path(L"two words.txt")}));

        spec.filename = L"blocked.bin";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>& done) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) &&
                Until([&] { return IsWindow(hwnd) && IsWindowEnabled(filename); }, done) &&
                SetText(filename, L"alpha.txt") && Send(filename, WM_KEYDOWN, VK_RETURN);
        });
        check("filter rejects disallowed file and allows retry",
              HasPaths(outcome, {path(L"alpha.txt")}));

        spec.filename = L"nested";
        outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) &&
                Until([&] { return SamePickerPath(Text(GetDlgItem(hwnd, kPathId)),
                                                 path(L"nested")) && IsWindowEnabled(filename); }, done) &&
                SetText(filename, L"inside.txt") && Send(filename, WM_KEYDOWN, VK_RETURN);
        });
        check("typed directory navigates before file submission",
              HasPaths(outcome, {path(L"nested\\inside.txt")}));

        spec.filename = L"alpha.txt";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            HWND search = GetDlgItem(hwnd, kSearchId);
            return SetText(search, L"alpha") && Send(search, WM_KEYDOWN, VK_DOWN) &&
                Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_ESCAPE);
        });
        check("search and arrow key do not submit; Escape cancels",
              outcome.driven && !outcome.picked && outcome.result.paths.empty());

        spec.filename = L"blocked.bin";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) && Send(hwnd, WM_CLOSE);
        });
        check("cancel rejected candidate validation", outcome.driven && !outcome.picked &&
              outcome.result.paths.empty());
        spec.filename = L"two words.txt";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            return Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
        });
        check("new request excludes previous validation result",
              HasPaths(outcome, {path(L"two words.txt")}));

        FolderPickerSpec folder;
        folder.mode = PickerMode::Folder;
        folder.initial_path = fixture.root.wstring();
        outcome = Run(folder, [](HWND hwnd, const std::atomic<bool>& done) {
            // The folder mode has no filename control. Enter is harmless until
            // the directory listing has verified that the current folder exists.
            const bool closed = Until([&] {
                if (!IsWindow(hwnd)) return true;
                Send(hwnd, WM_KEYDOWN, VK_RETURN);
                return !IsWindow(hwnd);
            }, done);
            return closed || done.load();
        }, true);
        check("legacy folder entry confirms current directory",
              HasPaths(outcome, {fixture.root.wstring()}));

        FolderPickerSpec image;
        image.mode = PickerMode::Image;
        image.initial_path = fixture.root.wstring();
        image.filename = L"alpha.txt";
        image.filters = {{L"Text and images", L"*.txt;*.png"}};
        outcome = Run(image, [](HWND hwnd, const std::atomic<bool>& done) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) &&
                Until([&] { return IsWindow(hwnd) && IsWindowEnabled(filename); }, done) &&
                SetText(filename, L"background.png") && Send(filename, WM_KEYDOWN, VK_RETURN);
        }, true);
        check("legacy image entry rejects text despite filter and accepts image extension",
              HasPaths(outcome, {path(L"background.png")}));
    } catch (const std::exception& error) {
        std::printf("[FAIL] fixture: %s\n", error.what());
        ++failures;
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
