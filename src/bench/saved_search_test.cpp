#include "../app/saved_search.h"

#include <windows.h>
#include <filesystem>
#include <cstdio>

namespace pulse::app {
static std::wstring fixture;
std::wstring GetPulseDataDir() { return fixture; }
}
using namespace pulse::app;

namespace {
int passed = 0;
int failed = 0;
void Check(bool condition, const wchar_t* name) {
    ++(condition ? passed : failed);
    wprintf(L"[%s] %s\n", condition ? L"PASS" : L"FAIL", name);
}
}

int wmain() {
    const auto dir = std::filesystem::absolute(L"bench_data/saved-search-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    fixture = dir.wstring();
    const std::wstring path = fixture + L"\\explicit.json";

    SavedSearchStore store;
    Check(store.Add({L"源码内容", SavedSearchMode::Content, L"C:\\src", L"TODO", true}),
          L"add content saved search");
    Check(store.Add({L"重复项", SavedSearchMode::Duplicates, L"D:\\files", L"", false}),
          L"add duplicate saved search");
    Check(!store.Add({L"源码内容", SavedSearchMode::Name, L"C:\\", L"name", true}),
          L"reject duplicate name");
    Check(!store.Add({L"invalid", SavedSearchMode::Content, L"C:\\", L"", true}),
          L"reject empty content query");
    Check(store.SaveTo(path), L"atomically save schema v1");

    SavedSearchStore restored;
    Check(restored.LoadFrom(path), L"load schema v1");
    Check(restored.items().size() == 2, L"restore saved search count");
    Check(restored.items()[0].name == L"源码内容" && restored.items()[0].recursive,
          L"restore Unicode fields and options");
    Check(restored.items()[1].mode == SavedSearchMode::Duplicates,
          L"restore duplicate mode");
    Check(restored.Remove(0) && restored.items().size() == 1, L"remove saved search");

    Check(SavedSearchStore::DefaultPath() == fixture + L"\\saved_searches.json",
          L"AUD-017 default path uses injected shared data root");
    Check(store.Save(), L"AUD-017 save through shared isolated root");
    SavedSearchStore isolated;
    Check(isolated.Load() && isolated.items().size() == 2 && isolated.items()[0].name == L"源码内容",
          L"AUD-017 default load reads isolated saved searches");
    fixture += L"\\second";
    std::filesystem::create_directories(fixture);
    Check(restored.Save(), L"AUD-017 second isolated root saves independently");
    Check(isolated.Load() && isolated.items().size() == 1 && isolated.items()[0].mode == SavedSearchMode::Duplicates,
          L"AUD-017 switching shared root cannot mix saved searches");
    std::filesystem::remove_all(dir);
    wprintf(L"%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
