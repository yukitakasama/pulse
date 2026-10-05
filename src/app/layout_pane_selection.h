#pragma once

#include "app_model.h"
#include <algorithm>

namespace pulse::app {

// Keep the active pane when shrinking without moving or discarding owned views.
// Existing visible panes take priority; expansion restores the ownership order.
inline std::vector<Pane*> SelectLayoutPanes(
    const std::vector<std::unique_ptr<Pane>>& panes, Pane* focused, size_t count,
    const std::vector<Pane*>& visible = {}) {
    count = std::min(count, panes.size());
    std::vector<Pane*> selected;
    selected.reserve(count);
    const auto append = [&](Pane* pane) {
        if (pane && selected.size() < count &&
            std::find(selected.begin(), selected.end(), pane) == selected.end())
            selected.push_back(pane);
    };
    for (Pane* pane : visible) append(pane);
    for (const auto& pane : panes) append(pane.get());
    if (!selected.empty() && focused &&
        std::find(selected.begin(), selected.end(), focused) == selected.end() &&
        std::any_of(panes.begin(), panes.end(),
                    [focused](const auto& pane) { return pane.get() == focused; }))
        selected.back() = focused;

    std::vector<Pane*> ordered;
    ordered.reserve(selected.size());
    for (const auto& pane : panes) {
        if (std::find(selected.begin(), selected.end(), pane.get()) != selected.end())
            ordered.push_back(pane.get());
    }
    return ordered;
}

} // namespace pulse::app
