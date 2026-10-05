#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/folder_picker_view.h"
#include "../ui/folder_picker_art.h"
#include <cstdio>
#include <cmath>
using namespace pulse::ui;
int RunArt(HWND hwnd, const std::wstring& output) {
    Compositor compositor;
    if (!compositor.Init(hwnd)) return 2;
    compositor.Resize(128, 128);
    auto* dc = compositor.Dc();
    dc->BeginDraw(); dc->Clear(D2D1::ColorF(0.1f, 0.7f, 0.3f));
    if (FAILED(dc->EndDraw())) return 3;
    const std::wstring fixture = output + L"\\art-fixture.png";
    if (!compositor.SaveSnapshot(fixture.c_str())) return 4;
    wchar_t full[MAX_PATH]{}; GetFullPathNameW(fixture.c_str(), MAX_PATH, full, nullptr);
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(full, GetFileExInfoStandard, &attrs)) return 5;
    PickerEntry entry; entry.path = full; entry.name = L"art-fixture.png";
    entry.kind = PickerEntryKind::Image; entry.modified = attrs.ftLastWriteTime;
    entry.size = (static_cast<uint64_t>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
    FolderPickerArt art;
    int failures = 0;
    auto await_art = [&](uint64_t generation, bool thumbnail) {
        art.SetContext(dc, hwnd, 1, generation);
        const auto start = GetTickCount64();
        PickerArtResult result{};
        do {
            dc->BeginDraw(); dc->Clear(D2D1::ColorF(1, 1, 1));
            art.Draw(dc, entry, D2D1::RectF(0, 0, 128, 128), thumbnail, &result);
            if (FAILED(dc->EndDraw())) return false;
            if (result == (thumbnail ? PickerArtResult::Thumbnail : PickerArtResult::ShellIcon)) return true;
            MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            Sleep(20);
        } while (GetTickCount64() - start < 15000);
        return false;
    };
    const bool thumbnail = await_art(1, true);
    printf("[%s] asynchronous real PNG thumbnail\n", thumbnail ? "PASS" : "FAIL");
    if (!thumbnail) ++failures;
    if (thumbnail) compositor.SaveSnapshot((output + L"\\art-thumbnail.png").c_str());
    const bool icon = await_art(1, false);
    printf("[%s] asynchronous native Shell icon\n", icon ? "PASS" : "FAIL");
    if (!icon) ++failures;
    // A new generation invalidates pending/results; a device reset clears GPU objects.
    art.SetContext(nullptr, hwnd, 1, 2);
    const bool renewed = await_art(2, true);
    printf("[%s] generation replacement and GPU cache reset\n", renewed ? "PASS" : "FAIL");
    if (!renewed) ++failures;
    const auto close_start = GetTickCount64(); art.Close();
    const bool closed = GetTickCount64() - close_start < 500;
    printf("[%s] close returns without joining background workers\n", closed ? "PASS" : "FAIL");
    if (!closed) ++failures;
    const bool reopened = await_art(3, true);
    printf("[%s] bounded service reused by next context\n", reopened ? "PASS" : "FAIL");
    if (!reopened) ++failures;
    art.Close();
    return failures ? 1 : 0;
}
int RunShowcase(HWND hwnd, const std::wstring& output) {
    Compositor compositor;
    if (!compositor.Init(hwnd)) return 2;
    compositor.RecreateTextFormats(1.0f);
    const std::wstring fixture_dir = output + L"\\showcase-fixtures-" + std::to_wstring(GetCurrentProcessId());
    if (!CreateDirectoryW(fixture_dir.c_str(), nullptr)) return 3;
    wchar_t absolute[MAX_PATH]{};
    GetFullPathNameW(fixture_dir.c_str(), MAX_PATH, absolute, nullptr);
    const std::wstring base = absolute;
    const std::wstring image_path = base + L"\\海岸与群山.png";
    const std::wstring folder_path = base + L"\\项目素材";
    const std::wstring text_path = base + L"\\创作说明.txt";
    const std::wstring pdf_path = base + L"\\品牌设计指南.pdf";
    CreateDirectoryW(folder_path.c_str(), nullptr);
    for (const auto& file : {text_path, pdf_path}) {
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
    compositor.Resize(320, 200);
    fluent::Painter painter(&compositor); painter.SetScale(1);
    auto* dc = compositor.Dc();
    const auto light_theme = MakeTheme(false, HexColor(0x0078D4));
    dc->BeginDraw(); dc->Clear(HexColor(0xBDE2EB)); painter.BeginFrame(light_theme, false);
    painter.FillRoundedRect(D2D1::RectF(220, 22, 264, 66), 22, HexColor(0xFFF2B0));
    painter.FillRoundedRect(D2D1::RectF(-45, 73, 184, 288), 100, HexColor(0x579B83));
    painter.FillRoundedRect(D2D1::RectF(125, 97, 355, 275), 90, HexColor(0x2C7268));
    painter.FillRoundedRect(D2D1::RectF(0, 159, 320, 205), 0, HexColor(0x77C6DC));
    const bool fixture_saved = SUCCEEDED(dc->EndDraw()) && compositor.SaveSnapshot(image_path.c_str());
    FolderPickerVisual v;
    v.mode = PickerMode::File; v.view = PickerViewMode::LargeIcons;
    v.title = L"打开文件"; v.current = base; v.path_text = L"此电脑  >  图片  >  设计素材";
    v.primary_text = L"打开"; v.cancel_text = L"取消";
    v.filename_text = L"海岸与群山.png"; v.filter_text = L"所有文件 (*.*)";
    v.can_back = v.can_up = true; v.chosen = image_path;
    v.selected = 1; v.selected_indices = {1}; v.focus = kPickList; v.show_focus = true;
    v.places = {{L"桌面", L"C:\\Desktop", L"\xE7F4"}, {L"文档", L"C:\\Documents", L"\xE8A5"},
        {L"下载", L"C:\\Downloads", L"\xE896"}, {L"图片", base, L"\xEB9F"}, {L"此电脑", L"", L"\xE7F4"}};
    const std::wstring files[] = {folder_path, image_path, text_path, pdf_path};
    for (int i = 0; i < 4; ++i) {
        PickerEntry entry; entry.path = files[i]; entry.name = files[i].substr(files[i].find_last_of(L'\\') + 1);
        entry.kind = i == 0 ? PickerEntryKind::Folder : i == 1 ? PickerEntryKind::Image : PickerEntryKind::File;
        WIN32_FILE_ATTRIBUTE_DATA attrs{};
        if (GetFileAttributesExW(entry.path.c_str(), GetFileExInfoStandard, &attrs)) {
            entry.modified = attrs.ftLastWriteTime;
            entry.size = (static_cast<uint64_t>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
        }
        v.entries.push_back(entry);
    }
    FolderPickerArt art; art.SetContext(dc, hwnd, 1, 1);
    bool ready[4]{};
    const auto start = GetTickCount64();
    while (fixture_saved && GetTickCount64() - start < 15000) {
        dc->BeginDraw();
        for (int i = 0; i < 4; ++i) {
            PickerArtResult result{};
            art.Draw(dc, v.entries[static_cast<size_t>(i)], D2D1::RectF(0, 0, 96, 96), true, &result);
            ready[i] = result == (i == 1 ? PickerArtResult::Thumbnail : PickerArtResult::ShellIcon);
        }
        if (FAILED(dc->EndDraw())) break;
        if (ready[0] && ready[1] && ready[2] && ready[3]) break;
        MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(20);
    }
    bool saved = fixture_saved && ready[0] && ready[1] && ready[2] && ready[3];
    compositor.Resize(860, 600);
    for (const bool dark : {false, true}) {
        const auto theme = MakeTheme(dark, HexColor(0x0078D4));
        const auto layout = LayoutFolderPicker(860, 600, v.places, painter, v.primary_text, v.cancel_text, 1, v.view, true);
        dc->BeginDraw(); dc->Clear(theme.bg); painter.BeginFrame(theme, false);
        DrawFolderPicker(compositor, painter, theme, v, layout, dark, false, &art);
        const bool drawn = SUCCEEDED(dc->EndDraw());
        const std::wstring snapshot = output + (dark ? L"\\showcase-dark.png" : L"\\showcase-light.png");
        saved = drawn && compositor.SaveSnapshot(snapshot.c_str()) && saved;
    }
    art.Close();
    const bool removed_image = DeleteFileW(image_path.c_str()) != FALSE;
    const bool removed_text = DeleteFileW(text_path.c_str()) != FALSE;
    const bool removed_pdf = DeleteFileW(pdf_path.c_str()) != FALSE;
    const bool removed_folder = RemoveDirectoryW(folder_path.c_str()) != FALSE;
    const bool removed_base = RemoveDirectoryW(base.c_str()) != FALSE;
    saved = saved && removed_image && removed_text && removed_pdf && removed_folder && removed_base;
    printf("[%s] real thumbnail + native icons showcase (light/dark), fixtures removed\n", saved ? "PASS" : "FAIL");
    return saved ? 0 : 1;
}
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring_view(argv[1]) == L"--extensions") {
        bool ok = true;
        for (const auto name : {L"photo.PNG", L"animation.GIF", L"scan.tiff", L"vector.svg", L"draw.emf", L"draw.wmf", L"design.psd", L"design.psb", L"photo.jfif"})
            ok = CanPickerThumbnailName(name) && ok;
        for (const auto name : {L"report.pdf", L"movie.mp4", L"notes.txt", L"archive.zip", L"no_extension"})
            ok = !CanPickerThumbnailName(name) && ok;
        printf("[%s] thumbnail extensions share preview families and exclude heavy document/video formats\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const bool showcase_mode = argc > 1 && std::wstring_view(argv[1]) == L"--showcase";
    const bool art_mode = argc > 1 && std::wstring_view(argv[1]) == L"--art";
    const std::wstring output = argc > (art_mode || showcase_mode ? 2 : 1) ? argv[art_mode || showcase_mode ? 2 : 1] : L"build\\file-picker-visual";
    CreateDirectoryW(output.c_str(), nullptr);
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW; wc.lpszClassName = L"PulsePickerVisualTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 900, 600, nullptr, nullptr, wc.hInstance, nullptr);
    if (showcase_mode) { const int result = RunShowcase(hwnd, output); DestroyWindow(hwnd); CoUninitialize(); return result; }
    if (art_mode) { const int result = RunArt(hwnd, output); DestroyWindow(hwnd); CoUninitialize(); return result; }
    int failures = 0, written = 0;
    {
        Compositor compositor;
        if (!compositor.Init(hwnd)) return 2;
        for (float scale : {1.0f, 1.5f, 2.0f}) for (int theme_index = 0; theme_index < 3; ++theme_index)
        for (int view = 0; view < 3; ++view) for (int scene = 0; scene < 5; ++scene) {
            const bool hc = theme_index == 2, dark = theme_index == 1;
            const auto theme = hc ? MakeHighContrastTheme() : MakeTheme(dark, HexColor(0x0078D4));
            compositor.RecreateTextFormats(scale);
            fluent::Painter painter(&compositor); painter.SetScale(scale);
            FolderPickerVisual v; v.mode = PickerMode::File; v.view = static_cast<PickerViewMode>(view);
            v.title = L"打开文件"; v.current = L"C:\\工作\\设计"; v.path_text = v.current;
            v.primary_text = L"打开"; v.cancel_text = L"取消"; v.can_back = v.can_up = true;
            v.filter_text = L"所有文件 (*.*)"; v.filename_text = L"产品设计稿.png";
            v.places = {{L"桌面", L"C:\\Desktop", L"\xE7F4"}, {L"文档", L"C:\\Documents", L"\xE8A5"}, {L"下载", L"C:\\Downloads", L"\xE896"}};
            for (int i = 0; i < 35; ++i) { PickerEntry e; e.name = i == 2 ? L"这是一个非常长的文件名用于验证不同缩放级别不会溢出或覆盖其他控件.png" : L"项目文件 " + std::to_wstring(i) + (i % 2 ? L".pdf" : L".png"); e.kind = i == 0 ? PickerEntryKind::Folder : PickerEntryKind::File; e.size = 12345; v.entries.push_back(e); }
            v.selected = 2; v.selected_indices = {1, 2, 4}; v.show_focus = true; v.focus = kPickList;
            if (scene == 1) v.entries.clear();
            if (scene == 2) v.error = L"无法访问此文件夹。请检查权限后重试。";
            if (scene == 3) { v.waiting = v.loading = true; v.spinner = .3f; }
            if (scene == 4) v.notice = L"所选文件不存在，请重新选择。";
            const float width = (scene == 4 ? 620.0f : 860.0f) * scale, height = 560 * scale;
            const auto layout = LayoutFolderPicker(width, height, v.places, painter, v.primary_text, v.cancel_text, scale, v.view, true);
            if (layout.filename.right >= layout.filter.left || layout.rows.bottom <= layout.rows.top) ++failures;
            if (scene == 0) {
                const float cell = (layout.rows.right - layout.rows.left - 8 * scale) / layout.columns;
                for (int col = 0; col < layout.columns; ++col)
                    if (HitTestFolderPicker(layout, v, layout.rows.left + (col + .5f) * cell, layout.rows.top + 5 * scale) != kPickRow + col) ++failures;
                const float scroll = ScrollPickerRowIntoView(layout, v.entries.size(), 0, 34);
                const float bottom = (34 / layout.columns + 1) * layout.row_h;
                if (bottom > scroll + layout.rows.bottom - layout.rows.top + 1) ++failures;
            }
            compositor.Resize(static_cast<int>(width), static_cast<int>(height));
            auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
            painter.BeginFrame(theme, hc); DrawFolderPicker(compositor, painter, theme, v, layout, dark, hc);
            if (FAILED(dc->EndDraw())) { ++failures; continue; }
            const auto file = output + L"\\picker_" + std::to_wstring(static_cast<int>(scale * 100)) + L"_t" + std::to_wstring(theme_index) + L"_v" + std::to_wstring(view) + L"_s" + std::to_wstring(scene) + L".png";
            if (!compositor.SaveSnapshot(file.c_str())) ++failures; else ++written;
        }
    }
    DestroyWindow(hwnd); CoUninitialize();
    printf("[%s] picker geometry and rendering: %d PNGs, %d failures\n", failures ? "FAIL" : "PASS", written, failures);
    return failures ? 1 : 0;
}
