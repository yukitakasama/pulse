#pragma once

// Pulse's own folder / picture picker: a modal window in the main UI's style
// that replaces IFileOpenDialog. Folders are read off the UI thread.

#include "folder_picker_model.h"

#include <d2d1.h>
#include <windows.h>

#include <string>

namespace pulse::ui {

struct FolderPickerSpec {
    PickerMode mode = PickerMode::Folder;
    std::wstring title;         // empty: "Choose a folder" / "Choose background image"
    std::wstring initial_path;  // empty: where the last pick of this mode was made
    std::vector<PickerFilter> filters;
    size_t filter_index = 0;
    std::wstring filename;
    bool allow_multiselect = false;
    bool show_hidden = false;
};

struct FilePickerResult {
    std::vector<std::wstring> paths;
    size_t filter_index = 0;
};

bool ShowFilePicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                    D2D1_COLOR_F accent, FilePickerResult& result);

// Returns true and sets `path` when the user picked something.
bool ShowFolderPicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                      D2D1_COLOR_F accent, std::wstring& path);

} // namespace pulse::ui
