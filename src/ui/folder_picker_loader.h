#pragma once

#include "folder_picker_model.h"
#include <memory>

namespace pulse::ui {

struct PickerValidation {
    uint64_t generation = 0;
    std::vector<std::wstring> paths;
    std::wstring navigate;
    DWORD error = ERROR_SUCCESS;
    std::wstring failed_path;
};

// Two bounded workers share a latest-only queue; closing never waits on a network request.
class PickerLoader {
public:
    PickerLoader(HWND hwnd, UINT message, UINT validation_message = 0);
    ~PickerLoader();
    PickerLoader(const PickerLoader&) = delete;
    PickerLoader& operator=(const PickerLoader&) = delete;
    void Load(uint64_t generation, std::wstring path, PickerMode mode, PickerOptions options = {});
    void Validate(uint64_t generation, std::wstring current, std::vector<std::wstring> names,
                  PickerMode mode, PickerOptions options = {});
    void CreateFolder(uint64_t generation, std::wstring current, std::wstring name);
    static std::unique_ptr<PickerListing> Take(LPARAM lparam);
    static std::unique_ptr<PickerValidation> TakeValidation(LPARAM lparam);
private:
    struct Target;
    std::shared_ptr<Target> target_;
};

PickerListing ReadPickerListing(const std::wstring& path, PickerMode mode, const PickerOptions& options = {});

} // namespace pulse::ui
