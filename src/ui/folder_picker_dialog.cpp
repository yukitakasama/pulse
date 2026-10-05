#include "edit_host.h"
#include "../common/windows_compat.h"
#include "folder_picker_dialog.h"

#include "folder_picker_loader.h"
#include "folder_picker_view.h"
#include "folder_picker_art.h"
#include "fluent_menu.h"
#include "typography.h"
#include "window_helpers.h"
#include "../common/display_path.h"
#include "../common/localization.h"

#include <commctrl.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace pulse::ui {
namespace {

constexpr wchar_t kPickerClass[] = L"PulseFolderPickerWindow";
constexpr UINT kListingMessage = WM_APP + 1;
constexpr UINT kValidationMessage = WM_APP + 2;
constexpr UINT_PTR kSearchTimer = 2;
constexpr int kPathEditId = 101;
constexpr int kFilenameEditId = 102;
constexpr int kSearchEditId = 103;
constexpr UINT_PTR kLoadTimer = 1;
constexpr UINT kSpinnerDelayMs = 150;
constexpr UINT kSpinnerFrameMs = 33;
constexpr float kDefaultWidth = 780.0f;
constexpr float kDefaultHeight = 600.0f;
constexpr float kMinWidth = 620.0f;
constexpr float kMinHeight = 540.0f;

// Where the last pick of each mode was made, for this session.
std::wstring g_last_folder[3];
PickerViewMode g_last_view[3] = {PickerViewMode::Details, PickerViewMode::LargeIcons, PickerViewMode::Details};

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw)
        path = raw;
    if (raw) CoTaskMemFree(raw);
    return path;
}

std::vector<PickerPlace> BuildPlaces() {
    struct Known {
        const KNOWNFOLDERID* id;
        l10n::StringId label;
        const wchar_t* glyph;
        uint32_t rgb;
    };
    const Known known[] = {
        {&FOLDERID_Desktop, l10n::StringId::Desktop, L"\xE7F4", 0x38BDF8},
        {&FOLDERID_Documents, l10n::StringId::PickerDocuments, L"\xE8A5", 0x60A5FA},
        {&FOLDERID_Downloads, l10n::StringId::Downloads, L"\xE896", 0xC084FC},
        {&FOLDERID_Pictures, l10n::StringId::PickerPictures, L"\xEB9F", 0xF472B6},
        {&FOLDERID_Profile, l10n::StringId::PickerHome, L"\xE80F", 0x34D399},
    };
    std::vector<PickerPlace> places;
    for (const Known& k : known) {
        std::wstring path = KnownFolder(*k.id);
        if (path.empty()) continue;
        places.push_back({l10n::Get(k.label), std::move(path), k.glyph, HexColor(k.rgb), false});
    }
    places.push_back({l10n::Get(l10n::StringId::ThisPc), L"", L"\xE977", {}, true});
    return places;
}

// "\\\\server\\share" reads better whole than as just "share".
bool IsShareRootName(const std::wstring& path) {
    return path.rfind(L"\\\\", 0) == 0 && PickerParent(path).empty();
}

std::wstring ErrorText(DWORD error, const std::wstring& path) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_DIRECTORY: {
        std::wstring text = l10n::Get(l10n::StringId::PickerNotFound);
        const size_t at = text.find(L"{path}");
        // The full path is already in the path field; the one-line message
        // names the missing folder itself.
        std::wstring name = path::FriendlyPathText(path);
        while (name.size() > 3 && name.back() == L'\\') name.pop_back();
        const size_t slash = name.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash + 1 < name.size() && !IsShareRootName(name))
            name = name.substr(slash + 1);
        if (at != std::wstring::npos) text.replace(at, 6, name);
        return text;
    }
    default:
        break;
    }
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length && buffer ? std::wstring(buffer, length) : std::wstring();
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' ||
                             text.back() == L' ' || text.back() == L'.' ||
                             text.back() == 0x3002))
        text.pop_back();
    return text;
}

