#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <cstdint>

namespace pulse::app {
struct ListTypeAhead {
    std::wstring prefix;
    uint64_t last_input = 0;
    int last_match = -1;

    template<class NameAt>
    int Find(wchar_t ch, uint64_t now, int current, int count, NameAt name_at) {
        if (ch < L' ' || count <= 0) return -1;
        if (now - last_input > 1000 || current != last_match) prefix.clear();
        const bool cycle = prefix.size() == 1 &&
            CompareStringOrdinal(prefix.data(), 1, &ch, 1, TRUE) == CSTR_EQUAL;
        if (!cycle) prefix.push_back(ch);
        last_input = now;
        const auto matches = [&](int index) {
            const auto& name = name_at(index);
            return name.size() >= prefix.size() &&
                CompareStringOrdinal(name.data(), static_cast<int>(prefix.size()),
                                     prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
        };
        // Extending a prefix may keep the current match; a repeated letter cycles.
        if (!cycle && prefix.size() > 1 && current >= 0 && current < count && matches(current))
            return last_match = current;
        const int start = current >= 0 && current < count ? current : -1;
        for (int step = 1; step <= count; ++step) {
            const int index = static_cast<int>((static_cast<int64_t>(start) + step) % count);
            if (matches(index)) return last_match = index;
        }
        return -1;
    }
};
} // namespace pulse::app
