#include "../app/context_menu_prefs.h"
#include "../index/index_config.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <cstdio>

namespace pulse::app {
static std::wstring fixture;
std::wstring GetPulseDataDir() { return fixture; }
}
static int failures = 0;
static void Check(bool ok, const char* name) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
int main() {
    using namespace pulse;
    const auto dir = std::filesystem::absolute(L"bench_data/audit-persistence-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    app::fixture = dir.wstring();
    const auto file = app::fixture + L"\\context_menu.json";
    app::ContextMenuPrefs original;
    original.share = true;
    original.SetItemEnabled(L"h:{11111111-2222-3333-4444-555555555555}", false);
    Check(original.Save(), "AUD-014 save nondefault fixture");
    std::wstring before;
    ReadUtf8File(file, before);
    HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "AUD-014 exclusive fixture lock");
    app::ContextMenuPrefs loaded;
    Check(!loaded.Load() && loaded.load_failed, "AUD-014 locked read fails");
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    Check(!loaded.Save(), "AUD-014 unlock cannot overwrite defaults");
    std::wstring after;
    ReadUtf8File(file, after);
    Check(before == after, "AUD-014 bytes retained");
    Check(loaded.Load() && !loaded.load_failed && loaded.share && loaded.Save(), "AUD-014 reread unlocks persistence");
    WriteUtf8FileAtomic(file, L"{\"share\":false,");
    Check(!loaded.Load() && loaded.share && !loaded.Save(), "AUD-014 truncated JSON retains live state and blocks save");
    HANDLE invalid = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    const unsigned char bytes[] = {0xff, 0xfe, 0xff};
    DWORD written = 0;
    Check(invalid != INVALID_HANDLE_VALUE && WriteFile(invalid, bytes, sizeof(bytes), &written, nullptr), "AUD-014 write invalid UTF8 fixture");
    if (invalid != INVALID_HANDLE_VALUE) CloseHandle(invalid);
    Check(!loaded.Load() && !loaded.Save(), "AUD-014 invalid UTF8 blocks save");
    DeleteFileW(file.c_str());
    Check(loaded.Load() && !loaded.load_failed && loaded.Save(), "AUD-014 missing file can initialize");

    const auto machine = app::fixture + L"\\index-config.json";
    const std::wstring valid = LR"({"index_path":"C:\\custom","excluded_paths":["C:\\private]data","D:\\secret"],"excluded_volume_ids":["volume-a"]})";
    WriteUtf8FileAtomic(machine, valid);
    index::IndexConfig config;
    Check(index::LoadIndexConfigFrom(machine, L"default", config) && config.excluded_paths.size() == 2 && config.index_path == L"C:\\custom", "AUD-015 load explicit isolated machine config");
    const auto paths = config.excluded_paths;
    const auto truncated = valid.substr(0, valid.find(L"secret") + 3);
    WriteUtf8FileAtomic(machine, truncated);
    Check(!index::LoadIndexConfigFrom(machine, L"default", config) && config.load_failed && config.excluded_paths == paths && config.index_path == L"C:\\custom", "AUD-015 truncated array retains previous config");
    Check(!index::SaveMachineConfig(config), "AUD-015 failed config cannot reach machine save");
    ReadUtf8File(machine, after);
    Check(after == truncated, "AUD-015 corrupt source remains unchanged");
    WriteUtf8FileAtomic(machine, valid);
    Check(index::LoadIndexConfigFrom(machine, L"default", config) && !config.load_failed, "AUD-015 successful reread clears failure");
    DeleteFileW(machine.c_str());
    Check(index::LoadIndexConfigFrom(machine, L"default", config) && config.index_path == L"default", "AUD-015 missing config initializes defaults");
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
