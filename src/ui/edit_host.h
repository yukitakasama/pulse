#pragma once
#include <windows.h>
#include "ui_compositor.h"

namespace pulse::ui {
// Applies the selected backend to existing child surfaces; returns custom mode.
bool SynchronizeChildEditBackend(Compositor& compositor, HWND hwnd);
bool HandleChildEditMessage(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HBRUSH background_brush,
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT& result);
// Presents a child editor's LumaText bitmap (e.g. right after showing it).
// When the layered present fails, the EDIT switches to native painting so it
// stays visible and clickable. Returns true when the LumaText bitmap is shown.
bool PresentChildEdit(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd);
LRESULT DefPresentedChildEditProc(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background,
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
// Height of a hosted EDIT: one line of its font, never taller than the field
// it sits in (callers centre it vertically in the field).
inline int EditLineHeight(HWND edit, HFONT font, int field_height) {
    int line = field_height;
    if (edit && font) {
        HDC hdc = GetDC(edit);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(edit, hdc);
        line = tm.tmHeight > 1 ? static_cast<int>(tm.tmHeight) : 1;
    }
    return line < field_height ? line : field_height;
}
// EDIT owns native editing and IME. Its LumaText bitmap is a child surface,
// clipped and moved by the parent rather than an independently owned popup.
inline HWND CreateChildEdit(HWND parent, const wchar_t* text = L"", DWORD edit_style = 0) {
    return CreateWindowExW(WS_EX_LAYERED, L"EDIT", text,
        WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL | edit_style,
        0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}
}
