#pragma once

// Layout and drawing of the Pulse folder / picture picker, separate from its
// window so the dialog gallery can render it offscreen.

#include "FluentTokens.h"
#include "fluent_components.h"
#include "folder_picker_model.h"
#include "ui_compositor.h"

#include <string>
#include <vector>

namespace pulse::ui {

class FolderPickerArt;
enum class PickerViewMode { Details, MediumIcons, LargeIcons };

enum PickerControl : int {
    kPickNone = 0,
    kPickClose = 1,
    kPickBack = 2,
    kPickUp = 3,
    kPickPath = 4,
    kPickCancel = 5,
    kPickPrimary = 6,
    kPickScrollbar = 7,
    kPickList = 8,      // empty space inside the list card
    kPickForward = 9, kPickRefresh, kPickView, kPickHidden, kPickFilename,
    kPickFilter, kPickSearch, kPickSort, kPickNewFolder,
    kPickPlace = 100,   // + place index
    kPickRow = 1000,    // + entry index
};

struct PickerPlace {
    std::wstring label;
    std::wstring path;   // "" = This PC
    std::wstring glyph;
    D2D1_COLOR_F color{};    // icon colour; alpha 0 = text colour
    bool separated = false;  // gap above, e.g. This PC after the folders
};

// Everything the picker draws.
struct FolderPickerVisual {
    PickerViewMode view = PickerViewMode::Details;
    std::vector<int> selected_indices;
    bool can_forward = false;
    bool show_hidden = false;
    bool validating = false;
    std::wstring filename_text, filter_text, search_text, notice;
    PickerMode mode = PickerMode::Folder;
    std::wstring title;
    std::wstring current;        // "" = This PC
    std::wstring path_text;      // drawn in the field when no EDIT is hosted
    bool hosted_edit = false;
    bool path_focused = false;
    std::vector<PickerPlace> places;
    std::vector<PickerEntry> entries;
    int selected = -1;
    float scroll = 0.0f;         // pixels
    bool waiting = false;        // a listing is on its way; the list draws nothing
    bool loading = false;        // waiting long enough to show the spinner
    float spinner = 0.0f;        // 0..1 animation phase
    std::wstring error;          // non-empty: the folder could not be read
    bool can_back = false;
    bool can_up = false;
    std::wstring chosen;         // what the primary button would return
    std::wstring primary_text;
    std::wstring cancel_text;
    int hover = kPickNone;
    int pressed = kPickNone;
    int focus = kPickNone;       // keyboard focus
    bool show_focus = false;
};

struct FolderPickerLayout {
    int columns = 1;
    float scale = 1;
    D2D1_RECT_F forward{}, refresh{}, view_button{}, hidden_button{}, filename{}, filter{}, search{}, sort{}, new_folder{};
    float width = 0.0f;
    float height = 0.0f;
    float title_bar = 0.0f;
    float row_h = 0.0f;
    D2D1_RECT_F close{};
    D2D1_RECT_F back{};
    D2D1_RECT_F up{};
    D2D1_RECT_F path{};
    D2D1_RECT_F sidebar{};
    std::vector<D2D1_RECT_F> places;
    D2D1_RECT_F card{};
    D2D1_RECT_F header{};
    D2D1_RECT_F rows{};
    D2D1_RECT_F footer{};
    D2D1_RECT_F summary{};
    D2D1_RECT_F cancel{};
    D2D1_RECT_F primary{};
};

FolderPickerLayout LayoutFolderPicker(float width, float height,
                                      const std::vector<PickerPlace>& places,
                                      const fluent::Painter& painter,
                                      const std::wstring& primary_text,
                                      const std::wstring& cancel_text, float scale,
                                      PickerViewMode view = PickerViewMode::Details, bool file_mode = false);
float PickerContentHeight(const FolderPickerLayout& layout, size_t count);
float ClampPickerScroll(const FolderPickerLayout& layout, size_t count, float scroll);
// Scroll that brings row `index` into view.
float ScrollPickerRowIntoView(const FolderPickerLayout& layout, size_t count, float scroll,
                              int index);
int HitTestFolderPicker(const FolderPickerLayout& layout, const FolderPickerVisual& visual,
                        float x, float y);
void DrawFolderPicker(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                      const FolderPickerVisual& visual, const FolderPickerLayout& layout,
                      bool dark, bool high_contrast, FolderPickerArt* art = nullptr);

} // namespace pulse::ui
