#pragma once
#include "folder_picker_model.h"
#include "shell_icons.h"
#include "thumbnail_cache.h"

namespace pulse::ui {
// Owns only bounded caches; all extraction uses Pulse's existing background backends.
bool CanPickerThumbnailName(std::wstring_view name);
enum class PickerArtResult { Pending, ShellIcon, Thumbnail };
class FolderPickerArt {
public:
    ~FolderPickerArt() { Close(); }
    void SetContext(ID2D1DeviceContext* dc, HWND hwnd, float scale, uint64_t generation);
    bool Draw(ID2D1DeviceContext* dc, const PickerEntry& entry, const D2D1_RECT_F& rect,
              bool thumbnails, PickerArtResult* result = nullptr);
    void Close();
private:
    struct Service;
    static Service& SharedService();
    Service* service_ = nullptr;
    ID2D1DeviceContext* dc_ = nullptr;
    HWND hwnd_ = nullptr;
    float scale_ = 1;
    uint64_t generation_ = 0;
};
}
