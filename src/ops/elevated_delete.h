#pragma once
#include "elevated_transfer.h"
namespace pulse::ops {
// STA worker only. Recycle uses ITransferSource::RecycleItem, never DeleteItem.
// Permanent deletion uses IFileOperation. Neither requests elevation or Shell UI.
ShellTransferResult DeleteWithCurrentToken(const std::vector<std::wstring>& sources,
    bool permanent, HWND owner, std::atomic<bool>& cancel, ShellTransferProgress progress = {});
}
