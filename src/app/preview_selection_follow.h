#pragma once
#include <string>
#include <cstdint>
#include <utility>

namespace pulse::app {
// Keep navigation inside the preview intact until the list selection changes.
struct PreviewSelectionFollow {
    std::wstring observed;
    uint64_t due = 0;
    void Reset(std::wstring path = {}) { observed = std::move(path); due = 0; }
    bool Observe(const std::wstring& path, uint64_t now) {
        if (path != observed) { observed = path; due = now + 120; return false; }
        if (!due || now < due) return false;
        due = 0;
        return true;
    }
};
} // namespace pulse::app
