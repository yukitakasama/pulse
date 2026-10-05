#include "app_input.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_sidebar_refresh.h"
#include <cstdio>

using namespace pulse;
// Runs only in an explicitly isolated --shot test instance with fixture files.
int RunSelectionInputProbe(AppState& s, const wchar_t* output) {
    FILE* log = nullptr;
    if (_wfopen_s(&log, output, L"w") || !log) return 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(log);
        failures += !ok;
    };
    const auto pump_sidebar = [&](auto ready) {
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            TickSidebarRefresh(s, GetTickCount64());
            if (ready()) return true;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(10);
        }
        return false;
    };
    check(pump_sidebar([&] { return !s.sidebar.drives.empty(); }), "sidebar loads asynchronously after worker startup");
    if (!s.sidebar.drives.empty()) {
        s.sidebar.drives.front().detail = L"stale-capacity";
        RequestSidebarRefresh(s);
        check(pump_sidebar([&] { return s.sidebar.drives.front().detail != L"stale-capacity"; }),
              "background capacity request replaces stale display without rebuilding sidebar");
    }
    auto* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->EntryCount() < 3) { std::fclose(log); return 3; }
    tab->ClearSelection();
    SetFocus(s.hwnd);
    SendMessageW(s.hwnd, WM_CHAR, L'a', 0);
    check(tab->selected_index >= 0 && tab->EntryAt(tab->selected_index).name == L"apple.txt",
          "WM_CHAR selects first matching fixture");
    SendMessageW(s.hwnd, WM_CHAR, L'a', 0);
    check(tab->selected_index >= 0 && tab->EntryAt(tab->selected_index).name == L"apricot.txt",
          "WM_CHAR cycles to next matching fixture");
    ToggleQuickPreview(s);
    const auto original = s.quickPreview.item().path;
    check(s.quickPreview.visible() && original.find(L"apricot.txt") != std::wstring::npos,
          "preview opens selected fixture");
    SetFocus(s.hwnd);
    s.listTypeAhead = {};
    SendMessageW(s.hwnd, WM_CHAR, L'b', 0);
    const auto now = GetTickCount64();
    TickQuickPreviewSelection(s, now);
    check(s.quickPreview.item().path == original, "selection update is debounced");
    TickQuickPreviewSelection(s, now + 121);
    check(s.quickPreview.item().path.find(L"banana.txt") != std::wstring::npos,
          "open preview follows the newly selected file");
    ui::QuickPreviewItem internal = s.quickPreview.item();
    internal.path = original;
    internal.name = L"apricot.txt";
    s.quickPreview.Update(internal);
    TickQuickPreviewSelection(s, now + 300);
    check(s.quickPreview.item().path == original, "unchanged list selection preserves preview navigation");
    tab->ClearSelection();
    TickQuickPreviewSelection(s, now + 400);
    TickQuickPreviewSelection(s, now + 521);
    check(s.quickPreview.visible(), "empty selection keeps the preview accessible");
    s.quickPreview.Close();
    std::fclose(log);
    return failures ? 1 : 0;
}
