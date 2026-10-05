#include "../common/windows_compat.h"
#include "folder_picker_art.h"
#include "../common/preview_extensions.h"
#include <cwctype>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
bool CanPickerThumbnailName(std::wstring_view name) {
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return false;
    std::wstring extension(name.substr(dot));
    for (auto& ch : extension) ch = static_cast<wchar_t>(std::towlower(ch));
    return extension == L".jfif" || preview::IsImageExtension(extension) ||
        preview::IsVectorExtension(extension) || preview::IsMetaFileExtension(extension) ||
        preview::IsPsdExtension(extension);
}
struct FolderPickerArt::Service {
    ShellIconCache icons;
    ThumbnailCache thumbnails{16ull * 1024ull * 1024ull, 96};
    FolderPickerArt* owner = nullptr;
};
FolderPickerArt::Service& FolderPickerArt::SharedService() {
    // One bounded service per UI thread. Closing a dialog never joins Shell work;
    // the application's UI-thread shutdown owns the worker lifetime.
    static thread_local Service service;
    return service;
}
void FolderPickerArt::SetContext(ID2D1DeviceContext* dc, HWND hwnd, float scale,
                                  uint64_t generation) {
    service_ = &SharedService();
    if (service_->owner != this) {
        service_->owner = this;
        dc_ = nullptr;
        hwnd_ = nullptr;
        service_->thumbnails.Evict();
    }
    if (generation_ != generation) service_->thumbnails.Evict();
    generation_ = generation;
    if (dc_ != dc) {
        service_->icons.SetDeviceContext(dc);
        service_->thumbnails.SetDeviceContext(dc);
        dc_ = dc;
    }
    if (hwnd_ != hwnd) {
        service_->icons.SetNotifyWindow(hwnd);
        service_->thumbnails.SetNotifyWindow(hwnd);
        hwnd_ = hwnd;
    }
    scale_ = scale;
    service_->icons.SetScale(scale);
}
bool FolderPickerArt::Draw(ID2D1DeviceContext* dc, const PickerEntry& entry,
                            const D2D1_RECT_F& rect, bool thumbnails, PickerArtResult* result) {
    if (result) *result = PickerArtResult::Pending;
    if (!dc || !service_ || service_->owner != this || entry.path.empty()) return false;
    const bool folder = entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive;
    const DWORD attrs = folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    const float edge = std::min(rect.right - rect.left, rect.bottom - rect.top);
    if (thumbnails && !folder && CanPickerThumbnailName(entry.name)) {
        const auto pixels = static_cast<uint32_t>(std::clamp(std::ceil(edge), 32.0f, 512.0f));
        const uint64_t modified = (static_cast<uint64_t>(entry.modified.dwHighDateTime) << 32) |
                                   entry.modified.dwLowDateTime;
        if (service_->thumbnails.DrawGridThumbnail(dc, rect, entry.path, attrs, pixels, generation_,
                modified, entry.size, 1.0f, false, nullptr) == PreviewDrawResult::Bitmap) {
            if (result) *result = PickerArtResult::Thumbnail;
            return true;
        }
    }
    service_->icons.Prefetch(entry.path, entry.name, folder, attrs, edge / scale_);
    if (auto* bitmap = service_->icons.CachedBitmapFor(entry.path, entry.name, folder, attrs, edge / scale_)) {
        dc->DrawBitmap(bitmap, rect, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        if (result) *result = PickerArtResult::ShellIcon;
        return true;
    }
    return false;
}
void FolderPickerArt::Close() {
    if (service_ && service_->owner == this) {
        service_->icons.SetNotifyWindow(nullptr);
        service_->thumbnails.SetNotifyWindow(nullptr);
        service_->thumbnails.Evict();
        service_->icons.SetDeviceContext(nullptr);
        service_->thumbnails.SetDeviceContext(nullptr);
        service_->owner = nullptr;
    }
    service_ = nullptr;
    dc_ = nullptr;
    hwnd_ = nullptr;
}
}
