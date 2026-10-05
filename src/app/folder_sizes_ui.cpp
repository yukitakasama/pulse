#include "folder_sizes_ui.h"
#include "app_state.h"
#include "session.h"
#include "../common/text_format.h"
#include "../common/localization.h"
#include "app_runtime.h"
#include "entry_group.h"
#include "entry_sort.h"
#include <algorithm>
#include <unordered_set>

namespace pulse {
namespace {
// A Size sort needs every folder's total, not only the rows on screen (#58);
// half of the size cache leaves room for what other panes show.
constexpr size_t kSortRequestLimit = 2048;
// Live re-sorts copy the listing on the window thread: keep them small and paced.
constexpr size_t kLiveResortLimit = 20000;
constexpr uint64_t kResortIntervalMs = 750;

std::wstring ChildPath(const std::wstring& parent, const std::wstring& name) {
    return !parent.empty() && parent.back() == L'\\' ? parent + name : parent + L"\\" + name;
}

bool SortsByFolderSize(const ui::PaneViewModel& pane) {
    return pane.sort_column == ui::SortColumn::Size && pane.snapshot && pane.is_file_system &&
        !pane.is_search && !pane.path.empty() && !fs::IsVirtualPath(pane.path);
}
} // namespace

void FillFolderSizes(AppState& s, ui::WindowViewModel& vm) {
    struct Row { ui::PaneViewModel* pane; int index; std::wstring path; };
    std::vector<Row> rows;
    std::vector<app::FolderSizeRequest> requests;
    std::vector<std::wstring> roots;
    for (auto& slot : vm.pane_slots) {
        auto& pane = slot.pane;
        pane.folder_size_labels.clear();
        pane.folder_size_actions.clear();
        // Content hits are files held in a separate paged store, not snapshot/entries.
        if (pane.loading || pane.content_results ||
            (!pane.is_file_system && !pane.is_search) || pane.is_recycle) continue;
        const bool labels = ui::ShowsFolderSize(pane.view_mode);
        const bool sorted = SortsByFolderSize(pane);
        if (!labels && !sorted) continue;
        std::unordered_set<int> shown;
        const auto list = s.renderer.PaneListRect(pane, slot.rect);
        ui::ViewLayout layout(pane.view_mode, list, pane.EntryCount(), pane.scroll_x, pane.scroll_y, s.scale,
                              s.renderer.ListRowHeightDip(pane, list), pane.Groups());
        const auto [first, last] = layout.VisibleRange();
        for (int i = std::max(0, first); labels && i <= last; ++i) {
            const int source = pane.SourceIndex(i);
            if (source < 0) continue;
            std::wstring path;
            if (pane.snapshot) {
                if (static_cast<size_t>(source) >= pane.snapshot->size()) continue;
                const auto& entry = (*pane.snapshot)[static_cast<size_t>(source)];
                if (!entry.is_dir) continue;
                path = entry.full_path.empty() && !pane.is_search ? ChildPath(pane.path, entry.name) : entry.full_path;
            } else {
                if (static_cast<size_t>(source) >= pane.entries.size()) continue;
                const auto& entry = pane.entries[static_cast<size_t>(source)];
                if (!entry.is_dir) continue;
                path = entry.path;
            }
            if (path.empty() || fs::IsVirtualPath(path)) continue;
            rows.push_back({&pane, source, path});
            requests.push_back({path, !fs::IsUncPath(path)});
            shown.insert(source);
        }
        if (sorted) {
            size_t count = shown.size();
            const auto& entries = *pane.snapshot;
            for (size_t i = 0; i < entries.size() && count < kSortRequestLimit; ++i) {
                const auto& entry = entries[i];
                if (!entry.is_dir || entry.drive_type != 0 || shown.contains(static_cast<int>(i))) continue;
                const std::wstring path = entry.full_path.empty() ? ChildPath(pane.path, entry.name) : entry.full_path;
                if (fs::IsVirtualPath(path)) continue;
                requests.push_back({path, !fs::IsUncPath(path), false});
                ++count;
            }
        }
        if (!pane.path.empty() && !fs::IsVirtualPath(pane.path)) roots.push_back(pane.path);
    }
    if (!s.isolatedTest && !s.shot.active)
        s.folderSizes.SetCachePath([] {
            const auto dir = app::GetPulseDataDir();
            return dir.empty() ? std::wstring() : dir + L"\\folder_sizes.json";
        });
    const bool probe_index = (s.isolatedTest || s.shot.active) &&
        GetEnvironmentVariableW(L"PULSE_TEST_FOLDER_INDEX", nullptr, 0) > 0;
    s.folderSizes.SetIndexEnabled((!s.isolatedTest && !s.shot.active) || probe_index);
    s.folderSizes.Sync(std::move(requests), std::move(roots));
    using S = app::FolderSizeState;
    using I = l10n::StringId;
    for (const auto& row : rows) {
        const auto value = s.folderSizes.Get(row.path);
        std::wstring text = value.has_value ? format::ByteSize(value.bytes) : L"";
        if (value.has_value && value.partial) text = L"\u2265 " + text;
        auto suffix = [&](I id) {
            if (!text.empty()) text += L" · ";
            text += l10n::Get(id);
        };
        switch (value.state) {
        case S::Indexed: suffix(I::FolderSizeIndexed); break;
        case S::Manual: suffix(I::FolderSizeCalculate); break;
        case S::Calculating: suffix(I::Calculating); break;
        case S::Updating: suffix(I::FolderSizeUpdating); break;
        case S::Partial: suffix(I::FolderSizePartial); break;
        case S::Unavailable: suffix(I::FolderSizeUnavailable); break;
        case S::Cached: suffix(I::FolderSizeCached); break;
        default: break;
        }
        row.pane->folder_size_labels.emplace(row.index, std::move(text));
        if (value.state == S::Manual || value.state == S::Unavailable ||
            value.state == S::Cached || value.state == S::Partial || value.state == S::Indexed)
            row.pane->folder_size_actions.insert(row.index);
    }
    for (const auto& slot : vm.pane_slots) if (slot.focused) {
        vm.pane.folder_size_labels = slot.pane.folder_size_labels;
        vm.pane.folder_size_actions = slot.pane.folder_size_actions;
    }
}

bool ResortForFolderSizes(AppState& s) {
    bool waiting = false;
    const uint64_t now = GetTickCount64();
    // Rows must not move under a press (the click resolves by index), a drag
    // or the rename box.
    const bool busy = s.renameIndex >= 0 || s.marqueeActive || s.dragPending ||
        (GetKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab || tab->sort_column != ui::SortColumn::Size || !tab->snapshot || tab->loading ||
            tab->pending_generation != 0 || tab->content_results || tab->search_content_active ||
            tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) ||
            tab->snapshot_path != tab->current_path || !tab->held_renames.empty() ||
            tab->snapshot->size() > kLiveResortLimit)
            return;
        const auto sizes = s.folderSizes.KnownChildren(tab->current_path);
        const uint64_t signature = app::FolderSizeSignature(sizes);
        if (signature == tab->folder_size_signature) return;
        if (busy || now - tab->folder_size_resorted_at < kResortIntervalMs) {
            waiting = true;
            return;
        }
        tab->folder_size_signature = signature;
        tab->folder_size_resorted_at = now;
        auto sorted = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
        {
            const app::ScopedEntryGrouping grouping(tab->EffectiveGroup(), tab->current_path);
            app::SortEntriesBySize(*sorted, tab->sort_direction, sizes);
        }
        if (std::equal(sorted->begin(), sorted->end(), tab->snapshot->begin(), tab->snapshot->end(),
                       [](const fs::DirEntry& a, const fs::DirEntry& b) { return a.name == b.name; }))
            return;
        // The selection stays on the same items while the rows move.
        const bool all = tab->all_selected;
        std::vector<std::wstring> names;
        std::wstring focus;
        if (!all) {
            const int count = static_cast<int>(tab->EntryCount());
            for (const int index : tab->SelectedIndices())
                if (index >= 0 && index < count) names.push_back(tab->EntryAt(static_cast<size_t>(index)).name);
            if (tab->selected_index >= 0 && tab->selected_index < count)
                focus = tab->EntryAt(static_cast<size_t>(tab->selected_index)).name;
        }
        tab->SetSnapshot(std::move(sorted));
        tab->order_held = false;
        if (!all) {
            if (names.empty()) tab->ClearSelection();
            else tab->RemapSelection(names, focus);
        }
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    });
    return waiting;
}
}