class FolderPickerWindow {
public:
    bool Show(HWND owner, const FolderPickerSpec& spec, bool dark, D2D1_COLOR_F accent,
              FilePickerResult& out) {
        owner_ = owner;
        dark_ = dark;
        accent_ = accent;
        mode_index_ = spec.mode == PickerMode::Image ? 1 : spec.mode == PickerMode::File ? 2 : 0;
        allow_multiselect_ = spec.allow_multiselect && spec.mode != PickerMode::Folder;
        options_.filters = spec.filters;
        options_.show_hidden = spec.show_hidden;
        if (options_.filters.empty() && spec.mode != PickerMode::Folder) {
            if (spec.mode == PickerMode::Image) {
                options_.filters = {{l10n::Pick(L"支持的图片", L"Supported images"), L"*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.jfif"},
                    {L"JPEG (*.jpg; *.jpeg; *.jfif)", L"*.jpg;*.jpeg;*.jfif"}, {L"PNG (*.png)", L"*.png"},
                    {L"WebP (*.webp)", L"*.webp"}, {L"BMP (*.bmp)", L"*.bmp"}};
            } else options_.filters = {{l10n::Pick(L"所有文件 (*.*)", L"All files (*.*)"), L"*.*"}};
        }
        options_.filter_index = options_.filters.empty() ? 0 : (std::min)(spec.filter_index, options_.filters.size() - 1);
        visual_.view = g_last_view[mode_index_];
        visual_.show_hidden = options_.show_hidden;
        visual_.filename_text = spec.filename;
        if (!options_.filters.empty()) visual_.filter_text = options_.filters[options_.filter_index].label;
        visual_.mode = spec.mode;
        visual_.title = !spec.title.empty() ? spec.title
            : spec.mode == PickerMode::File ? l10n::Pick(L"打开", L"Open")
            : l10n::Get(spec.mode == PickerMode::Image
                            ? l10n::StringId::TooltipChooseBackground
                            : l10n::StringId::PickerTitleFolder);
        visual_.primary_text = l10n::Get(spec.mode != PickerMode::Folder
                                             ? l10n::StringId::PickerSelect
                                             : l10n::StringId::PickerSelectFolder);
        visual_.cancel_text = l10n::Get(l10n::StringId::Cancel);
        visual_.places = BuildPlaces();
        visual_.hosted_edit = true;
        visual_.focus = kPickList;
        fallback_ = KnownFolder(spec.mode == PickerMode::Image ? FOLDERID_Pictures
                                                               : FOLDERID_Desktop);
        std::wstring start = NormalizePickerInput(spec.initial_path);
        if (start.empty()) start = g_last_folder[mode_index_];
        if (start.empty()) start = fallback_;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow()))
               / 96.0f;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kPickerClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, kPickerClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kDefaultWidth * scale_);
        const int height = static_cast<int>(kDefaultHeight * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kPickerClass, visual_.title.c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
            owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return false;
        // WM_CREATE read the DPI of the monitor the window landed on, which
        // can differ from the owner's guess above.
        CenterOwnedWindow(hwnd_, owner_, static_cast<int>(kDefaultWidth * scale_),
                          static_cast<int>(kDefaultHeight * scale_));
        loader_ = std::make_unique<PickerLoader>(hwnd_, kListingMessage, kValidationMessage);
        initial_load_ = true;
        Navigate(start, false);
        const bool owner_enabled = owner_ && IsWindowEnabled(owner_);
        if (owner_enabled) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        PresentEdit();

        MSG message{};
        while (!done_) {
            const BOOL got = GetMessageW(&message, nullptr, 0, 0);
            if (got <= 0) { if (got == 0) PostQuitMessage(static_cast<int>(message.wParam)); break; }
            RedirectStrayModalKey(message, hwnd_);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Close();
        if (owner_enabled) EnableWindow(owner_, TRUE);
        if (result_.empty()) return false;
        out.paths = result_;
        out.filter_index = options_.filter_index;
        return true;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<FolderPickerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<FolderPickerWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // ---- navigation -------------------------------------------------------

    // By value: callers pass entry paths that the reset below destroys.
    // `verified`: the folder is known to exist (it was just listed as a row, or
    // is the parent of a folder that listed), so it may be picked while it
    // loads. Typed, remembered and history paths wait for their listing.
    void Navigate(std::wstring path, bool push_history, std::wstring select_after = {},
                  bool verified = false) {
        if (push_history && !SamePickerPath(path, visual_.current))
            history_.Navigate(visual_.current);
        visual_.current = path;
        current_verified_ = verified;
        visual_.entries.clear();
        typeahead_.clear();
        visual_.selected = -1;
        visual_.selected_indices.clear();
        selection_anchor_ = -1;
        visual_.pressed = kPickNone;
        visual_.validating = false;
        visual_.notice.clear();
        visual_.scroll = 0.0f;
        visual_.error.clear();
        visual_.waiting = true;
        visual_.loading = false;
        visual_.can_back = history_.CanGoBack();
        visual_.can_forward = history_.CanGoForward();
        visual_.can_up = !path.empty();
        select_after_ = std::move(select_after);
        load_started_ = GetTickCount64();
        SetEditText(DisplayPath(path));
        UpdateChosen();
        if (loader_) loader_->Load(++generation_, path, visual_.mode, options_);
        if (hwnd_) {
            SetTimer(hwnd_, kLoadTimer, kSpinnerDelayMs, nullptr);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void GoUp() {
        if (visual_.current.empty()) return;
        const std::wstring child = visual_.current;
        Navigate(PickerParent(child), true, child, current_verified_);
    }

    void GoBack() {
        if (!history_.CanGoBack()) return;
        const std::wstring child = visual_.current;
        const std::wstring target = history_.Back(visual_.current);
        Navigate(target, false, child);
    }

    void GoForward() {
        if (!history_.CanGoForward()) return;
        Navigate(history_.Forward(visual_.current), false);
    }

    void Reload() {
        const std::wstring keep = SelectedEntry() ? SelectedEntry()->path : std::wstring();
        Navigate(visual_.current, false, keep);
    }

    void OnListing(std::unique_ptr<PickerListing> listing) {
        if (!listing || listing->generation != generation_) return;
        KillTimer(hwnd_, kLoadTimer);
        visual_.waiting = false;
        visual_.loading = false;
        current_verified_ = listing->error == ERROR_SUCCESS;
        if (listing->error != ERROR_SUCCESS) {
            if (initial_load_ && !fallback_.empty() &&
                !SamePickerPath(listing->path, fallback_)) {
                // A remembered or suggested folder that is gone: start from
                // the default place instead of an error page.
                Navigate(fallback_, false);
                return;
            }
            visual_.error = ErrorText(listing->error, listing->path);
            if (visual_.error.empty())
                visual_.error = l10n::Get(l10n::StringId::PickerOpenFailed);
        }
        initial_load_ = false;
        visual_.entries = std::move(listing->entries);
        if (!select_after_.empty()) {
            for (size_t i = 0; i < visual_.entries.size(); ++i) {
                if (SamePickerPath(visual_.entries[i].path, select_after_)) {
                    Select(static_cast<int>(i));
                    break;
                }
            }
            select_after_.clear();
        }
        UpdateChosen();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    const PickerEntry* SelectedEntry() const {
        if (visual_.selected < 0 || visual_.selected >= static_cast<int>(visual_.entries.size()))
            return nullptr;
        return &visual_.entries[static_cast<size_t>(visual_.selected)];
    }

    static std::wstring ReadEdit(HWND edit) {
        const int length = GetWindowTextLengthW(edit);
        std::wstring text(static_cast<size_t>((std::max)(length, 0)) + 1, L'\0');
        GetWindowTextW(edit, text.data(), length + 1);
        text.resize(static_cast<size_t>((std::max)(length, 0)));
        return text;
    }

    void UpdateChosen() {
        if (filename_edit_) EnableWindow(filename_edit_, !visual_.validating && !visual_.waiting);
        visual_.chosen.clear();
        if (visual_.validating || visual_.waiting) return;
        if (visual_.mode != PickerMode::Folder && !visual_.filename_text.empty()) {
            visual_.chosen = visual_.filename_text;
            return;
        }
        if (visual_.mode != PickerMode::Folder) return;
        if (visual_.error.empty() && !visual_.waiting)
            visual_.chosen = PickerChosenPath(visual_.mode, visual_.current, SelectedEntry());
    }

    void SetFilename(std::wstring text) {
        visual_.filename_text = std::move(text);
        setting_edits_ = true;
        if (filename_edit_) SetWindowTextW(filename_edit_, visual_.filename_text.c_str());
        setting_edits_ = false;
        UpdateChosen();
    }

    void SyncSelectedNames() {
        if (visual_.mode == PickerMode::Folder) return;
        std::wstring text;
        for (int i : visual_.selected_indices) {
            if (i < 0 || i >= static_cast<int>(visual_.entries.size())) continue;
            if (!text.empty()) text += L" ";
            const auto& name = visual_.entries[static_cast<size_t>(i)].name;
            text += visual_.selected_indices.size() > 1 ? L"\"" + name + L"\"" : name;
        }
        SetFilename(std::move(text));
    }

    void Select(int index, bool extend = false, bool toggle = false, bool focus_only = false) {
        if (visual_.validating) { ++generation_; visual_.validating = false; }
        const int count = static_cast<int>(visual_.entries.size());
        if (!count) { visual_.selected = -1; visual_.selected_indices.clear(); UpdateChosen(); return; }
        index = std::clamp(index, 0, count - 1);
        visual_.selected = index;
        if (!focus_only) {
            if (allow_multiselect_ && extend && selection_anchor_ >= 0) {
                if (!toggle) visual_.selected_indices.clear();
                for (int i = (std::min)(selection_anchor_, index); i <= (std::max)(selection_anchor_, index); ++i)
                    if (std::find(visual_.selected_indices.begin(), visual_.selected_indices.end(), i) == visual_.selected_indices.end())
                        visual_.selected_indices.push_back(i);
            } else if (allow_multiselect_ && toggle) {
                auto it = std::find(visual_.selected_indices.begin(), visual_.selected_indices.end(), index);
                if (it == visual_.selected_indices.end()) visual_.selected_indices.push_back(index);
                else visual_.selected_indices.erase(it);
                selection_anchor_ = index;
            } else { visual_.selected_indices = {index}; selection_anchor_ = index; }
            std::sort(visual_.selected_indices.begin(), visual_.selected_indices.end());
            SyncSelectedNames();
        }
        visual_.scroll = ScrollPickerRowIntoView(layout_, visual_.entries.size(), visual_.scroll, index);
        visual_.notice.clear();
        UpdateChosen();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OpenEntry(int index) {
        if (index < 0 || index >= static_cast<int>(visual_.entries.size())) return;
        const auto entry = visual_.entries[static_cast<size_t>(index)];
        if (entry.kind == PickerEntryKind::Image || entry.kind == PickerEntryKind::File) {
            SubmitSelection();
        } else { SetFilename(L""); Navigate(entry.path, true, {}, true); }
    }

    void ValidateNames(std::vector<std::wstring> names) {
        if (!loader_ || visual_.validating || visual_.waiting || names.empty()) return;
        if (!allow_multiselect_ && names.size() > 1) {
            visual_.notice = l10n::Pick(L"此处只能选择一个文件。", L"Select one file here.");
            InvalidateRect(hwnd_, nullptr, FALSE); return;
        }
        validation_navigation_only_ = false;
        visual_.validating = true;
        visual_.notice = l10n::Pick(L"正在检查所选项目…", L"Checking the selected items…");
        UpdateChosen();
        loader_->Validate(++generation_, visual_.current, std::move(names), visual_.mode, options_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SubmitSelection() {
        if (visual_.validating) return;
        std::vector<std::wstring> names;
        if (visual_.mode != PickerMode::Folder && !visual_.filename_text.empty()) {
            if (!ParsePickerNames(visual_.filename_text, names)) {
                visual_.notice = l10n::Pick(L"请为每个文件名使用配对的引号。", L"Use matching quotes around each filename.");
                InvalidateRect(hwnd_, nullptr, FALSE); return;
            }
        } else if (!visual_.chosen.empty()) names.push_back(visual_.chosen);
        ValidateNames(std::move(names));
    }

    void OnValidation(std::unique_ptr<PickerValidation> validation) {
        if (!validation || validation->generation != generation_ || !visual_.validating) return;
        visual_.validating = false;
        visual_.notice.clear();
        if (validation->error != ERROR_SUCCESS) {
            visual_.notice = validation->error == ERROR_UNSUPPORTED_TYPE
                ? l10n::Pick(L"所选文件不符合当前文件类型。", L"The selected file does not match the file type.")
                : ErrorText(validation->error, validation->failed_path);
            if (visual_.notice.empty()) visual_.notice = l10n::Get(l10n::StringId::PickerOpenFailed);
            if (visual_.waiting && loader_) loader_->Load(++generation_, visual_.current, visual_.mode, options_);
            UpdateChosen(); InvalidateRect(hwnd_, nullptr, FALSE); return;
        }
        if (!validation->navigate.empty()) {
            SetFilename(L""); Navigate(validation->navigate, true); FocusList(); return;
        }
        if (validation->paths.empty()) { UpdateChosen(); return; }
        if (validation_navigation_only_) {
            Navigate(PickerParent(validation->paths.front()), true, validation->paths.front());
            FocusList(); return;
        }
        result_ = std::move(validation->paths);
        g_last_folder[mode_index_] = visual_.current.empty() ? PickerParent(result_.front()) : visual_.current;
        g_last_view[mode_index_] = visual_.view;
        done_ = true;
        PostMessageW(hwnd_, WM_NULL, 0, 0);
    }

    void Cancel() {
        result_.clear();
        done_ = true;
        PostMessageW(hwnd_, WM_NULL, 0, 0);
    }

    void Close() {
        if (!hwnd_) return;
        KillTimer(hwnd_, kLoadTimer);
        KillTimer(hwnd_, kSearchTimer);
        // Stop the loader first: after this no listing can be posted, so the
        // ones already queued are all that must be freed.
        loader_.reset();
        MSG pending{};
        while (PeekMessageW(&pending, hwnd_, kListingMessage, kListingMessage, PM_REMOVE))
            PickerLoader::Take(pending.lParam);
        while (PeekMessageW(&pending, hwnd_, kValidationMessage, kValidationMessage, PM_REMOVE))
            PickerLoader::TakeValidation(pending.lParam);
        art_.Close();
        if (IsWindow(hwnd_)) {
            HideComposedDialog(hwnd_, owner_);
            DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
    }

    std::wstring DisplayPath(const std::wstring& path) const {
        return path.empty() ? l10n::Get(l10n::StringId::ThisPc) : path::FriendlyPathText(path);
    }

    // ---- path field --------------------------------------------------------

    D2D1_COLOR_F EditForeground() const { return ColorFromRef(EditTextColor(dark_)); }
    D2D1_COLOR_F EditBackground() const { return ColorFromRef(EditBackColor(dark_)); }

    HBRUSH EditBrush() const { return EditBackBrush(edit_brush_); }

    void CreateFonts() {
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        font_ = typography::CreateEditFont(scale_);
        for (HWND edit : {edit_, filename_edit_, search_edit_})
            if (edit && font_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

    HWND MakeEdit(int id, const wchar_t* text) {
        HWND edit = CreateChildEdit(hwnd_, text);
        if (!edit) return nullptr;
        SetWindowLongPtrW(edit, GWLP_ID, id);
        SetWindowTheme(edit, L"", L"");
        if (!compositor_.CustomEditEnabled()) SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA);
        if (font_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
        return edit;
    }

    void CreateEdit() {
        edit_ = MakeEdit(kPathEditId, L"");
        if (visual_.mode != PickerMode::Folder) {
            filename_edit_ = MakeEdit(kFilenameEditId, visual_.filename_text.c_str());
            if (filename_edit_) SendMessageW(filename_edit_, EM_SETCUEBANNER, TRUE,
                reinterpret_cast<LPARAM>(l10n::Pick(L"文件名", L"File name")));
        }
        search_edit_ = MakeEdit(kSearchEditId, L"");
        if (search_edit_) SendMessageW(search_edit_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(l10n::Pick(L"搜索当前文件夹", L"Search this folder")));
    }

    void SetEditText(const std::wstring& text) {
        edit_text_ = text;
        if (!edit_) return;
        setting_edits_ = true;
        SetWindowTextW(edit_, text.c_str());
        setting_edits_ = false;
        PresentEdit();
    }

    void PresentEdit() {
        for (HWND edit : {edit_, filename_edit_, search_edit_})
            if (edit && IsWindowVisible(edit))
                PresentChildEdit(compositor_, compositor_.TextFormat(), EditForeground(), EditBackground(), edit);
    }

    void PlaceOneEdit(HWND edit, const D2D1_RECT_F& cell) {
        if (!edit || !hwnd_) return;
        const int x = static_cast<int>(std::lround(cell.left + 10.0f * scale_));
        const int w = std::max(40, static_cast<int>(std::lround(cell.right - cell.left -
                                                                20.0f * scale_)));
        const int cell_h = std::max(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
        const int line_h = EditLineHeight(edit, font_, cell_h);
        const int y = static_cast<int>(std::lround(cell.top)) + std::max(0, (cell_h - line_h) / 2);
        SetWindowPos(edit, HWND_TOP, x, y, w, line_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void PlaceEdit() {
        PlaceOneEdit(edit_, layout_.path);
        PlaceOneEdit(filename_edit_, layout_.filename);
        PlaceOneEdit(search_edit_, layout_.search);
    }

    void FocusPath() {
        if (!edit_) return;
        SetFocus(edit_);
        SendMessageW(edit_, EM_SETSEL, 0, -1);
    }

    void FocusList() {
        visual_.focus = kPickList;
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SubmitPath() {
        const auto text = ReadEdit(edit_);
        if (text == l10n::Get(l10n::StringId::ThisPc) || text.empty()) {
            Navigate(L"", true); FocusList(); return;
        }
        validation_navigation_only_ = true;
        visual_.validating = true;
        visual_.notice = l10n::Pick(L"正在检查路径…", L"Checking the path…");
        PickerOptions all;
        loader_->Validate(++generation_, visual_.current, {text}, PickerMode::File, all);
        UpdateChosen(); FocusList();
    }

    void InputChanged(HWND edit) {
        if (setting_edits_) return;
        visual_.pressed = kPickNone;
        if (visual_.validating) { ++generation_; visual_.validating = false; }
        visual_.notice.clear();
        if (edit == filename_edit_) {
            visual_.filename_text = ReadEdit(edit);
            visual_.selected_indices.clear();
        } else if (edit == search_edit_) {
            visual_.search_text = ReadEdit(edit);
            SetTimer(hwnd_, kSearchTimer, 180, nullptr);
        }
        UpdateChosen();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<FolderPickerWindow*>(ref);
        if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
        switch (msg) {
        case WM_KEYDOWN:
            if (wparam == VK_RETURN) {
                if (hwnd == self->edit_) self->SubmitPath();
                else if (hwnd == self->filename_edit_) self->SubmitSelection();
                else { KillTimer(self->hwnd_, kSearchTimer); self->options_.search = ReadEdit(hwnd); self->Reload(); self->FocusList(); }
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                if (hwnd == self->edit_) { self->SetEditText(self->edit_text_); self->FocusList(); }
                else if (hwnd == self->search_edit_ && !ReadEdit(hwnd).empty()) SetWindowTextW(hwnd, L"");
                else self->Cancel();
                return 0;
            }
            if (wparam == VK_TAB) { self->MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return 0; }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == 'L') { self->FocusPath(); return 0; }
            break;
        case WM_SYSKEYDOWN:
            if (wparam == VK_LEFT) { self->GoBack(); return 0; }
            if (wparam == VK_RIGHT) { self->GoForward(); return 0; }
            if (wparam == VK_UP) { self->GoUp(); return 0; }
            if (wparam == 'D') { self->FocusPath(); return 0; }
            break;
        case WM_CHAR:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB) return 0;
            break;
        case WM_SETFOCUS:
            self->visual_.path_focused = hwnd == self->edit_;
            self->visual_.focus = hwnd == self->edit_ ? kPickPath : hwnd == self->filename_edit_ ? kPickFilename : kPickSearch;
            break;
        case WM_KILLFOCUS:
            if (hwnd == self->edit_) self->visual_.path_focused = false;
            break;
        }
        const bool repaint = msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
                             msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK;
        LRESULT result = 0;
        if (!HandleChildEditMessage(self->compositor_, self->compositor_.TextFormat(),
                                    self->EditForeground(), self->EditBackground(),
                                    self->EditBrush(), hwnd, msg, wparam, lparam, result)) {
            result = DefPresentedChildEditProc(self->compositor_, self->compositor_.TextFormat(),
                                               self->EditForeground(), self->EditBackground(),
                                               hwnd, msg, wparam, lparam);
        }
        if (repaint && self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        return result;
    }

    void UpdateTooltips() {
        if (!tooltip_) {
            tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!tooltip_) return;
            SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, static_cast<LPARAM>(420 * scale_));
            SetWindowTheme(tooltip_, dark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        }
        struct Tip { int id; D2D1_RECT_F rect; const wchar_t* text; };
        const Tip tips[] = {
            {kPickBack, layout_.back, l10n::Pick(L"后退 (Alt+←)", L"Back (Alt+Left)")},
            {kPickForward, layout_.forward, l10n::Pick(L"前进 (Alt+→)", L"Forward (Alt+Right)")},
            {kPickUp, layout_.up, l10n::Pick(L"上一级 (Alt+↑)", L"Up (Alt+Up)")},
            {kPickRefresh, layout_.refresh, l10n::Pick(L"刷新 (F5)", L"Refresh (F5)")},
            {kPickView, layout_.view_button, l10n::Pick(L"视图：详细信息、中图标、大图标", L"View: details, medium or large icons")},
            {kPickSort, layout_.sort, l10n::Pick(L"排序", L"Sort")},
            {kPickHidden, layout_.hidden_button, l10n::Pick(L"显示或隐藏隐藏项目", L"Show or hide hidden items")},
            {kPickNewFolder, layout_.new_folder, l10n::Pick(L"新建文件夹 (Ctrl+Shift+N)", L"New folder (Ctrl+Shift+N)")},
            {kPickFilter, layout_.filter, l10n::Pick(L"文件类型", L"File type")}
        };
        for (const auto& tip : tips) {
            TOOLINFOW info{sizeof(info)};
            info.hwnd = hwnd_; info.uId = static_cast<UINT_PTR>(tip.id);
            SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
            info.uFlags = TTF_SUBCLASS;
            info.rect = {static_cast<LONG>(tip.rect.left), static_cast<LONG>(tip.rect.top),
                         static_cast<LONG>(tip.rect.right), static_cast<LONG>(tip.rect.bottom)};
            info.lpszText = const_cast<LPWSTR>(tip.text);
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        }
    }

    // ---- layout and painting ----------------------------------------------

    void Relayout() {
        RECT client{};
        GetClientRect(hwnd_, &client);
        layout_ = LayoutFolderPicker(static_cast<float>(client.right),
                                     static_cast<float>(client.bottom), visual_.places, painter_,
                                     visual_.primary_text, visual_.cancel_text, scale_, visual_.view, visual_.mode != PickerMode::Folder);
        visual_.scroll = ClampPickerScroll(layout_, visual_.entries.size(), visual_.scroll);
        PlaceEdit();
        UpdateTooltips();
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            art_.SetContext(nullptr, hwnd_, scale_, generation_);
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                     scale_);
        art_.SetContext(compositor_.Dc(), hwnd_, scale_, generation_);
        DrawFolderPicker(compositor_, painter_, theme, visual_, layout_, dark_, high_contrast, &art_);
        EndSurface(compositor_);
        PresentEdit();
    }

    int Hit(LPARAM lparam) const {
        return HitTestFolderPicker(layout_, visual_, static_cast<float>(GET_X_LPARAM(lparam)),
                                   static_cast<float>(GET_Y_LPARAM(lparam)));
    }

    void ScrollBy(float delta) {
        visual_.scroll = ClampPickerScroll(layout_, visual_.entries.size(),
                                           visual_.scroll + delta);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Thumb dragging maps the pointer linearly onto the scroll range.
    void DragScrollbar(float y) {
        const float viewport = layout_.rows.bottom - layout_.rows.top;
        const float content = PickerContentHeight(layout_, visual_.entries.size());
        if (viewport <= 0.0f || content <= viewport) return;
        const float ratio = std::clamp((y - layout_.rows.top) / viewport, 0.0f, 1.0f);
        visual_.scroll = ClampPickerScroll(layout_, visual_.entries.size(),
                                           ratio * content - viewport * 0.5f);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    int PageRows() const {
        return std::max(1, static_cast<int>((layout_.rows.bottom - layout_.rows.top) /
                                            layout_.row_h) - 1) * layout_.columns;
    }

    std::vector<int> FocusOrder() const {
        std::vector<int> order;
        if (visual_.can_back) order.push_back(kPickBack);
        if (visual_.can_forward) order.push_back(kPickForward);
        if (visual_.can_up) order.push_back(kPickUp);
        order.insert(order.end(), {kPickPath, kPickRefresh, kPickSearch, kPickView, kPickSort, kPickHidden, kPickNewFolder, kPickList});
        if (filename_edit_) order.insert(order.end(), {kPickFilename, kPickFilter});
        order.push_back(kPickCancel);
        if (!visual_.chosen.empty()) order.push_back(kPickPrimary);
        return order;
    }

    void MoveFocus(int direction) {
        const auto order = FocusOrder();
        const auto it = std::find(order.begin(), order.end(), visual_.focus);
        int index = it == order.end() ? 0 : static_cast<int>(it - order.begin());
        index = (index + direction + static_cast<int>(order.size())) % static_cast<int>(order.size());
        visual_.focus = order[static_cast<size_t>(index)];
        visual_.show_focus = true;
        HWND target = visual_.focus == kPickPath ? edit_ : visual_.focus == kPickFilename ? filename_edit_ :
            visual_.focus == kPickSearch ? search_edit_ : hwnd_;
        SetFocus(target);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    RECT ScreenRect(D2D1_RECT_F r) const {
        POINT top{static_cast<LONG>(r.left), static_cast<LONG>(r.top)};
        ClientToScreen(hwnd_, &top);
        return {top.x, top.y, top.x + static_cast<LONG>(r.right-r.left), top.y + static_cast<LONG>(r.bottom-r.top)};
    }

    int ChooseMenu(const D2D1_RECT_F& anchor, const std::vector<std::wstring>& labels, int selected) {
        std::vector<FluentMenuItem> items;
        for (size_t i = 0; i < labels.size(); ++i) {
            FluentMenuItem item;
            item.command = static_cast<int>(i) + 1; item.text = labels[i];
            item.radio_group = true; item.radio = static_cast<int>(i) == selected;
            items.push_back(std::move(item));
        }
        menu_.SetTheme(dark_, accent_);
        return menu_.TrackDropdown(ScreenRect(anchor), std::move(items)) - 1;
    }

    void NewFolder() {
        if (visual_.current.empty() || visual_.waiting || visual_.validating) return;
        menu_.SetTheme(dark_, accent_);
        menu_.SetInitialFilterText(l10n::Pick(L"新建文件夹", L"New folder"));
        menu_.SetSelectAllOnOpen(true);
        menu_.SetFilterPlaceholder(l10n::Pick(L"文件夹名称", L"Folder name"));
        auto rows = [](const std::wstring& name) {
            FluentMenuItem item;
            item.command = 1; item.text = l10n::Pick(L"创建文件夹", L"Create folder"); item.enabled = !name.empty();
            return std::vector<FluentMenuItem>{std::move(item)};
        };
        const RECT anchor = ScreenRect(layout_.new_folder);
        const int picked = menu_.TrackPopup({anchor.left, anchor.bottom}, rows(L"New folder"), rows);
        if (done_ || (picked != 1 && !menu_.LastFilterCommitted())) return;
        const auto name = menu_.LastFilterQuery();
        if (name.empty()) return;
        validation_navigation_only_ = false;
        visual_.validating = true;
        visual_.notice = l10n::Pick(L"正在创建文件夹…", L"Creating folder…");
        UpdateChosen();
        loader_->CreateFolder(++generation_, visual_.current, name);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Activate(int id) {
        if (id == kPickClose || id == kPickCancel) Cancel();
        else if (id == kPickPrimary) SubmitSelection();
        else if (id == kPickBack) GoBack();
        else if (id == kPickForward) GoForward();
        else if (id == kPickUp) GoUp();
        else if (id == kPickRefresh) Reload();
        else if (id == kPickPath) FocusPath();
        else if (id == kPickFilename) SetFocus(filename_edit_);
        else if (id == kPickSearch) SetFocus(search_edit_);
        else if (id == kPickNewFolder) NewFolder();
        else if (id == kPickHidden) { options_.show_hidden = !options_.show_hidden; visual_.show_hidden = options_.show_hidden; Reload(); }
        else if (id == kPickView) {
            const int choice = ChooseMenu(layout_.view_button, {l10n::Pick(L"详细信息", L"Details"),
                l10n::Pick(L"中图标", L"Medium icons"), l10n::Pick(L"大图标", L"Large icons")}, static_cast<int>(visual_.view));
            if (choice >= 0 && !done_) { visual_.view = static_cast<PickerViewMode>(choice); g_last_view[mode_index_] = visual_.view; Relayout(); }
        } else if (id == kPickFilter) {
            std::vector<std::wstring> labels;
            for (const auto& f : options_.filters) labels.push_back(f.label);
            const int choice = ChooseMenu(layout_.filter, labels, static_cast<int>(options_.filter_index));
            if (choice >= 0 && !done_) {
                options_.filter_index = static_cast<size_t>(choice);
                visual_.filter_text = options_.filters[options_.filter_index].label; Reload();
            }
        } else if (id == kPickSort) {
            const int choice = ChooseMenu(layout_.sort, {l10n::Pick(L"名称", L"Name"), l10n::Pick(L"修改时间", L"Date modified"),
                l10n::Pick(L"大小", L"Size"), l10n::Pick(L"类型", L"Type"), l10n::Pick(L"升序", L"Ascending"),
                l10n::Pick(L"降序", L"Descending")}, static_cast<int>(options_.sort));
            if (choice >= 0 && !done_) {
                if (choice < 4) options_.sort = static_cast<PickerSort>(choice);
                else options_.descending = choice == 5;
                Reload();
            }
        } else if (id >= kPickPlace && id < kPickRow) {
            const size_t index = static_cast<size_t>(id - kPickPlace);
            if (index < visual_.places.size()) Navigate(visual_.places[index].path, true);
        }
        if (!done_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void EnterOnList() {
        if (visual_.selected_indices.size() > 1) { SubmitSelection(); return; }
        if (SelectedEntry()) OpenEntry(visual_.selected);
        else if (!visual_.chosen.empty()) SubmitSelection();
    }

    bool HandleKey(WPARAM key) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const int count = static_cast<int>(visual_.entries.size());
        auto list_move = [&](int index) {
            visual_.focus = kPickList;
            visual_.show_focus = true;
            if (count > 0) Select(index, shift, ctrl && shift, ctrl && !shift);
        };
        switch (key) {
        case VK_ESCAPE: Cancel(); return true;
        case VK_TAB: MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return true;
        case VK_RETURN:
            if (visual_.focus != kPickList) Activate(visual_.focus);
            else EnterOnList();
            return true;
        case VK_SPACE:
            if (visual_.focus != kPickList) Activate(visual_.focus);
            else if (visual_.selected >= 0) Select(visual_.selected, shift, ctrl);
            return true;
        case VK_LEFT:
            if (visual_.view != PickerViewMode::Details) list_move(visual_.selected - 1);
            return true;
        case VK_RIGHT:
            if (visual_.view != PickerViewMode::Details) list_move(visual_.selected + 1);
            return true;
        case VK_UP: list_move(visual_.selected < 0 ? count - 1 : visual_.selected - layout_.columns);
            return true;
        case VK_DOWN: list_move(visual_.selected < 0 ? 0 : visual_.selected + layout_.columns); return true;
        case VK_HOME: list_move(0); return true;
        case VK_END: list_move(count - 1); return true;
        case VK_PRIOR: list_move(std::max(0, visual_.selected) - PageRows()); return true;
        case VK_NEXT: list_move(std::max(0, visual_.selected) + PageRows()); return true;
        case VK_BACK: GoUp(); return true;
        case VK_F5: Reload(); return true;
        case VK_F4: FocusPath(); return true;
        case 'L':
            if (ctrl) { FocusPath(); return true; }
            break;
        case 'F':
            if (ctrl) { SetFocus(search_edit_); return true; }
            break;
        case 'A':
            if (ctrl && allow_multiselect_) {
                if (visual_.validating) { ++generation_; visual_.validating = false; }
                visual_.selected_indices.clear();
                for (int i = 0; i < count; ++i) {
                    const auto kind = visual_.entries[static_cast<size_t>(i)].kind;
                    if (kind == PickerEntryKind::Image || kind == PickerEntryKind::File)
                        visual_.selected_indices.push_back(i);
                }
                SyncSelectedNames(); InvalidateRect(hwnd_, nullptr, FALSE); return true;
            }
            break;
        case 'N':
            if (ctrl && shift) { NewFolder(); return true; }
            break;
        }
        return false;
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFonts();
            edit_brush_ = CreateSolidBrush(dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255));
            CreateEdit();
            menu_.Create(hwnd_, &compositor_, scale_);
            menu_.SetTheme(dark_, accent_);
            Relayout();
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return BorderlessHitTest(hwnd_, lparam, layout_.title_bar, layout_.close);
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = static_cast<LONG>(kMinWidth * scale_);
            info->ptMinTrackSize.y = static_cast<LONG>(kMinHeight * scale_);
            return 0;
        }
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            Relayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            PlaceEdit();
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            menu_.Create(hwnd_, &compositor_, scale_);
            CreateFonts();
            if (const auto* suggested = reinterpret_cast<RECT*>(lparam)) {
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
            }
            Relayout();
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            const HDC hdc = reinterpret_cast<HDC>(wparam);
            SetTextColor(hdc, EditTextColor(dark_));
            SetBkColor(hdc, EditBackColor(dark_));
            return reinterpret_cast<LRESULT>(EditBrush());
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == EN_CHANGE) InputChanged(reinterpret_cast<HWND>(lparam));
            return 0;
        case WM_TIMER:
            if (wparam == kSearchTimer) {
                KillTimer(hwnd_, kSearchTimer);
                options_.search = visual_.search_text;
                Reload(); return 0;
            }
            if (wparam == kLoadTimer) {
                if (!visual_.waiting) { KillTimer(hwnd_, kLoadTimer); return 0; }
                if (!visual_.loading) {
                    visual_.loading = true;
                    SetTimer(hwnd_, kLoadTimer, kSpinnerFrameMs, nullptr);
                }
                const ULONGLONG elapsed = GetTickCount64() - load_started_;
                visual_.spinner = static_cast<float>(elapsed % 1000ULL) / 1000.0f;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case kListingMessage:
            OnListing(PickerLoader::Take(lparam));
            return 0;
        case kValidationMessage:
            OnValidation(PickerLoader::TakeValidation(lparam));
            return 0;
        case WM_MOUSEMOVE: {
            if (dragging_scrollbar_) {
                DragScrollbar(static_cast<float>(GET_Y_LPARAM(lparam)));
                return 0;
            }
            const int next = Hit(lparam);
            if (next != visual_.hover) {
                visual_.hover = next;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            visual_.hover = kPickNone;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            ScrollBy(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA *
                     layout_.row_h * 3.0f);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const int hit = Hit(lparam);
            visual_.show_focus = false;
            if (hit >= kPickRow) {
                const int index = hit - kPickRow;
                visual_.focus = kPickList;
                SetFocus(hwnd_);
                Select(index, (GetKeyState(VK_SHIFT) & 0x8000) != 0, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                if (message == WM_LBUTTONDBLCLK) OpenEntry(index);
                return 0;
            }
            if (hit == kPickList) {
                FocusList();
                visual_.selected = -1;
                visual_.selected_indices.clear();
                SetFilename(L"");
                UpdateChosen();
                return 0;
            }
            if (hit == kPickScrollbar) {
                dragging_scrollbar_ = true;
                SetCapture(hwnd_);
                DragScrollbar(static_cast<float>(GET_Y_LPARAM(lparam)));
                return 0;
            }
            if (hit == kPickPath) { FocusPath(); return 0; }
            if (hit == kPickFilename) { SetFocus(filename_edit_); return 0; }
            if (hit == kPickSearch) { SetFocus(search_edit_); return 0; }
            visual_.pressed = hit;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (dragging_scrollbar_) {
                dragging_scrollbar_ = false;
                ReleaseCapture();
                return 0;
            }
            const int hit = Hit(lparam);
            const int pressed = visual_.pressed;
            visual_.pressed = kPickNone;
            ReleaseCapture();
            if (pressed != kPickNone && hit == pressed) Activate(hit);
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            dragging_scrollbar_ = false;
            return 0;
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wparam) == XBUTTON1) GoBack();
            else if (GET_XBUTTON_WPARAM(wparam) == XBUTTON2) GoForward();
            return TRUE;
        case WM_KEYDOWN:
            if (HandleKey(wparam)) return 0;
            break;
        case WM_SYSKEYDOWN:
            if (wparam == VK_UP) { GoUp(); return 0; }
            if (wparam == VK_LEFT) { GoBack(); return 0; }
            if (wparam == VK_RIGHT) { GoForward(); return 0; }
            if (wparam == 'D') { FocusPath(); return 0; }
            break;
        case WM_CHAR:
            if (wparam >= 0x20 && wparam != 0x7F && visual_.focus == kPickList &&
                !(GetKeyState(VK_CONTROL) & 0x8000)) {
                const auto now = GetTickCount64();
                const wchar_t character = static_cast<wchar_t>(wparam);
                if (now - typeahead_time_ > 900) typeahead_.clear();
                const bool cycling = typeahead_.size() == 1 && towlower(typeahead_.front()) == towlower(character);
                if (!cycling) typeahead_.push_back(character);
                typeahead_time_ = now;
                const int count = static_cast<int>(visual_.entries.size());
                for (int step = 0; step < count; ++step) {
                    const int start = visual_.selected < 0 ? 0 : visual_.selected + (typeahead_.size() == 1 ? 1 : 0);
                    const int index = (start + step) % count;
                    if (_wcsnicmp(visual_.entries[static_cast<size_t>(index)].name.c_str(), typeahead_.c_str(), typeahead_.size()) == 0) {
                        Select(index); break;
                    }
                }
                return 0;
            }
            break;
        case WM_SETFOCUS:
            if (visual_.focus == kPickPath) visual_.focus = kPickList;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            Cancel();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            menu_.Dismiss();
            if (tooltip_) { DestroyWindow(tooltip_); tooltip_ = nullptr; }
            for (HWND edit : {edit_, filename_edit_, search_edit_}) {
                if (edit) { RemoveWindowSubclass(edit, EditProc, 1); DestroyWindow(edit); }
            }
            edit_ = filename_edit_ = search_edit_ = nullptr;
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND tooltip_ = nullptr;
    HWND owner_ = nullptr;
    HWND edit_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{&compositor_};
    FolderPickerLayout layout_;
    FolderPickerVisual visual_;
    PickerHistory history_;
    std::unique_ptr<PickerLoader> loader_;
    std::wstring fallback_;
    std::wstring select_after_;
    std::wstring edit_text_;
    std::vector<std::wstring> result_;
    PickerOptions options_;
    bool allow_multiselect_ = false;
    bool validation_navigation_only_ = false;
    bool setting_edits_ = false;
    int selection_anchor_ = -1;
    HWND filename_edit_ = nullptr;
    HWND search_edit_ = nullptr;
    FluentMenu menu_;
    FolderPickerArt art_;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    uint64_t generation_ = 0;
    ULONGLONG load_started_ = 0;
    ULONGLONG typeahead_time_ = 0;
    std::wstring typeahead_;
    size_t mode_index_ = 0;
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    bool initial_load_ = false;
    bool current_verified_ = false;
    bool dragging_scrollbar_ = false;
};

} // namespace

bool ShowFilePicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                    D2D1_COLOR_F accent, FilePickerResult& result) {
    FolderPickerWindow window;
    FilePickerResult picked;
    if (!window.Show(owner, spec, dark, accent, picked)) return false;
    result = std::move(picked);
    return true;
}

bool ShowFolderPicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                      D2D1_COLOR_F accent, std::wstring& path) {
    auto single = spec;
    single.allow_multiselect = false;
    FilePickerResult result;
    if (!ShowFilePicker(owner, single, dark, accent, result) || result.paths.empty()) return false;
    path = std::move(result.paths.front());
    return true;
}

} // namespace pulse::ui
