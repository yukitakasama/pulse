#include "../preview_host/preview_properties.h"
#include "../common/preview_extensions.h"
#include <windows.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>

bool RunThumbnailCacheTests();
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
class PropertyStore final : public IPropertyStore {
public:
    bool empty = false;
    std::vector<PROPERTYKEY> requested;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (id != IID_IUnknown && id != IID_IPropertyStore) return E_NOINTERFACE;
        *object = static_cast<IPropertyStore*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) override { if (!count) return E_POINTER; *count = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD, PROPERTYKEY*) override { return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override {
        if (!value) return E_POINTER;
        PropVariantInit(value);
        requested.push_back(key);
        if (empty) return S_OK;
        if (IsEqualPropertyKey(key, PKEY_Title)) return InitPropVariantFromString(L"Fixture title", value);
        if (IsEqualPropertyKey(key, PKEY_Video_FrameWidth) || IsEqualPropertyKey(key, PKEY_Video_FrameHeight)) {
            value->vt = VT_UI4;
            value->ulVal = IsEqualPropertyKey(key, PKEY_Video_FrameWidth) ? 1920u : 1080u;
        }
        return S_OK;
    }
    bool Asked(REFPROPERTYKEY key) const {
        return std::any_of(requested.begin(), requested.end(), [&](const auto& candidate) {
            return IsEqualPropertyKey(candidate, key);
        });
    }
};
}
int main() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Check(SUCCEEDED(initialized), "initialize isolated property test");
    for (const auto ext : pulse::preview::formats::kVideo) {
        PropertyStore store;
        std::wstring path = L"fixture" + std::wstring(ext);
        for (auto& c : path) c = static_cast<wchar_t>(towupper(c));
        const auto result = pulse::preview::ReadPropertiesFromStore(path, &store);
        Check(result.size() == 1 && result[0].value == L"1920 x 1080" &&
              store.Asked(PKEY_Media_Duration) && store.Asked(PKEY_Video_FrameRate) &&
              store.Asked(PKEY_Video_Compression) && !store.Asked(PKEY_Title),
              "AUD-023: every supported video extension requests video properties");
    }
    for (const auto ext : pulse::preview::formats::kAudio) {
        PropertyStore store;
        const auto result = pulse::preview::ReadPropertiesFromStore(L"fixture" + std::wstring(ext), &store);
        Check(result.size() == 1 && result[0].value == L"Fixture title" &&
              store.Asked(PKEY_Music_Artist) && store.Asked(PKEY_Music_AlbumTitle) &&
              store.Asked(PKEY_Media_Duration) && store.Asked(PKEY_Audio_EncodingBitrate) &&
              store.Asked(PKEY_Audio_SampleRate) && !store.Asked(PKEY_Video_FrameWidth),
              "AUD-023: every supported audio extension requests audio properties");
    }
    PropertyStore missing; missing.empty = true;
    Check(pulse::preview::ReadPropertiesFromStore(L"missing.M2TS", &missing).empty() &&
          pulse::preview::ReadPropertiesFromStore(L"missing.OPUS", &missing).empty() &&
          pulse::preview::ReadPropertiesFromStore(L"missing.mp4", nullptr).empty(),
          "AUD-023: absent properties and unavailable stores produce no bogus rows");
    Check(pulse::preview::IsVideoExtension(L".mts") && pulse::preview::IsVideoExtension(L".3gp") &&
          pulse::preview::IsAudioExtension(L".oga") && pulse::preview::IsAudioExtension(L".aiff"),
          "AUD-023: formerly omitted media families remain in shared tables");
    Check(RunThumbnailCacheTests(), "AUD-024: thumbnail fallback and direct-animation cache tests");
    if (SUCCEEDED(initialized)) CoUninitialize();
    return failures ? 1 : 0;
}
