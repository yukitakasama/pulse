#include "../ui/archive_preview.h"
#include "../ui/table_view.h"
#include "../ui/preview_notice.h"
#include "../ui/ui_compositor.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <vector>

template <typename T> using WrlPtr = Microsoft::WRL::ComPtr<T>;
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
bool SavePng(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, const std::filesystem::path& path) {
    WrlPtr<ID2D1Bitmap1> readable;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, bitmap->GetPixelFormat());
    const auto size = bitmap->GetPixelSize();
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) || FAILED(readable->CopyFromBitmap(nullptr, bitmap, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    WrlPtr<IWICImagingFactory> wic;
    WrlPtr<IWICStream> stream;
    WrlPtr<IWICBitmapEncoder> encoder;
    WrlPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(size.width, size.height)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WritePixels(size.height, mapped.pitch, mapped.pitch * size.height, mapped.bits)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    readable->Unmap();
    return ok;
}
bool GuardUnchanged(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, const D2D1_RECT_F& rect) {
    WrlPtr<ID2D1Bitmap1> readable;
    const auto size = bitmap->GetPixelSize();
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, bitmap->GetPixelFormat());
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) || FAILED(readable->CopyFromBitmap(nullptr, bitmap, nullptr))) return false;
    D2D1_MAPPED_RECT map{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &map))) return false;
    bool ok = true;
    for (UINT y=0; y<size.height && ok; ++y) for (UINT x=0; x<size.width; ++x) {
        if (x >= rect.left-1 && x < rect.right+1 && y >= rect.top-1 && y < rect.bottom+1) continue;
        const auto* pixel=map.bits+y*map.pitch+x*4;
        if (pixel[0]!=255 || pixel[1]!=0 || pixel[2]!=255) { ok=false; break; }
    }
    readable->Unmap();
    return ok;
}
std::wstring Workbook(size_t selected = 0, bool failed = false) {
    std::wstring p=L"PULSETBL\t1\nW\t100\t" + std::wstring(failed ? L"0" : L"1") + L"\t"+std::to_wstring(selected)+L"\n";
    for (size_t i=0;i<100;++i) {
        p += L"S\txlsx\tSheet"+std::to_wstring(i+1);
        if (i==selected && failed) p += L"\t0\t0\t1\tread-failed\t0\n";
        else if (i==selected) p += L"\t1\t1\t0\t\t0\nR\tvalue"+std::to_wstring(i+1)+L"\n";
        else p += L"\t0\t0\t0\tnot-loaded\t0\n";
    }
    return p;
}
void Run(pulse::ui::Compositor& compositor, bool dark, float scale, const wchar_t* language) {
    using namespace pulse::ui;
    auto* dc=compositor.Dc();
    auto* factory=compositor.DwriteFactory();
    const auto theme=MakeTheme(dark,HexColor(0x0078d4));
    const auto bg=dark ? HexColor(0x202020) : HexColor(0xffffff);
    const float width=380*scale, height=560*scale;
    const auto rect=D2D1::RectF(12*scale,12*scale,width-12*scale,height-12*scale);
    WrlPtr<ID2D1Bitmap1> bitmap;
    auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
    Check(SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(width),static_cast<UINT32>(height)),nullptr,0,props,&bitmap)),"create offscreen bitmap");
    if (!bitmap) return;
    dc->SetTarget(bitmap.Get()); dc->SetDpi(96,96);
    const std::wstring tag=std::wstring(language)+L"-"+(dark ? L"dark-" : L"light-")+std::to_wstring(static_cast<int>(scale*100));
    auto save=[&](const wchar_t* name) { Check(SavePng(dc,bitmap.Get(),std::filesystem::path(L"build/preview-integrity-ui")/(std::wstring(name)+L"-"+tag+L".png")),"save screenshot"); };
    ArchivePreview archive;
    Check(archive.SetPayload(L"PULSEARC\t1\tDIR\t-\t1\n0\td\t14\t-\t-\tfolder\n1\t-\t14\t-\t-\tnested.txt\n0\t-\t10\t-\t-\tsecond.txt\n",L"Sample folder",24),"parse folder fixture");
    auto drawArchive=[&] { dc->BeginDraw();dc->Clear(bg);archive.Draw(dc,&compositor,rect,theme,scale,true);Check(SUCCEEDED(dc->EndDraw()),"draw real archive control"); };
    drawArchive();
    auto pointFor=[&](const wchar_t* path) {
        for (float y=rect.top;y<rect.bottom;y+=scale) if (archive.SelectAt(rect.left+30*scale,y) && archive.SelectedPath()==path) return y;
        return -1.0f;
    };
    const float second=pointFor(L"second.txt");
    Check(second>=0 && !archive.SelectedIsDirectory(),"right-click selects second file row");
    const float folder=pointFor(L"folder");
    Check(folder>=0 && archive.SelectedIsDirectory(),"right-click selects directory row");
    archive.Key(VK_RIGHT); drawArchive();
    const float nested=pointFor(L"folder\\nested.txt");
    Check(nested>=0 && !archive.SelectedIsDirectory(),"right-click selects nested file row");
    Check(archive.SelectAt(rect.left+30*scale,folder) && archive.SelectedIsDirectory(),"right-click directory does not toggle expansion");
    Check(archive.SelectAt(rect.left+30*scale,nested) && archive.SelectedPath()==L"folder\\nested.txt","nested row remains visible after directory right-click");
    drawArchive();save(L"folder-selected-nested");
    Check(!archive.SelectAt(rect.left+30*scale,rect.bottom-scale) && !archive.HasSelection() && archive.SelectedPath().empty(),"blank space clears prior context target");
    archive.SetPayload(L"PULSEARC\t1\tISO/UDF\t-\t1\n0\t-\t14\t-\t-\tloaded.txt\n",L"Hybrid.iso",50000);
    archive.SetFilter(L"missing");
    Check(archive.FilterHits()==0 && archive.StateNote().find(pulse::l10n::IsChinese() ? L"已加载" : L"loaded")!=std::wstring::npos && archive.StateNote().find(L"UDF")!=std::wstring::npos,"empty search retains loaded-only UDF warning");
    drawArchive();save(L"archive-partial-search");
    TableView table;
    Check(table.SetPayload(Workbook()) && table.PlainText()==L"value1\n","parse on-demand workbook with only selected content");
    auto drawTable=[&] {dc->BeginDraw();dc->Clear(bg);table.Draw(dc,factory,rect,theme,dark,scale,bg,{});Check(SUCCEEDED(dc->EndDraw()),"draw real table control");};
    drawTable();
    bool changed=true;
    for(int i=0;i<16;++i) changed=table.Key(VK_NEXT,false,true)&&changed;
    uint32_t requested=0;
    Check(changed && table.PlainText().empty() && table.SelectedSheetIndex()==16 && table.IsSheetRequestPending(),"Ctrl+PageDown selects unloaded sheet 17 and displays loading state");
    Check(table.TakePendingSheetRequest(requested) && requested==16 && !table.TakePendingSheetRequest(requested),"rapid switches coalesce into exactly one pending sheet request");
    Check(table.SetPayload(Workbook(15)) && table.SelectedSheetIndex()==16 && table.PlainText().empty(),"late response for another sheet cannot replace current selection");
    table.FailPendingSheetRequest(15);
    Check(table.IsSheetRequestPending(),"stale failure cannot cancel current sheet request");
    drawTable();save(L"sheet17-loading");
    Check(table.SetPayload(Workbook(16)) && table.PlainText()==L"value17\n" && !table.IsSheetRequestPending(),"requested sheet 17 response provides its real content");
    drawTable();save(L"sheet17-keyboard");
    Check(table.Key(VK_PRIOR,false,true) && table.PlainText().empty() && table.TakePendingSheetRequest(requested) && requested==15,"previous key requests sheet16 on demand");
    drawTable();
    Check(table.MouseDown(rect.left+43*scale,rect.bottom-17*scale,false) && table.PlainText()==L"value17\n" &&
        !table.IsSheetRequestPending() && !table.TakePendingSheetRequest(requested),"returning to cached sheet17 cancels pending selection without duplicate request");
    table.MouseUp();drawTable();save(L"sheet17-click");
    Check(table.MouseDown(rect.left+75*scale,rect.bottom-17*scale,false) && !table.TakePendingSheetRequest(requested),"clicking selected loaded tab does not queue another request");
    table.MouseUp();
    Check(table.SetPayload(Workbook(15)) && table.SelectedSheetIndex()==16 && table.PlainText()==L"value17\n","cancelled sheet response cannot replace cached current sheet");
    for(size_t i=17;i<100;++i) table.Key(VK_NEXT,false,true);
    Check(table.TakePendingSheetRequest(requested) && requested==99 && table.SetPayload(Workbook(99)) &&
        table.PlainText()==L"value100\n","sheet100 remains selectable and accepts requested content");
    Check(table.Key(VK_PRIOR,false,true) && table.TakePendingSheetRequest(requested) && requested==98,"select another sheet before simulating load failure");
    table.FailPendingSheetRequest(98);
    Check(!table.IsSheetRequestPending() && table.SelectedSheetIndex()==98 && table.PlainText().empty(),"selected-sheet failure retains selection and ends loading");
    drawTable();save(L"sheet-load-failed");
    Check(table.SetPayload(Workbook(98,true)) && table.Key(VK_NEXT,false,true) && table.Key(VK_PRIOR,false,true) &&
        table.TakePendingSheetRequest(requested) && requested==98 && table.SetPayload(Workbook(98,true)) &&
        !table.IsSheetRequestPending(),"identical failed retry response ends loading instead of leaving it pending");
    table.Clear();
    Check(!table.IsSheetRequestPending() && !table.TakePendingSheetRequest(requested) && table.SetPayload(Workbook()) &&
        table.PlainText()==L"value1\n","file change clears pending sheet requests and cached contents");
    table.Clear();
    Check(table.SetPayload(L"PULSETBL\t1\nS\tcsv\tdata.csv\t2\t2\t0\tcomma\t1\nR\ta\tb\nR\t1\t2\nX\ta,b\\n1,2\\n\n") &&
        !table.IsSpreadsheet() && table.PlainText()==L"a\tb\n1\t2\n" && !table.TakePendingSheetRequest(requested),
        "CSV rows and source mode remain independent of spreadsheet loading state");
    WrlPtr<IDWriteTextFormat> format;
    factory->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,13*scale,L"en-US",&format);
    Check(format!=nullptr,"create notice text format");
    if (!format) return;
    for (const auto* error : {L"svg-unsupported:text", L"image-preview-incomplete:frame-limit", L"docx-reading"}) {
        const bool apng = error[0] == L'i';
        const auto text=PreviewNotice(error, apng ? L"PULSEIMG\t1\nT\tanimation\nF\t4096\t4097\n" : L"");
        if (apng) Check(text.find(L"4096 / 4097") != std::wstring::npos,
                        "APNG notice includes actual loaded and declared frame counts");
        const auto notice=D2D1::RectF(24*scale,24*scale,240*scale,240*scale);
        const float wide=DrawPreviewNotice(nullptr,factory,format.Get(),text,D2D1::RectF(0,0,1000*scale,400*scale),scale,theme.text,bg);
        dc->BeginDraw();dc->Clear(HexColor(0xff00ff));
        const float narrow=DrawPreviewNotice(dc,factory,format.Get(),text,notice,scale,theme.text,bg);
        Check(SUCCEEDED(dc->EndDraw()),"render notice");
        Check(narrow>wide && narrow<=notice.bottom-notice.top,"notice wraps within narrow bounds");
        Check(GuardUnchanged(dc,bitmap.Get(),notice),"notice leaves outside guard pixels unchanged");
        save(error[0]==L's' ? L"svg-notice" : apng ? L"apng-notice" : L"docx-notice");
        const auto short_rect=D2D1::RectF(24*scale,24*scale,240*scale,50*scale);
        dc->BeginDraw();dc->Clear(HexColor(0xff00ff));
        const float clipped=DrawPreviewNotice(dc,factory,format.Get(),text,short_rect,scale,theme.text,bg);
        Check(SUCCEEDED(dc->EndDraw()),"render notice in limited height");
        Check(clipped<=short_rect.bottom-short_rect.top && GuardUnchanged(dc,bitmap.Get(),short_rect),"notice respects short available rectangle");
    }
    dc->SetTarget(nullptr);
}
}
int wmain() {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr),L"en-US");
    std::filesystem::create_directories(L"build/preview-integrity-ui");
    HWND window=CreateWindowExW(0,L"STATIC",L"Preview integrity render tests",WS_OVERLAPPEDWINDOW,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(window!=nullptr,"create hidden compositor window");
    {
        pulse::ui::Compositor compositor;
        const bool initialized=window && compositor.Init(window);
        Check(initialized,"initialize production compositor");
        if(initialized) for(const auto* language : {L"en-US", L"zh-CN"}) {
            pulse::l10n::SetLanguage(language);
            Check(!pulse::l10n::Get(pulse::l10n::StringId::ArcFiles).empty() &&
                  !pulse::l10n::Get(pulse::l10n::StringId::ArcFolders).empty(), "localized archive labels are loaded");
            for(bool dark : {false,true}) for(float scale : {1.0f,1.5f}) Run(compositor,dark,scale,language);
        }
    }
    if(window) DestroyWindow(window);
    CoUninitialize();
    return failures ? 1 : 0;
}
