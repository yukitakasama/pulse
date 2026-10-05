#include "edit_host.h"
#include "../common/windows_compat.h"
#include "advanced_search_dialog.h"
#include "FluentTokens.h"
#include "fluent_components.h"
#include "fluent_menu.h"
#include "folder_picker_dialog.h"
#include "typography.h"
#include "ui_compositor.h"
#include "window_helpers.h"
#include "../common/localization.h"

#include <commctrl.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "uxtheme.lib")

// Advanced search, redesigned for everyday use:
//   quick recipes -> one keyword box -> "names | contents" -> clickable pills for
//   type/date/size/location -> collapsible expert options -> a plain-language
//   summary with a live result count.
namespace pulse::ui {
namespace {

using I = l10n::StringId;

constexpr wchar_t kClass[] = L"PulseAdvancedSearchWindow";
constexpr float kDlgW = 640.0f;
constexpr float kPad = 24.0f;
constexpr float kLabelW = 84.0f;
constexpr float kPillH = 30.0f;
constexpr UINT_PTR kCountTimer = 73;

enum HitId : int {
    kHitNone = 0, kHitSearch = 1, kHitCancel = 2, kHitClose = 3,
    kHitWholeWord = 11, kHitMatchCase = 12, kHitClear = 13, kHitMore = 20,
    kHitRecipe = 30, kHitMode = 40,
    kHitType = 100, kHitDate = 200, kHitSize = 300, kHitLocation = 400,
    kHitNameHow = 500, kHitContentMode = 600,
};

std::wstring Format(I id, const std::wstring& a) {
    std::wstring out(a.size() + 256, L'\0');
    const int n = swprintf_s(out.data(), out.size(), l10n::Get(id).c_str(), a.c_str());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

// English labels are title-case ("This week"); mid-sentence they read better lower-case.
std::wstring LowerFirst(std::wstring text) {
    if (!text.empty() && text[0] < 0x80) text[0] = static_cast<wchar_t>(towlower(text[0]));
    return text;
}

std::wstring FolderLeaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos || slash + 1 >= p.size() ? p : p.substr(slash + 1);
}

std::wstring GroupDigits(size_t value) {
    std::wstring digits = std::to_wstring(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<size_t>(i), 1, L',');
    return digits;
}

std::wstring KindLabel(index::SearchKind kind) {
    switch (kind) {
    case index::SearchKind::Folder: return l10n::Get(I::KindFolder);
    case index::SearchKind::Document: return l10n::Get(I::KindDocument);
    case index::SearchKind::Image: return l10n::Get(I::KindImage);
    case index::SearchKind::Video: return l10n::Get(I::KindVideo);
    case index::SearchKind::Audio: return l10n::Get(I::KindAudio);
    case index::SearchKind::Archive: return l10n::Get(I::KindArchive);
    case index::SearchKind::Code: return l10n::Get(I::KindCode);
    case index::SearchKind::Custom: return l10n::Get(I::KindCustom);
    default: return l10n::Get(I::KindAny);
    }
}

std::wstring ExtLabel(const std::wstring& exts) {
    std::wstring out = L".";
    for (wchar_t c : exts) {
        if (c == L';') out += L" .";
        else out.push_back(static_cast<wchar_t>(towupper(c)));
    }
    return out;
}

std::wstring DateLabel(app::DatePreset preset) {
    switch (preset) {
    case app::DatePreset::Today: return l10n::Get(I::DateToday);
    case app::DatePreset::Yesterday: return l10n::Get(I::DateYesterday);
    case app::DatePreset::ThisWeek: return l10n::Get(I::DateThisWeek);
    case app::DatePreset::ThisMonth: return l10n::Get(I::DateThisMonth);
    case app::DatePreset::ThisYear: return l10n::Get(I::DateThisYear);
    case app::DatePreset::Custom: return l10n::Get(I::AdvCustom);
    default: return l10n::Get(I::KindAny);
    }
}

std::wstring SizeLabel(app::SizePreset preset, const std::wstring& custom) {
    switch (preset) {
    case app::SizePreset::Empty: return l10n::Get(I::SizeEmpty);
    case app::SizePreset::Lt1MB: return l10n::Get(I::SizeLt1MB);
    case app::SizePreset::From1To10MB: return l10n::Get(I::Size1To10MB);
    case app::SizePreset::Gt10MB: return l10n::Get(I::SizeGt10MB);
    case app::SizePreset::Gt100MB: return l10n::Get(I::SizeGt100MB);
    case app::SizePreset::Gt1GB: return l10n::Get(I::SizeGt1GB);
    case app::SizePreset::Custom: {
        std::wstring text = custom;
        for (size_t at; (at = text.find(L"size:")) != std::wstring::npos;) text.erase(at, 5);
        return text.empty() ? l10n::Get(I::AdvCustom) : text;
    }
    default: return l10n::Get(I::KindAny);
    }
}

std::wstring NameHowLabel(app::NameMatchHow how) {
    switch (how) {
    case app::NameMatchHow::StartsWith: return l10n::Get(I::AdvSearchStarts);
    case app::NameMatchHow::Exact: return l10n::Get(I::AdvSearchExact);
    default: return l10n::Get(I::AdvSearchContains);
    }
}

std::wstring ContentModeLabel(index::ContentMatchMode mode) {
    switch (mode) {
    case index::ContentMatchMode::Phrase: return l10n::Get(I::ContentPhrase);
    case index::ContentMatchMode::AnyWord: return l10n::Get(I::ContentAnyWord);
    default: return l10n::Get(I::ContentAllWords);
    }
}

struct Pill {
    D2D1_RECT_F bounds{};
    std::wstring text;
    const wchar_t* glyph = L"";
    int id = 0;
    bool selected = false;
    bool enabled = true;
};

struct RowLabel {
    D2D1_RECT_F bounds{};
    std::wstring text;
};

class AdvancedSearchWindow {
public:
    AdvancedSearchDialogResult Show(HWND owner, app::AdvancedSearchSpec spec, bool dark,
                                    D2D1_COLOR_F accent, AdvancedSearchCountRequest count) {
        owner_ = owner;
        spec_ = std::move(spec);
        dark_ = dark;
        accent_ = accent;
        count_request_ = std::move(count);
        content_mode_ = !spec_.content.empty();
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow())) / 96.0f;
        result_ = {};

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kDlgW * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClass,
            l10n::Get(I::AdvancedSearch).c_str(),
            WS_POPUP | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width,
            static_cast<int>(560.0f * scale_), owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return result_;
        Recompute(false);
        const int height = static_cast<int>(std::ceil(height_px_));
        SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        CenterOwnedWindow(hwnd_, owner_, width, height);
        if (owner_) EnableWindow(owner_, FALSE);
        Render();
        ShowWindow(hwnd_, SW_SHOW);
        LayoutEdits();
        SetForegroundWindow(hwnd_);
        if (edit_kw_) {
            SetFocus(edit_kw_);
            SendMessageW(edit_kw_, EM_SETSEL, 0, -1);
        }
        RequestCountSoon();

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (IsDialogMessageW(hwnd_, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        done_ = true;
        if (IsWindow(hwnd_)) HideComposedDialog(hwnd_, owner_);
        DestroyEdits();
        if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        if (owner_) EnableWindow(owner_, TRUE);
        return result_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<AdvancedSearchWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<AdvancedSearchWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    D2D1_RECT_F R(float x, float y, float w, float h) const { return DipRect(scale_, x, y, w, h); }
    D2D1_RECT_F CloseRect() const { return R(kDlgW - 46, 0, 46, 36); }

    // ---- state helpers -------------------------------------------------------------

    bool RecipeActive(int recipe) const {
        const bool plain_size = spec_.size == app::SizePreset::Any;
        switch (recipe) {
        case 0: return !content_mode_ && spec_.kind == index::SearchKind::Document &&
                       spec_.date == app::DatePreset::ThisMonth && plain_size;
        case 1: return !content_mode_ && spec_.kind == index::SearchKind::Any &&
                       spec_.date == app::DatePreset::Any && spec_.size == app::SizePreset::Gt100MB;
        case 2: return !content_mode_ && spec_.kind == index::SearchKind::Image &&
                       spec_.date == app::DatePreset::ThisWeek && plain_size;
        case 3: return content_mode_ && spec_.kind == index::SearchKind::Document &&
                       spec_.date == app::DatePreset::Any && plain_size;
        default: return false;
        }
    }

    int AdvancedSetCount() const {
        int n = 0;
        if (!EditText(edit_exts_).empty()) ++n;
        if (!EditText(edit_exclude_).empty()) ++n;
        if (content_mode_) {
            if (!EditText(edit_name_also_).empty()) ++n;
            if (spec_.content_mode != index::ContentMatchMode::AllWords) ++n;
            if (spec_.whole_word) ++n;
            if (spec_.case_sensitive) ++n;
        } else if (spec_.name_how != app::NameMatchHow::Contains) {
            ++n;
        }
        return n;
    }

    void SyncFromEdits() {
        const std::wstring keyword = EditText(edit_kw_);
        const std::wstring exclude = EditText(edit_exclude_);
        if (content_mode_) {
            spec_.content = keyword;
            spec_.name = EditText(edit_name_also_);
            spec_.content_exclude = exclude;
            spec_.exclude_name.clear();
        } else {
            spec_.name = keyword;
            spec_.content.clear();
            spec_.content_exclude.clear();
            spec_.exclude_name = exclude;
            spec_.whole_word = spec_.case_sensitive = false;
        }
        spec_.custom_exts = app::NormalizeExtensionList(EditText(edit_exts_));
        if (!spec_.custom_exts.empty()) spec_.kind = index::SearchKind::Custom;
        else if (spec_.kind == index::SearchKind::Custom) spec_.kind = index::SearchKind::Any;
    }

    std::wstring Summary() const {
        std::vector<std::wstring> parts;
        std::wstring what = spec_.kind == index::SearchKind::Any ? l10n::Get(I::AdvSumFiles)
            : spec_.kind == index::SearchKind::Custom ? ExtLabel(spec_.custom_exts)
            : LowerFirst(KindLabel(spec_.kind));
        parts.push_back(Format(I::AdvSumFindFormat, what));
        std::wstring keyword = content_mode_ ? spec_.content : spec_.name;
        if (content_mode_) {
            parts.push_back(keyword.empty() ? l10n::Get(I::AdvSumKeywordNeeded)
                                            : Format(I::AdvSumContentFormat, keyword));
            if (!spec_.name.empty()) parts.push_back(Format(I::AdvSumNameFormat, spec_.name));
        } else if (!keyword.empty()) {
            parts.push_back(Format(I::AdvSumNameFormat, keyword));
        }
        if (spec_.date != app::DatePreset::Any)
            parts.push_back(Format(I::AdvSumDateFormat, LowerFirst(DateLabel(spec_.date))));
        if (spec_.size != app::SizePreset::Any)
            parts.push_back(LowerFirst(SizeLabel(spec_.size, spec_.size_custom)));
        const std::wstring& exclude = content_mode_ ? spec_.content_exclude : spec_.exclude_name;
        if (!exclude.empty()) parts.push_back(Format(I::AdvSumExcludeFormat, exclude));
        std::wstring where = l10n::Get(I::LocationIndexed);
        if (spec_.location == app::LocationScope::CurrentFolder && !spec_.current_folder.empty())
            where = FolderLeaf(spec_.current_folder);
        else if (spec_.location == app::LocationScope::CustomFolder && !spec_.custom_folder.empty())
            where = FolderLeaf(spec_.custom_folder);
        parts.push_back(Format(I::AdvSumInFormat, where));
        std::wstring out;
        for (const auto& part : parts) {
            if (!out.empty()) out += L" \u00b7 ";
            out += part;
        }
        return out;
    }

    std::wstring CountLine(bool& danger) const {
        danger = false;
        if (scope_error_) {
            danger = true;
            return l10n::Get(I::AdvancedSearchNeedScopeMessage);
        }
        if (content_mode_) return l10n::Get(I::AdvCountContent);
        if (!count_request_) return {};
        if (count_pending_ || !count_known_) return l10n::Get(I::AdvCountBusy);
        if (count_ == 0) return l10n::Get(I::AdvCountNone);
        return Format(I::AdvCountFormat, GroupDigits(count_));
    }

    // ---- layout --------------------------------------------------------------------

    float PillWidth(const std::wstring& text, const wchar_t* glyph) const {
        float w = painter_.MeasureButtonWidth(text, glyph ? glyph : L"", false);
        if (w < 1.0f) w = (24.0f + static_cast<float>(text.size()) * 8.0f) * scale_;
        return std::max(44.0f * scale_, std::ceil(w + 4.0f * scale_));
    }

    // Lays pills out left to right, wrapping inside [left, right]. Returns the bottom.
    float FlowPills(std::vector<Pill> row, float left, float right, float top) {
        const float gap = 6.0f * scale_;
        const float h = kPillH * scale_;
        float x = left, y = top;
        for (auto& pill : row) {
            const float w = std::min(right - left, PillWidth(pill.text, pill.glyph));
            if (x > left && x + w > right) {
                x = left;
                y += h + gap;
            }
            pill.bounds = D2D1::RectF(x, y, x + w, y + h);
            x += w + gap;
            pills_.push_back(std::move(pill));
        }
        return y + h;
    }

    void AddRowLabel(I id, float top) {
        rows_.push_back({D2D1::RectF(kPad * scale_, top, (kPad + kLabelW - 6.0f) * scale_,
                                     top + kPillH * scale_), l10n::Get(id)});
    }

    void Layout() {
        pills_.clear();
        rows_.clear();
        const float s = scale_;
        const float left = kPad * s;
        const float right = (kDlgW - kPad) * s;
        const float value_left = (kPad + kLabelW) * s;
        float y = 52.0f * s;

        // Quick recipes: a whole search in one click.
        {
            std::vector<Pill> row;
            const I labels[] = {I::AdvRecipeDocs, I::AdvRecipeBig, I::AdvRecipeImages, I::AdvRecipeMentions};
            const wchar_t* glyphs[] = {L"\xE8A5", L"\xE7B8", L"\xEB9F", L"\xE721"};
            for (int i = 0; i < 4; ++i)
                row.push_back({{}, l10n::Get(labels[i]), glyphs[i], kHitRecipe + i, RecipeActive(i), true});
            y = FlowPills(std::move(row), left, right, y) + 14.0f * s;
        }
        kw_rect_ = D2D1::RectF(left, y, right, y + 36.0f * s);
        y = kw_rect_.bottom + 10.0f * s;
        {
            const std::wstring a = l10n::Get(I::AdvModeName), b = l10n::Get(I::AdvModeContent);
            const float w = std::max(PillWidth(a, L"\xE8D2"), PillWidth(b, L"\xE8A5")) + 8.0f * s;
            mode_rect_[0] = D2D1::RectF(left, y, left + w, y + kPillH * s);
            mode_rect_[1] = D2D1::RectF(left + w + 6.0f * s, y, left + 2.0f * w + 6.0f * s, y + kPillH * s);
            y = mode_rect_[0].bottom + 16.0f * s;
        }
        // Type
        {
            AddRowLabel(I::AdvSearchKind, y);
            std::vector<Pill> row;
            const index::SearchKind kinds[] = {
                index::SearchKind::Any, index::SearchKind::Folder, index::SearchKind::Document,
                index::SearchKind::Image, index::SearchKind::Video, index::SearchKind::Audio,
                index::SearchKind::Archive, index::SearchKind::Code};
            for (auto kind : kinds)
                row.push_back({{}, KindLabel(kind), L"", kHitType + static_cast<int>(kind), spec_.kind == kind, true});
            if (spec_.kind == index::SearchKind::Custom && !spec_.custom_exts.empty())
                row.push_back({{}, ExtLabel(spec_.custom_exts), L"",
                               kHitType + static_cast<int>(index::SearchKind::Custom), true, true});
            y = FlowPills(std::move(row), value_left, right, y) + 10.0f * s;
        }
        // Date
        {
            AddRowLabel(I::AdvSearchDate, y);
            std::vector<Pill> row;
            for (int v = 0; v <= static_cast<int>(app::DatePreset::ThisYear); ++v) {
                const auto preset = static_cast<app::DatePreset>(v);
                row.push_back({{}, DateLabel(preset), L"", kHitDate + v, spec_.date == preset, true});
            }
            if (spec_.date == app::DatePreset::Custom)
                row.push_back({{}, DateLabel(spec_.date), L"",
                               kHitDate + static_cast<int>(app::DatePreset::Custom), true, true});
            y = FlowPills(std::move(row), value_left, right, y) + 10.0f * s;
        }
        // Size
        {
            AddRowLabel(I::AdvSearchSize, y);
            std::vector<Pill> row;
            const app::SizePreset sizes[] = {
                app::SizePreset::Any, app::SizePreset::Lt1MB, app::SizePreset::From1To10MB,
                app::SizePreset::Gt10MB, app::SizePreset::Gt100MB, app::SizePreset::Gt1GB};
            // Compact, language-neutral labels keep the whole scale on one line.
            const wchar_t* compact[] = {nullptr, L"< 1 MB", L"1\u201310 MB", L"> 10 MB", L"> 100 MB", L"> 1 GB"};
            for (size_t i = 0; i < std::size(sizes); ++i)
                row.push_back({{}, compact[i] ? std::wstring(compact[i]) : SizeLabel(sizes[i], {}), L"",
                               kHitSize + static_cast<int>(sizes[i]), spec_.size == sizes[i], true});
            if (spec_.size == app::SizePreset::Empty || spec_.size == app::SizePreset::Custom)
                row.push_back({{}, SizeLabel(spec_.size, spec_.size_custom), L"",
                               kHitSize + static_cast<int>(spec_.size), true, true});
            y = FlowPills(std::move(row), value_left, right, y) + 10.0f * s;
        }
        // Location
        {
            AddRowLabel(I::AdvSearchLocation, y);
            std::vector<Pill> row;
            row.push_back({{}, l10n::Get(I::LocationIndexed), L"\xE774", kHitLocation + 0,
                           spec_.location == app::LocationScope::Indexed, true});
            std::wstring current = l10n::Get(I::LocationCurrent);
            if (!spec_.current_folder.empty()) current += L" \u00b7 " + FolderLeaf(spec_.current_folder);
            row.push_back({{}, current, L"\xE8B7", kHitLocation + 1,
                           spec_.location == app::LocationScope::CurrentFolder, !spec_.current_folder.empty()});
            const bool custom = spec_.location == app::LocationScope::CustomFolder && !spec_.custom_folder.empty();
            row.push_back({{}, custom ? FolderLeaf(spec_.custom_folder) : l10n::Get(I::AdvPickFolder),
                           L"\xED25", kHitLocation + 2, custom, true});
            y = FlowPills(std::move(row), value_left, right, y) + 12.0f * s;
        }
        // Expert options, folded away until asked for.
        {
            const int n = AdvancedSetCount();
            wchar_t text[128]{};
            if (n > 0) swprintf_s(text, l10n::Get(I::AdvMoreSetFormat).c_str(), n);
            more_text_ = std::wstring(more_ ? L"\u25be  " : L"\u25b8  ") +
                         (n > 0 ? std::wstring(text) : l10n::Get(I::AdvMore));
            more_rect_ = D2D1::RectF(left, y, left + 320.0f * s, y + 26.0f * s);
            y = more_rect_.bottom + 8.0f * s;
        }
        ext_rect_ = exclude_rect_ = name_also_rect_ = ww_rect_ = mc_rect_ = D2D1::RectF(0, 0, 0, 0);
        if (more_) {
            const float field_h = 32.0f * s;
            AddRowLabel(I::AdvSearchExtensions, y);
            ext_rect_ = D2D1::RectF(value_left, y, right, y + field_h);
            y = ext_rect_.bottom + 8.0f * s;
            AddRowLabel(I::AdvSearchExclude, y);
            exclude_rect_ = D2D1::RectF(value_left, y, right, y + field_h);
            y = exclude_rect_.bottom + 8.0f * s;
            if (content_mode_) {
                AddRowLabel(I::AdvSearchName, y);
                name_also_rect_ = D2D1::RectF(value_left, y, right, y + field_h);
                y = name_also_rect_.bottom + 8.0f * s;
            }
            AddRowLabel(I::AdvMatch, y);
            std::vector<Pill> row;
            if (content_mode_) {
                for (int v = 0; v < 3; ++v) {
                    const auto mode = static_cast<index::ContentMatchMode>(v);
                    row.push_back({{}, ContentModeLabel(mode), L"", kHitContentMode + v, spec_.content_mode == mode, true});
                }
            } else {
                for (int v = 0; v < 3; ++v) {
                    const auto how = static_cast<app::NameMatchHow>(v);
                    row.push_back({{}, NameHowLabel(how), L"", kHitNameHow + v, spec_.name_how == how, true});
                }
            }
            y = FlowPills(std::move(row), value_left, right, y) + 8.0f * s;
            if (content_mode_) {
                ww_rect_ = D2D1::RectF(value_left, y, value_left + 200.0f * s, y + 28.0f * s);
                mc_rect_ = D2D1::RectF(value_left + 210.0f * s, y, right, y + 28.0f * s);
                y = ww_rect_.bottom + 8.0f * s;
            }
        }
        // Footer: what will be searched, how many hits, and the buttons.
        footer_top_ = y + 6.0f * s;
        summary_ = Summary();
        const float summary_w = right - left;
        float summary_h = painter_.MeasureWrappedCaptionHeight(summary_, summary_w);
        if (summary_h < 1.0f) summary_h = 20.0f * s;
        summary_origin_ = D2D1::Point2F(left, footer_top_ + 12.0f * s);
        const float buttons_top = summary_origin_.y + summary_h + 10.0f * s;
        const float bh = 32.0f * s;
        search_rect_ = D2D1::RectF(right - 96.0f * s, buttons_top, right, buttons_top + bh);
        cancel_rect_ = D2D1::RectF(search_rect_.left - 8.0f * s - 80.0f * s, buttons_top,
                                   search_rect_.left - 8.0f * s, buttons_top + bh);
        clear_rect_ = D2D1::RectF(cancel_rect_.left - 8.0f * s - 76.0f * s, buttons_top,
                                  cancel_rect_.left - 8.0f * s, buttons_top + bh);
        count_rect_ = D2D1::RectF(left, buttons_top, clear_rect_.left - 8.0f * s, buttons_top + bh);
        height_px_ = search_rect_.bottom + 18.0f * s;
    }

    // ---- edits ---------------------------------------------------------------------

    D2D1_COLOR_F EditForeground() const { return ColorFromRef(EditTextColor(dark_)); }
    D2D1_COLOR_F EditBackground() const { return ColorFromRef(EditBackColor(dark_)); }

    I KeywordHint() const { return content_mode_ ? I::AdvKeywordContentHint : I::AdvKeywordNameHint; }
    I ExcludeHint() const { return content_mode_ ? I::AdvExcludeContentHint : I::AdvExcludeNameHint; }

    HWND CreateField(int id, const std::wstring& text, I cue_id) {
        HWND edit = CreateChildEdit(hwnd_, text.c_str());
        if (!edit) return nullptr;
        SetWindowTheme(edit, L"", L"");
        const auto& cue = l10n::Get(cue_id);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
        if (!compositor_.CustomEditEnabled())
            SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA);
        if (font_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SetWindowSubclass(edit, EditProc, static_cast<UINT_PTR>(id), reinterpret_cast<DWORD_PTR>(this));
        return edit;
    }

    void SetCue(HWND edit, I cue_id) {
        if (!edit) return;
        const auto& cue = l10n::Get(cue_id);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    }

    static bool Visible(const D2D1_RECT_F& r) { return r.right > r.left; }

    void PlaceEdit(HWND hwnd, const D2D1_RECT_F& cell) {
        if (!hwnd || !hwnd_) return;
        if (!Visible(cell)) {
            if (GetFocus() == hwnd && edit_kw_) SetFocus(edit_kw_);
            ShowWindow(hwnd, SW_HIDE);
            return;
        }
        POINT pt{ static_cast<int>(std::lround(cell.left + 10.0f * scale_)),
                  static_cast<int>(std::lround(cell.top)) };
        const int w = std::max(40, static_cast<int>(std::lround(cell.right - cell.left - 20.0f * scale_)));
        const int cell_h = std::max(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
        const int line_h = EditLineHeight(hwnd, font_, cell_h);
        pt.y += std::max(0, (cell_h - line_h) / 2);
        SetWindowPos(hwnd, HWND_TOP, pt.x, pt.y, w, line_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // Layered children need an initial bitmap before they can receive clicks.
        PresentChildEdit(compositor_, compositor_.TextFormat(), EditForeground(), EditBackground(), hwnd);
    }

    void LayoutEdits() {
        if (done_) return;
        PlaceEdit(edit_kw_, kw_rect_);
        PlaceEdit(edit_exts_, ext_rect_);
        PlaceEdit(edit_exclude_, exclude_rect_);
        PlaceEdit(edit_name_also_, name_also_rect_);
    }

    std::vector<HWND> VisibleEdits() const {
        std::vector<HWND> edits;
        if (edit_kw_) edits.push_back(edit_kw_);
        if (edit_exts_ && Visible(ext_rect_)) edits.push_back(edit_exts_);
        if (edit_exclude_ && Visible(exclude_rect_)) edits.push_back(edit_exclude_);
        if (edit_name_also_ && Visible(name_also_rect_)) edits.push_back(edit_name_also_);
        return edits;
    }

    static std::wstring EditText(HWND hwnd) {
        if (!hwnd) return {};
        const int n = GetWindowTextLengthW(hwnd);
        std::wstring text(static_cast<size_t>(n), L'\0');
        if (n > 0) GetWindowTextW(hwnd, text.data(), n + 1);
        return text;
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<AdvancedSearchWindow*>(ref);
        if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
        switch (msg) {
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                self->Complete(false);
                return 0;
            }
            if (wparam == VK_RETURN) {
                self->Complete(true);
                return 0;
            }
            if (wparam == VK_TAB) {
                const auto edits = self->VisibleEdits();
                const auto it = std::find(edits.begin(), edits.end(), hwnd);
                if (!edits.empty()) {
                    const int n = static_cast<int>(edits.size());
                    int i = it == edits.end() ? 0 : static_cast<int>(it - edits.begin());
                    i = (i + ((GetKeyState(VK_SHIFT) & 0x8000) ? n - 1 : 1)) % n;
                    SetFocus(edits[static_cast<size_t>(i)]);
                }
                return 0;
            }
            break;
        case WM_CHAR:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB) return 0;
            break;
        }
        // Shared LumaText hosting presents typed text at once and falls back
        // to the native EDIT when a layered present fails (#41), so a field
        // never turns invisible or click-through.
        const bool focus_ring = msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
                                msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK;
        LRESULT result = 0;
        if (!HandleChildEditMessage(self->compositor_, self->compositor_.TextFormat(),
                                    self->EditForeground(), self->EditBackground(),
                                    EditBackBrush(self->edit_brush_), hwnd, msg, wparam, lparam, result)) {
            result = DefPresentedChildEditProc(self->compositor_, self->compositor_.TextFormat(),
                                               self->EditForeground(), self->EditBackground(),
                                               hwnd, msg, wparam, lparam);
        }
        if (focus_ring) InvalidateRect(self->hwnd_, nullptr, FALSE);
        return result;
    }

    void DestroyEdits() {
        for (HWND* edit : {&edit_kw_, &edit_exts_, &edit_exclude_, &edit_name_also_}) {
            if (*edit) { DestroyWindow(*edit); *edit = nullptr; }
        }
    }

    // ---- behaviour -----------------------------------------------------------------

    // Re-reads the edits, recompiles the query, re-lays out and (if needed) resizes.
    void Recompute(bool resize = true) {
        SyncFromEdits();
        preview_ = app::CompileSearchQuery(spec_);
        const auto split = app::SplitSearchQueryText(preview_);
        scope_error_ = split.content.present() && app::ContentSearchNeedsScope(split) &&
                       spec_.location == app::LocationScope::Indexed;
        const float old_height = height_px_;
        Layout();
        if (resize && hwnd_ && std::fabs(old_height - height_px_) > 0.5f) {
            RECT rc{};
            GetWindowRect(hwnd_, &rc);
            SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left,
                         static_cast<int>(std::ceil(height_px_)),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutEdits();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        RequestCountSoon();
    }

    void RequestCountSoon() {
        if (!hwnd_ || !count_request_ || content_mode_ || scope_error_) {
            count_pending_ = false;
            return;
        }
        if (count_known_ && preview_ == counted_query_ && !count_timer_) return;
        count_pending_ = true;
        count_timer_ = true;
        SetTimer(hwnd_, kCountTimer, 250, nullptr);
    }

    void SetContentMode(bool content) {
        if (content == content_mode_) return;
        SyncFromEdits();
        content_mode_ = content;
        if (!content && edit_name_also_) SetWindowTextW(edit_name_also_, L"");
        SetCue(edit_kw_, KeywordHint());
        SetCue(edit_exclude_, ExcludeHint());
    }

    void ApplyRecipe(int recipe) {
        SyncFromEdits();
        const bool active = RecipeActive(recipe);
        spec_.kind = index::SearchKind::Any;
        spec_.custom_exts.clear();
        if (edit_exts_) SetWindowTextW(edit_exts_, L"");
        spec_.date = app::DatePreset::Any;
        spec_.size = app::SizePreset::Any;
        if (active) {
            if (recipe == 3) SetContentMode(false);
        } else if (recipe == 0) {
            SetContentMode(false);
            spec_.kind = index::SearchKind::Document;
            spec_.date = app::DatePreset::ThisMonth;
        } else if (recipe == 1) {
            SetContentMode(false);
            spec_.size = app::SizePreset::Gt100MB;
        } else if (recipe == 2) {
            SetContentMode(false);
            spec_.kind = index::SearchKind::Image;
            spec_.date = app::DatePreset::ThisWeek;
        } else {
            SetContentMode(true);
            spec_.kind = index::SearchKind::Document;
            if (edit_kw_) SetFocus(edit_kw_);
        }
    }

    void Click(int id) {
        SyncFromEdits();
        if (id == kHitMore) {
            more_ = !more_;
        } else if (id == kHitWholeWord) {
            spec_.whole_word = !spec_.whole_word;
        } else if (id == kHitMatchCase) {
            spec_.case_sensitive = !spec_.case_sensitive;
        } else if (id == kHitClear) {
            const auto current = spec_.current_folder;
            spec_ = {};
            spec_.current_folder = current;
            if (!current.empty()) spec_.location = app::LocationScope::CurrentFolder;
            for (HWND edit : {edit_kw_, edit_exts_, edit_exclude_, edit_name_also_})
                if (edit) SetWindowTextW(edit, L"");
            SetContentMode(false);
            if (edit_kw_) SetFocus(edit_kw_);
        } else if (id >= kHitRecipe && id < kHitRecipe + 4) {
            ApplyRecipe(id - kHitRecipe);
        } else if (id == kHitMode || id == kHitMode + 1) {
            SetContentMode(id == kHitMode + 1);
            if (edit_kw_) SetFocus(edit_kw_);
        } else if (id >= kHitType && id < kHitType + 100) {
            const auto kind = static_cast<index::SearchKind>(id - kHitType);
            if (kind == index::SearchKind::Custom) {
                more_ = true;
                Recompute();
                if (edit_exts_) SetFocus(edit_exts_);
                return;
            }
            spec_.kind = kind;
            spec_.custom_exts.clear();
            if (edit_exts_) SetWindowTextW(edit_exts_, L"");
        } else if (id >= kHitDate && id < kHitDate + 100) {
            spec_.date = static_cast<app::DatePreset>(id - kHitDate);
        } else if (id >= kHitSize && id < kHitSize + 100) {
            spec_.size = static_cast<app::SizePreset>(id - kHitSize);
        } else if (id >= kHitLocation && id < kHitLocation + 100) {
            const int v = id - kHitLocation;
            if (v == 2) {
                FolderPickerSpec picker;
                picker.title = l10n::Get(I::AdvSearchBrowse);
                picker.initial_path = spec_.custom_folder;
                std::wstring folder;
                if (!ShowFolderPicker(hwnd_, picker, dark_, accent_, folder)) return;
                spec_.custom_folder = folder;
                spec_.location = app::LocationScope::CustomFolder;
            } else {
                spec_.location = v == 1 ? app::LocationScope::CurrentFolder : app::LocationScope::Indexed;
            }
        } else if (id >= kHitNameHow && id < kHitNameHow + 100) {
            spec_.name_how = static_cast<app::NameMatchHow>(id - kHitNameHow);
        } else if (id >= kHitContentMode && id < kHitContentMode + 100) {
            spec_.content_mode = static_cast<index::ContentMatchMode>(id - kHitContentMode);
        }
        Recompute();
    }

    void Complete(bool accepted) {
        if (accepted) {
            Recompute();
            if (scope_error_) return;
            result_.accepted = true;
            result_.query = preview_;
        }
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    int Hit(float x, float y) const {
        if (ContainsRect(CloseRect(), x, y)) return kHitClose;
        if (ContainsRect(search_rect_, x, y)) return kHitSearch;
        if (ContainsRect(cancel_rect_, x, y)) return kHitCancel;
        if (ContainsRect(clear_rect_, x, y)) return kHitClear;
        if (ContainsRect(more_rect_, x, y)) return kHitMore;
        if (Visible(ww_rect_) && ContainsRect(ww_rect_, x, y)) return kHitWholeWord;
        if (Visible(mc_rect_) && ContainsRect(mc_rect_, x, y)) return kHitMatchCase;
        for (int i = 0; i < 2; ++i)
            if (ContainsRect(mode_rect_[i], x, y)) return kHitMode + i;
        for (const auto& pill : pills_)
            if (pill.enabled && ContainsRect(pill.bounds, x, y)) return pill.id;
        return kHitNone;
    }

    // ---- painting ------------------------------------------------------------------

    void Render() {
        if (!compositor_.Dc()) return;
        Theme theme = MakeTheme(dark_, accent_);
        if (IsHighContrast()) theme = MakeHighContrastTheme();
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        BeginSurface(compositor_, painter_, theme, dark_, IsHighContrast(), backdrop_, scale_);

        painter_.DrawText(l10n::Get(I::AdvancedSearch), R(kPad, 12, 300, 26),
                          compositor_.HeaderFormat(), theme.text);

        for (const auto& pill : pills_) {
            fluent::ButtonSpec button;
            button.bounds = pill.bounds;
            button.text = pill.text;
            button.glyph = pill.glyph;
            button.kind = fluent::ButtonKind::Toggle;
            button.state.selected = button.state.checked = pill.selected;
            button.state.hovered = hover_ == pill.id;
            button.state.pressed = pressed_ == pill.id;
            button.state.enabled = pill.enabled;
            painter_.DrawButton(button);
        }
        for (const auto& row : rows_) {
            const float h = row.bounds.bottom - row.bounds.top;
            const float line = 18.0f * scale_;
            painter_.DrawText(row.text, D2D1::RectF(row.bounds.left, row.bounds.top + (h - line) * 0.5f,
                              row.bounds.right, row.bounds.top + (h + line) * 0.5f),
                              compositor_.SmallFormat(), theme.text_secondary);
        }

        auto field = [&](const D2D1_RECT_F& bounds, HWND edit, I placeholder) {
            if (!Visible(bounds) || !edit) return;
            fluent::TextFieldSpec spec;
            spec.bounds = bounds;
            spec.hosted_edit = true;
            spec.suppress_text = !EditText(edit).empty();
            spec.placeholder = l10n::Get(placeholder);
            spec.leading_glyph = edit == edit_kw_ ? L"\xE721" : L"";
            spec.state.focused = GetFocus() == edit;
            painter_.DrawTextField(spec);
        };
        field(kw_rect_, edit_kw_, KeywordHint());
        field(ext_rect_, edit_exts_, I::AdvSearchExtHint);
        field(exclude_rect_, edit_exclude_, ExcludeHint());
        field(name_also_rect_, edit_name_also_, I::AdvNameAlso);

        const I mode_labels[2] = {I::AdvModeName, I::AdvModeContent};
        const wchar_t* mode_glyphs[2] = {L"\xE8D2", L"\xE8A5"};
        for (int i = 0; i < 2; ++i) {
            // Same toggle look as the pills: the chosen mode reads as "on", not disabled.
            fluent::ButtonSpec segment;
            segment.bounds = mode_rect_[i];
            segment.text = l10n::Get(mode_labels[i]);
            segment.glyph = mode_glyphs[i];
            segment.kind = fluent::ButtonKind::Toggle;
            segment.state.selected = segment.state.checked = content_mode_ == (i == 1);
            segment.state.hovered = hover_ == kHitMode + i;
            painter_.DrawButton(segment);
        }

        painter_.DrawText(more_text_, more_rect_, compositor_.SmallFormat(),
                          hover_ == kHitMore ? theme.text : theme.accent);
        if (Visible(ww_rect_)) {
            fluent::ControlState state;
            state.checked = spec_.whole_word;
            state.hovered = hover_ == kHitWholeWord;
            painter_.DrawCheckBox(ww_rect_, l10n::Get(I::AdvSearchWholeWord), state);
        }
        if (Visible(mc_rect_)) {
            fluent::ControlState state;
            state.checked = spec_.case_sensitive;
            state.hovered = hover_ == kHitMatchCase;
            painter_.DrawCheckBox(mc_rect_, l10n::Get(I::AdvSearchMatchCase), state);
        }

        // Footer band.
        {
            auto band = theme.text;
            band.a = 0.04f;
            painter_.FillRoundedRect(D2D1::RectF(0, footer_top_, kDlgW * scale_, height_px_), 0.0f, band);
            auto line = theme.text;
            line.a = 0.10f;
            painter_.FillRoundedRect(D2D1::RectF(0, footer_top_, kDlgW * scale_, footer_top_ + 1.0f), 0.0f, line);
        }
        painter_.DrawWrappedCaption(summary_, summary_origin_,
                                    (kDlgW - 2.0f * kPad) * scale_, theme.text);
        bool danger = false;
        const std::wstring count = CountLine(danger);
        if (!count.empty()) {
            const float line = 18.0f * scale_;
            const float mid = (count_rect_.top + count_rect_.bottom) * 0.5f;
            painter_.DrawText(count, D2D1::RectF(count_rect_.left, mid - line * 0.5f,
                              count_rect_.right, mid + line * 0.5f),
                              compositor_.SmallFormat(), danger ? theme.danger : theme.text_secondary);
        }

        fluent::ButtonSpec clear;
        clear.bounds = clear_rect_;
        clear.text = l10n::Get(I::ClearAll);
        clear.kind = fluent::ButtonKind::Transparent;
        clear.state.hovered = hover_ == kHitClear;
        painter_.DrawButton(clear);
        fluent::ButtonSpec cancel;
        cancel.bounds = cancel_rect_;
        cancel.text = l10n::Get(I::Cancel);
        cancel.state.hovered = hover_ == kHitCancel;
        painter_.DrawButton(cancel);
        fluent::ButtonSpec search;
        search.bounds = search_rect_;
        search.text = l10n::Get(I::AdvSearchRun);
        search.kind = fluent::ButtonKind::Primary;
        search.state.hovered = hover_ == kHitSearch;
        search.state.enabled = !scope_error_;
        painter_.DrawButton(search);

        fluent::ControlState close_state{};
        close_state.hovered = hover_ == kHitClose;
        close_state.pressed = pressed_ == kHitClose;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close_state);
        EndSurface(compositor_);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE: {
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            backdrop_ = ApplyBackdrop(hwnd_, dark_);
            font_ = typography::CreateEditFont(scale_);
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            edit_brush_ = CreateSolidBrush(dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255));
            edit_kw_ = CreateField(1, content_mode_ ? spec_.content : spec_.name, KeywordHint());
            edit_exts_ = CreateField(2, spec_.kind == index::SearchKind::Custom ? spec_.custom_exts : L"",
                                     I::AdvSearchExtHint);
            edit_exclude_ = CreateField(3, content_mode_ ? spec_.content_exclude : spec_.exclude_name,
                                        ExcludeHint());
            edit_name_also_ = CreateField(4, content_mode_ ? spec_.name : L"", I::AdvNameAlso);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd_, kCountTimer);
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            compositor_.Shutdown();
            done_ = true;
            return 0;
        case WM_TIMER:
            if (wparam == kCountTimer) {
                KillTimer(hwnd_, kCountTimer);
                count_timer_ = false;
                if (count_request_ && !content_mode_ && !scope_error_) {
                    counted_query_ = preview_;
                    count_request_(hwnd_, preview_);
                }
                return 0;
            }
            break;
        case kAdvancedSearchCountMessage:
            if (!count_timer_) {  // A newer request is already queued: wait for it.
                count_ = static_cast<size_t>(wparam);
                count_known_ = true;
                count_pending_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_DPICHANGED: {
            const auto* rc = reinterpret_cast<RECT*>(lparam);
            scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            Layout();
            SetWindowPos(hwnd_, nullptr, rc->left, rc->top, static_cast<int>(kDlgW * scale_),
                         static_cast<int>(std::ceil(height_px_)), SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutEdits();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            LayoutEdits();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            LayoutEdits();
            return 0;
        case WM_CTLCOLOREDIT: {
            const HDC hdc = reinterpret_cast<HDC>(wparam);
            SetTextColor(hdc, EditTextColor(dark_));
            SetBkColor(hdc, EditBackColor(dark_));
            return reinterpret_cast<LRESULT>(EditBackBrush(edit_brush_));
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK) { Complete(true); return 0; }
            if (LOWORD(wparam) == IDCANCEL) { Complete(false); return 0; }
            if (HIWORD(wparam) == EN_CHANGE) Recompute();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            Render();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return BorderlessHitTest(hwnd_, lparam, 36 * scale_, CloseRect());
        case WM_CLOSE:
            Complete(false);
            return 0;
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                 static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) {
                hover_ = next;
                SetCursor(LoadCursorW(nullptr, next ? IDC_HAND : IDC_ARROW));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && hover_ != kHitNone) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            HWND edit = nullptr;
            if (ContainsRect(kw_rect_, x, y)) edit = edit_kw_;
            else if (Visible(ext_rect_) && ContainsRect(ext_rect_, x, y)) edit = edit_exts_;
            else if (Visible(exclude_rect_) && ContainsRect(exclude_rect_, x, y)) edit = edit_exclude_;
            else if (Visible(name_also_rect_) && ContainsRect(name_also_rect_, x, y)) edit = edit_name_also_;
            if (edit) {
                // The rounded field includes padding outside the native text line.
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                MapWindowPoints(hwnd_, edit, &point, 1);
                SendMessageW(edit, WM_LBUTTONDOWN, wparam, MAKELPARAM(point.x, point.y));
                return 0;
            }
            pressed_ = Hit(x, y);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const int id = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                               static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (id != pressed || id == kHitNone) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (id == kHitSearch) Complete(true);
            else if (id == kHitCancel || id == kHitClose) Complete(false);
            else Click(id);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) Complete(false);
            else if (wparam == VK_RETURN) Complete(true);
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND edit_kw_ = nullptr;
    HWND edit_exts_ = nullptr;
    HWND edit_exclude_ = nullptr;
    HWND edit_name_also_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    app::AdvancedSearchSpec spec_;
    AdvancedSearchDialogResult result_;
    AdvancedSearchCountRequest count_request_;
    std::wstring preview_;
    std::wstring counted_query_;
    std::wstring summary_;
    std::wstring more_text_;
    std::vector<Pill> pills_;
    std::vector<RowLabel> rows_;
    D2D1_RECT_F kw_rect_{}, ext_rect_{}, exclude_rect_{}, name_also_rect_{};
    D2D1_RECT_F mode_rect_[2]{};
    D2D1_RECT_F more_rect_{}, ww_rect_{}, mc_rect_{};
    D2D1_RECT_F search_rect_{}, cancel_rect_{}, clear_rect_{}, count_rect_{};
    D2D1_POINT_2F summary_origin_{};
    float footer_top_ = 0.0f;
    float height_px_ = 0.0f;
    size_t count_ = 0;
    bool count_known_ = false;
    bool count_pending_ = false;
    bool count_timer_ = false;
    bool content_mode_ = false;
    bool more_ = false;
    bool dark_ = true;
    bool backdrop_ = false;
    bool done_ = false;
    bool scope_error_ = false;
    float scale_ = 1.0f;
    D2D1_COLOR_F accent_{};
    int hover_ = 0;
    int pressed_ = 0;
};

} // namespace

AdvancedSearchDialogResult ShowAdvancedSearchDialog(HWND owner, app::AdvancedSearchSpec spec,
                                                    bool dark, D2D1_COLOR_F accent,
                                                    AdvancedSearchCountRequest count) {
    AdvancedSearchWindow window;
    return window.Show(owner, std::move(spec), dark, accent, std::move(count));
}

} // namespace pulse::ui
