#pragma once
#include "app_model.h"
#include "network_locations.h"
#include <algorithm>

namespace pulse::app {
inline void AppendSystemNetworkLocations(ui::SidebarGroup& networks,
                                        const std::vector<SidebarEntry>& imported) {
    networks.navigable = true;
    networks.navigation_path = L"pulse:networks";
    for (const auto& entry : imported) {
        if (std::any_of(networks.items.begin(), networks.items.end(),
            [&](const auto& item) { return SameNetworkLocation(item.path, entry.path); })) continue;
        ui::SidebarItem item;
        item.label = entry.label;
        item.detail = entry.detail;
        item.path = entry.path;
        item.icon_glyph = L"\xE968";
        item.fallback_text = L"Net";
        item.icon_color = ui::HexColor(0x38BDF8);
        networks.items.push_back(std::move(item));
    }
}
} // namespace pulse::app
