#include "../app/list_type_ahead.h"
#include "../app/preview_selection_follow.h"
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    const std::vector<std::wstring> names{L"Apple", L"Apricot", L"Banana", L"azure", L"中文"};
    const auto at = [&](int i) -> const std::wstring& { return names[i]; };
    pulse::app::ListTypeAhead type;
    check(type.Find(L'a', 2000, -1, 5, at) == 0, "first letter selects first match");
    check(type.Find(L'A', 2100, 0, 5, at) == 1, "repeated letter cycles case-insensitively");
    check(type.Find(L'a', 2200, 1, 5, at) == 3, "cycle follows displayed order");
    check(type.Find(L'a', 2300, 3, 5, at) == 0, "cycle wraps to start");
    check(type.Find(L'p', 2400, 0, 5, at) == 0, "prefix extension retains current match");
    check(type.Find(L'r', 2500, 0, 5, at) == 1, "multi-character prefix selects Apricot");
    check(type.Find(L'b', 4000, 1, 5, at) == 2, "timeout starts a new prefix");
    check(type.Find(L'中', 6000, 2, 5, at) == 4, "Unicode initial matches");
    check(type.Find(L'x', 8000, 4, 5, at) == -1, "unmatched prefix leaves selection unchanged");
    check(type.Find(L'a', 9000, -1, 0, at) == -1, "empty list is safe");
    type = {};
    check(type.Find(L'a', 10000, 0, 5, at) == 1, "new search begins after current selection");
    check(type.Find(L'b', 10100, 3, 5, at) == 2, "external selection change clears prefix");
    pulse::app::PreviewSelectionFollow follow;
    follow.Reset(L"a");
    check(!follow.Observe(L"a", 1000), "unchanged selection does not reset preview navigation");
    check(!follow.Observe(L"b", 1010) && !follow.Observe(L"b", 1129), "selection waits for debounce");
    check(!follow.Observe(L"c", 1130) && !follow.Observe(L"c", 1249) && follow.Observe(L"c", 1250),
          "rapid switching loads only the latest stable selection");
    check(!follow.Observe(L"c", 1300), "stable selection updates only once");
    follow.Reset();
    check(!follow.Observe(L"", 2000), "closing preview clears pending selection");
    return failures ? 1 : 0;
}
