#include "mod.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace mmmfix {
namespace {

enum class AspectMode : LONG {
    Off = 0,
    Pillarbox = 1,
};

struct SceneState {
    std::atomic<bool> backgroundCleared{false};
    std::atomic<bool> lastBackgroundAdjusted{false};
    std::atomic<bool> lastUiChromeAdjusted{false};
    std::atomic<bool> lastTextAdjusted{false};
    std::atomic<bool> nvlUiSeen{false};
    std::atomic<bool> messageUiSeen{false};
    std::atomic<bool> systemUiSeen{false};
    std::atomic<ID3D11VertexShader*> frameBackgroundVertexShader{nullptr};
    std::atomic<UINT> textureWidths[8]{};
    std::atomic<UINT> textureHeights[8]{};
};
thread_local SceneState* g_currentScene = nullptr;
template <typename T> struct SceneValue {
    std::atomic<T> fallback{};
    std::atomic<T> SceneState::* member;
    SceneValue(T initial, std::atomic<T> SceneState::* field) : fallback(initial), member(field) {}
    std::atomic<T>& value() { return g_currentScene ? g_currentScene->*member : fallback; }
    T load(std::memory_order order = std::memory_order_seq_cst) { return value().load(order); }
    void store(T v, std::memory_order order = std::memory_order_seq_cst) {
        value().store(v, order);
        if (g_currentScene) fallback.store(v, order); // overlay diagnostics only
    }
    T exchange(T v) { return value().exchange(v); }
    bool compare_exchange_strong(T& expected, T desired) { return value().compare_exchange_strong(expected, desired); }
};

HMODULE g_self = nullptr;
std::wstring g_iniPath;
std::wstring g_logPath;
std::atomic<bool> g_running{false};
std::atomic<bool> g_overlayVisible{false};
std::atomic<LONG> g_aspectMode{static_cast<LONG>(AspectMode::Off)};
std::atomic<UINT> g_hotkey{VK_F12};
std::atomic<bool> g_logging{true};
std::atomic<bool> g_spritesOverPillarbox{true};
std::atomic<LONG> g_overlaySelection{0};
std::atomic<bool> g_restoreCursor{true};
std::atomic<LONG> g_textboxOpacity{100};
std::atomic<LONG> g_textboxColorR{255};
std::atomic<LONG> g_textboxColorG{255};
std::atomic<LONG> g_textboxColorB{255};
std::atomic<bool> g_matchVoicedTextSpeed{false};
std::atomic<bool> g_continueVoiceUntilNext{false};
// The engine fades dialogue channel 3 to zero before it reaches the channel
// stop wrapper. The XAudio trace associates the next mono source created by a
// channel-3 voice-start command with this pointer so that fade can be held.
std::atomic<unsigned> g_dialogueVoiceCreatePending{0};
std::atomic<IXAudio2SourceVoice*> g_dialogueSourceVoice{nullptr};
std::atomic<uint32_t> g_dialogueHeldVolumeBits{0};
std::atomic<bool> g_highResolution{false};
UINT g_configuredRenderWidth = 0;
UINT g_configuredRenderHeight = 0;
std::atomic<UINT> g_nativeRenderWidth{0};
std::atomic<UINT> g_nativeRenderHeight{0};
// Persists the story's 4:3 presentation across backgroundless and native
// 16:9 black frames. Known system UI and swap-chain changes clear it.
std::atomic<bool> g_storyPillarboxActive{false};
SceneValue<bool> g_backgroundCleared{false, &SceneState::backgroundCleared};
thread_local bool g_internalDraw = false;
bool g_nativeWindows = true;
bool g_wrapEnabled = false;
UINT g_wrapWidth = 1320;
UINT g_wrapCaller = 0;
// The engine's active body layout identifies panel-free NVL passages. Updated
// before glyph layout, independent of the optional ADV width override.
std::atomic<bool> g_engineNvlLayout{false};

std::atomic<UINT> g_rtWidth{0};
std::atomic<UINT> g_rtHeight{0};
std::atomic<UINT> g_textureWidth{0};
std::atomic<UINT> g_textureHeight{0};
std::atomic<float> g_viewportX{0.0f};
std::atomic<float> g_viewportY{0.0f};
std::atomic<float> g_viewportWidth{0.0f};
std::atomic<float> g_viewportHeight{0.0f};
SceneValue<bool> g_lastBackgroundAdjusted{false, &SceneState::lastBackgroundAdjusted};
SceneValue<bool> g_lastUiChromeAdjusted{false, &SceneState::lastUiChromeAdjusted};
SceneValue<bool> g_lastTextAdjusted{false, &SceneState::lastTextAdjusted};
SceneValue<bool> g_nvlUiSeen{false, &SceneState::nvlUiSeen};
SceneValue<bool> g_messageUiSeen{false, &SceneState::messageUiSeen};
SceneValue<bool> g_systemUiSeen{false, &SceneState::systemUiSeen};
SceneValue<ID3D11VertexShader*> g_frameBackgroundVertexShader{nullptr, &SceneState::frameBackgroundVertexShader};

WNDPROC g_originalWndProc = nullptr;
HWND g_window = nullptr;
std::atomic<HCURSOR> g_gameCursor{nullptr};
std::atomic<bool> g_cursorRestorePending{false};
std::atomic<bool> g_forceCursorVisible{false};

using ShowCursorFn = int(WINAPI*)(BOOL);
using SetCursorFn = HCURSOR(WINAPI*)(HCURSOR);
ShowCursorFn g_originalShowCursor = nullptr;
SetCursorFn g_originalSetCursor = nullptr;

using CreateDXGIFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
using D3D11CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                             const D3D_FEATURE_LEVEL*, UINT, UINT,
                                             ID3D11Device**, D3D_FEATURE_LEVEL*,
                                             ID3D11DeviceContext**);
using FactoryCreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PSSetShaderResourcesFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                          D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                       const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using RSSetScissorRectsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);

CreateDXGIFactory1Fn g_originalCreateDXGIFactory1 = nullptr;
D3D11CreateDeviceFn g_originalD3D11CreateDevice = nullptr;
FactoryCreateSwapChainFn g_originalCreateSwapChain = nullptr;
PresentFn g_originalPresent = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;
CreateDeferredContextFn g_originalCreateDeferredContext = nullptr;
CreateTexture2DFn g_originalCreateTexture2D = nullptr;
std::atomic<unsigned> g_candidateLogCount{0};
std::atomic<unsigned> g_uiLogCount{0};
std::atomic<unsigned> g_geometryLogMask{0};
std::atomic<ID3D11VertexShader*> g_backgroundVertexShader{nullptr};
std::atomic<ID3D11VertexShader*> g_nvlFontVertexShader{nullptr};
std::atomic<ID3D11InputLayout*> g_nvlFontInputLayout{nullptr};
std::atomic<ID3D11GeometryShader*> g_nativeNvlGeometryShader{nullptr};
std::atomic<ID3D11GeometryShader*> g_advNameGeometryShader{nullptr};
std::atomic<ID3D11GeometryShader*> g_maybeFontGeometryShader{nullptr};
std::atomic<ID3D11PixelShader*> g_textboxPixelShader{nullptr};
std::atomic<ID3D11Buffer*> g_textboxStyleBuffer{nullptr};

struct ContextHooks {
    std::atomic<void**> vtable{nullptr};
    std::atomic<unsigned long long> draws{0};
    std::atomic<unsigned long long> indexedDraws{0};
    std::atomic<unsigned long long> textureSets{0};
    std::atomic<unsigned> samples{0};
    PSSetShaderResourcesFn psSetShaderResources = nullptr;
    OMSetRenderTargetsFn omSetRenderTargets = nullptr;
    RSSetViewportsFn rsSetViewports = nullptr;
    RSSetScissorRectsFn rsSetScissorRects = nullptr;
    DrawFn draw = nullptr;
    DrawIndexedFn drawIndexed = nullptr;
    MapFn map = nullptr;
    UnmapFn unmap = nullptr;
    HRESULT (STDMETHODCALLTYPE* finishCommandList)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**) = nullptr;
    SceneState scene;
    void (STDMETHODCALLTYPE* iaSetVertexBuffers)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*) = nullptr;
    void (STDMETHODCALLTYPE* iaSetInputLayout)(ID3D11DeviceContext*, ID3D11InputLayout*) = nullptr;
    void (STDMETHODCALLTYPE* iaSetPrimitiveTopology)(ID3D11DeviceContext*, D3D11_PRIMITIVE_TOPOLOGY) = nullptr;
    void (STDMETHODCALLTYPE* vsSetShader)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT) = nullptr;
    void (STDMETHODCALLTYPE* gsSetShader)(ID3D11DeviceContext*, ID3D11GeometryShader*, ID3D11ClassInstance* const*, UINT) = nullptr;
};

ContextHooks g_contextHooks[8];
void HookContext(ID3D11DeviceContext* context);
std::atomic<UINT> g_psTextureWidths[8]{};
std::atomic<UINT> g_psTextureHeights[8]{};
std::atomic<UINT>& TextureWidth(UINT slot) {
    return g_currentScene ? g_currentScene->textureWidths[slot] : g_psTextureWidths[slot];
}
std::atomic<UINT>& TextureHeight(UINT slot) {
    return g_currentScene ? g_currentScene->textureHeights[slot] : g_psTextureHeights[slot];
}

struct BufferSnapshot {
    ID3D11Resource* resource = nullptr;
    UINT byteWidth = 0;
    UINT copiedBytes = 0;
    // The engine streams every sprite through one 64 MiB dynamic buffer. UI
    // quads seen so far begin a few KiB into it, so retaining the first 64 KiB
    // is enough to inspect them without copying the whole allocation per frame.
    unsigned char bytes[65536]{};
};

SRWLOCK g_snapshotLock = SRWLOCK_INIT;
BufferSnapshot g_bufferSnapshots[32];

struct ActiveMap {
    ID3D11Resource* resource = nullptr;
    void* data = nullptr;
    UINT byteWidth = 0;
};
thread_local ActiveMap g_activeMap;

template <typename T>
void SafeRelease(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

std::wstring ModuleSiblingPath(const wchar_t* fileName) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) {
        slash[1] = L'\0';
    } else {
        path[0] = L'\0';
    }
    return std::wstring(path) + fileName;
}

void Log(const char* format, ...) {
    if (!g_logging.load()) {
        return;
    }
    char message[1024]{};
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[1200]{};
    const int length = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u %s\r\n",
                                now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::max(length, 0)), &written, nullptr);
    CloseHandle(file);
}

UINT ParseHotkey(const wchar_t* text) {
    if ((_wcsnicmp(text, L"F", 1) == 0) && wcslen(text) <= 3) {
        const int number = _wtoi(text + 1);
        if (number >= 1 && number <= 12) {
            return static_cast<UINT>(VK_F1 + number - 1);
        }
    }
    if (_wcsicmp(text, L"INSERT") == 0) return VK_INSERT;
    if (_wcsicmp(text, L"HOME") == 0) return VK_HOME;
    if (_wcsicmp(text, L"END") == 0) return VK_END;
    if (_wcsicmp(text, L"DELETE") == 0) return VK_DELETE;
    return VK_F12;
}

std::wstring HotkeyName(UINT key) {
    if (key >= VK_F1 && key <= VK_F12) {
        return L"F" + std::to_wstring(key - VK_F1 + 1);
    }
    if (key == VK_INSERT) return L"INSERT";
    if (key == VK_HOME) return L"HOME";
    if (key == VK_END) return L"END";
    if (key == VK_DELETE) return L"DELETE";
    return L"F12";
}

void LoadConfiguration() {
    wchar_t hotkey[32]{};
    GetPrivateProfileStringW(L"Overlay", L"Hotkey", L"F12", hotkey, 32, g_iniPath.c_str());
    g_hotkey.store(ParseHotkey(hotkey));
    const int mode = GetPrivateProfileIntW(L"Graphics", L"AspectMode", 0, g_iniPath.c_str());
    // Versions through 1.7 stored the pillarbox mode as 2. Treat every old
    // non-zero value as enabled now that the full-frame test mode is gone.
    g_aspectMode.store(mode == 0 ? static_cast<LONG>(AspectMode::Off)
                                 : static_cast<LONG>(AspectMode::Pillarbox));
    g_spritesOverPillarbox.store(
        GetPrivateProfileIntW(L"Graphics", L"SpritesOverPillarbox", 1, g_iniPath.c_str()) != 0);
    g_textboxOpacity.store(std::clamp(static_cast<LONG>(
        GetPrivateProfileIntW(L"Graphics", L"TextboxOpacity", 100, g_iniPath.c_str())), 0L, 200L));
    g_textboxColorR.store(std::clamp(static_cast<LONG>(
        GetPrivateProfileIntW(L"Graphics", L"TextboxColorR", 255, g_iniPath.c_str())), 0L, 255L));
    g_textboxColorG.store(std::clamp(static_cast<LONG>(
        GetPrivateProfileIntW(L"Graphics", L"TextboxColorG", 255, g_iniPath.c_str())), 0L, 255L));
    g_textboxColorB.store(std::clamp(static_cast<LONG>(
        GetPrivateProfileIntW(L"Graphics", L"TextboxColorB", 255, g_iniPath.c_str())), 0L, 255L));
    g_restoreCursor.store(GetPrivateProfileIntW(L"Input", L"RestoreCursor", 1,
                                                 g_iniPath.c_str()) != 0);
    g_matchVoicedTextSpeed.store(GetPrivateProfileIntW(
        L"Text", L"MatchVoicedTextSpeed", 0, g_iniPath.c_str()) != 0);
    g_continueVoiceUntilNext.store(GetPrivateProfileIntW(
        L"Audio", L"ContinueVoiceUntilNextVoice", 0, g_iniPath.c_str()) != 0);
    g_highResolution.store(GetPrivateProfileIntW(L"Rendering", L"HighResolution", 0,
                                                  g_iniPath.c_str()) != 0);
    const int renderWidth = GetPrivateProfileIntW(L"Rendering", L"Width", 0, g_iniPath.c_str());
    const int renderHeight = GetPrivateProfileIntW(L"Rendering", L"Height", 0, g_iniPath.c_str());
    g_configuredRenderWidth = renderWidth >= 1920 && renderWidth <= 16384
        ? static_cast<UINT>(renderWidth) : 0;
    g_configuredRenderHeight = renderHeight >= 1080 && renderHeight <= 16384
        ? static_cast<UINT>(renderHeight) : 0;
    if (g_highResolution.load()) {
        const UINT initialWidth = g_configuredRenderWidth
            ? g_configuredRenderWidth : static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
        const UINT initialHeight = g_configuredRenderHeight
            ? g_configuredRenderHeight : static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));
        if (initialWidth > 1920 && initialHeight > 1080) {
            g_nativeRenderWidth.store(initialWidth);
            g_nativeRenderHeight.store(initialHeight);
        }
    }
    g_logging.store(GetPrivateProfileIntW(L"Debug", L"Logging", 1, g_iniPath.c_str()) != 0);
    g_wrapEnabled = GetPrivateProfileIntW(L"ExperimentalWrapping", L"Enabled", 0, g_iniPath.c_str()) != 0;
    const int width = GetPrivateProfileIntW(L"ExperimentalWrapping", L"Width", 1320, g_iniPath.c_str());
    const int caller = GetPrivateProfileIntW(L"ExperimentalWrapping", L"Caller", 0, g_iniPath.c_str());
    // Zero width is an observation-only mode. Reject malformed values.
    g_wrapWidth = width >= 0 && width <= 65535 ? static_cast<UINT>(width) : 0;
    g_wrapCaller = caller >= 0 && caller <= 6 ? static_cast<UINT>(caller) : 7;
}

#include "experimental_wrapping.inl"
#include "voice_text_speed.inl"
#include "voice_continue.inl"

void SaveAspectMode() {
    const std::wstring value = std::to_wstring(g_aspectMode.load());
    WritePrivateProfileStringW(L"Graphics", L"AspectMode", value.c_str(), g_iniPath.c_str());
}

void SaveSpritesOverPillarbox() {
    WritePrivateProfileStringW(L"Graphics", L"SpritesOverPillarbox",
                               g_spritesOverPillarbox.load() ? L"1" : L"0", g_iniPath.c_str());
}

void SaveTextboxStyle() {
    const std::wstring opacity = std::to_wstring(g_textboxOpacity.load());
    const std::wstring red = std::to_wstring(g_textboxColorR.load());
    const std::wstring green = std::to_wstring(g_textboxColorG.load());
    const std::wstring blue = std::to_wstring(g_textboxColorB.load());
    WritePrivateProfileStringW(L"Graphics", L"TextboxOpacity", opacity.c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"Graphics", L"TextboxColorR", red.c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"Graphics", L"TextboxColorG", green.c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"Graphics", L"TextboxColorB", blue.c_str(), g_iniPath.c_str());
}

void SaveVoiceTextSpeed() {
    WritePrivateProfileStringW(L"Text", L"MatchVoicedTextSpeed",
                               g_matchVoicedTextSpeed.load() ? L"1" : L"0",
                               g_iniPath.c_str());
}

void SaveVoiceContinuation() {
    WritePrivateProfileStringW(L"Audio", L"ContinueVoiceUntilNextVoice",
                               g_continueVoiceUntilNext.load() ? L"1" : L"0",
                               g_iniPath.c_str());
}

const wchar_t* AspectModeName() {
    switch (static_cast<AspectMode>(g_aspectMode.load())) {
    case AspectMode::Pillarbox: return L"4:3 pillarbox";
    default: return L"Off";
    }
}

bool StoryPillarboxActive() {
    return static_cast<AspectMode>(g_aspectMode.load()) == AspectMode::Pillarbox &&
           (g_lastBackgroundAdjusted.load() || g_storyPillarboxActive.load());
}

template <typename Function>
bool HookVtable(void* object, size_t index, Function replacement, Function& original) {
    if (!object) return false;
    auto*** objectAsVtable = reinterpret_cast<void***>(object);
    void** slot = &(*objectAsVtable)[index];
    if (*slot == reinterpret_cast<void*>(replacement)) return true;
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    if (!original) original = reinterpret_cast<Function>(*slot);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(replacement));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

bool ReplaceImport(HMODULE module, const char* importedDll, const char* importedFunction,
                   void* replacement, void** original) {
    if (!module) return false;
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dllName, importedDll) != 0) continue;
        auto* names = descriptor->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk)
            : reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(byName->Name), importedFunction) != 0) continue;
            auto* slot = reinterpret_cast<void**>(&addresses->u1.Function);
            if (*slot == replacement) return true;
            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
            if (!*original) *original = *slot;
            InterlockedExchangePointer(slot, replacement);
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
            return true;
        }
    }
    return false;
}

#include "xaudio_trace.inl"

void GetTextureDimensions(ID3D11ShaderResourceView* view, UINT& width, UINT& height) {
    width = 0;
    height = 0;
    if (view) {
        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        ID3D11Texture2D* texture = nullptr;
        if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            width = desc.Width;
            height = desc.Height;
        }
        SafeRelease(texture);
        SafeRelease(resource);
    }
}

void UpdateTextureDimensions(ID3D11ShaderResourceView* view) {
    UINT width = 0;
    UINT height = 0;
    GetTextureDimensions(view, width, height);
    g_textureWidth.store(width);
    g_textureHeight.store(height);
}

void SaveBufferSnapshot(ID3D11Resource* resource, const void* data, UINT byteWidth) {
    if (!resource || !data || !byteWidth) return;
    AcquireSRWLockExclusive(&g_snapshotLock);
    BufferSnapshot* target = nullptr;
    for (auto& snapshot : g_bufferSnapshots) {
        if (snapshot.resource == resource) {
            target = &snapshot;
            break;
        }
        if (!target && !snapshot.resource) target = &snapshot;
    }
    if (!target) target = &g_bufferSnapshots[reinterpret_cast<uintptr_t>(resource) % 32];
    target->resource = resource;
    target->byteWidth = byteWidth;
    target->copiedBytes = std::min<UINT>(byteWidth, sizeof(target->bytes));
    std::memcpy(target->bytes, data, target->copiedBytes);
    ReleaseSRWLockExclusive(&g_snapshotLock);
}

bool LoadBufferSnapshot(ID3D11Resource* resource, BufferSnapshot& result) {
    if (!resource) return false;
    bool found = false;
    AcquireSRWLockShared(&g_snapshotLock);
    for (const auto& snapshot : g_bufferSnapshots) {
        if (snapshot.resource == resource) {
            result = snapshot;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_snapshotLock);
    return found;
}

ContextHooks* FindContextHooks(ID3D11DeviceContext* context) {
    void** vtable = *reinterpret_cast<void***>(context);
    for (auto& hooks : g_contextHooks) {
        if (hooks.vtable.load(std::memory_order_acquire) == vtable) return &hooks;
    }
    return nullptr;
}

struct SceneScope {
    SceneState* previous = g_currentScene;
    SceneScope(ID3D11DeviceContext* context) {
        if (g_nativeWindows && !g_internalDraw) {
            if (auto* hooks = FindContextHooks(context)) g_currentScene = &hooks->scene;
        }
    }
    ~SceneScope() { g_currentScene = previous; }
};
void STDMETHODCALLTYPE HookIASetVertexBuffers(ID3D11DeviceContext* context, UINT start, UINT count,
    ID3D11Buffer* const* buffers, const UINT* strides, const UINT* offsets) {
    if (auto* hooks = FindContextHooks(context); hooks && hooks->iaSetVertexBuffers)
        hooks->iaSetVertexBuffers(context, start, count, buffers, strides, offsets);
    HookContext(context);
}
void STDMETHODCALLTYPE HookIASetInputLayout(ID3D11DeviceContext* context, ID3D11InputLayout* layout) {
    if (auto* hooks = FindContextHooks(context); hooks && hooks->iaSetInputLayout)
        hooks->iaSetInputLayout(context, layout);
    HookContext(context);
}
void STDMETHODCALLTYPE HookIASetPrimitiveTopology(ID3D11DeviceContext* context, D3D11_PRIMITIVE_TOPOLOGY topology) {
    if (auto* hooks = FindContextHooks(context); hooks && hooks->iaSetPrimitiveTopology)
        hooks->iaSetPrimitiveTopology(context, topology);
    HookContext(context);
}
void STDMETHODCALLTYPE HookVSSetShader(ID3D11DeviceContext* context, ID3D11VertexShader* shader,
    ID3D11ClassInstance* const* instances, UINT count) {
    if (auto* hooks = FindContextHooks(context); hooks && hooks->vsSetShader)
        hooks->vsSetShader(context, shader, instances, count);
    HookContext(context);
}
void STDMETHODCALLTYPE HookGSSetShader(ID3D11DeviceContext* context, ID3D11GeometryShader* shader,
    ID3D11ClassInstance* const* instances, UINT count) {
    if (auto* hooks = FindContextHooks(context); hooks && hooks->gsSetShader)
        hooks->gsSetShader(context, shader, instances, count);
    HookContext(context);
}
HRESULT STDMETHODCALLTYPE HookFinishCommandList(ID3D11DeviceContext* context, BOOL restore,
                                                ID3D11CommandList** list) {
    auto* hooks = FindContextHooks(context);
    if (!hooks || !hooks->finishCommandList) return E_FAIL;
    const HRESULT result = hooks->finishCommandList(context, restore, list);
    if (SUCCEEDED(result)) {
        SceneScope scope(context);
        g_backgroundCleared.store(false);
        g_lastBackgroundAdjusted.store(false);
        g_lastUiChromeAdjusted.store(false);
        g_lastTextAdjusted.store(false);
        g_nvlUiSeen.store(false);
        g_messageUiSeen.store(false);
        g_systemUiSeen.store(false);
        g_frameBackgroundVertexShader.store(nullptr);
    }
    HookContext(context);
    return result;
}

// Sample actual context state independently of the global classifier cache.
// Bounded logging also proves whether the game reaches our draw hooks at all.
void DiagnoseDraw(ID3D11DeviceContext* context, bool indexed, UINT count) {
    if (g_internalDraw || !g_logging.load()) return;
    ContextHooks* hooks = FindContextHooks(context);
    if (!hooks) return;
    if (indexed) hooks->indexedDraws.fetch_add(1);
    else hooks->draws.fetch_add(1);
    if (hooks->samples.fetch_add(1) >= 8) return;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    ID3D11ShaderResourceView* views[8]{};
    context->PSGetShaderResources(0, 8, views);
    UINT widths[8]{}, heights[8]{};
    for (unsigned i = 0; i < 8; ++i) {
        GetTextureDimensions(views[i], widths[i], heights[i]);
        SafeRelease(views[i]);
    }
    UINT viewportCount = 1;
    D3D11_VIEWPORT viewport{};
    context->RSGetViewports(&viewportCount, &viewport);
    Log("Diagnostic draw: context=%p device=%p type=%u indexed=%d count=%u mode=%ld viewport-count=%u viewport=%.0fx%.0f cached-t0=%ux%u actual-textures=[%ux%u,%ux%u,%ux%u,%ux%u,%ux%u,%ux%u,%ux%u,%ux%u]",
        context, device, static_cast<unsigned>(context->GetType()), indexed, count,
        g_aspectMode.load(), viewportCount, viewport.Width, viewport.Height,
        TextureWidth(0).load(), TextureHeight(0).load(),
        widths[0], heights[0], widths[1], heights[1], widths[2], heights[2],
        widths[3], heights[3], widths[4], heights[4], widths[5], heights[5],
        widths[6], heights[6], widths[7], heights[7]);
    SafeRelease(device);
}

void DiagnoseContexts() {
    for (auto& hooks : g_contextHooks) {
        void** table = hooks.vtable.load(std::memory_order_acquire);
        if (!table) continue;
        Log("Diagnostic context totals: vtable=%p Draw=%llu DrawIndexed=%llu PSSetSRV=%llu",
            table, hooks.draws.load(), hooks.indexedDraws.load(), hooks.textureSets.load());
        hooks.samples.store(0);
    }
}

HRESULT STDMETHODCALLTYPE HookMap(ID3D11DeviceContext* context, ID3D11Resource* resource,
                                  UINT subresource, D3D11_MAP mapType, UINT mapFlags,
                                  D3D11_MAPPED_SUBRESOURCE* mapped) {
    ContextHooks* hooks = FindContextHooks(context);
    if (!hooks || !hooks->map) return E_FAIL;
    const HRESULT result = hooks->map(context, resource, subresource, mapType, mapFlags, mapped);
    HookContext(context);
    g_activeMap = {};
    if (SUCCEEDED(result) && resource && mapped && mapped->pData && subresource == 0) {
        ID3D11Buffer* buffer = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Buffer),
                                               reinterpret_cast<void**>(&buffer)))) {
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            g_activeMap = {resource, mapped->pData, desc.ByteWidth};
            buffer->Release();
        }
    }
    return result;
}

void STDMETHODCALLTYPE HookUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource,
                                 UINT subresource) {
    if (subresource == 0 && g_activeMap.resource == resource) {
        if (g_activeMap.byteWidth >= sizeof(g_bufferSnapshots[0].bytes)) {
            SaveBufferSnapshot(resource, g_activeMap.data, g_activeMap.byteWidth);
        }
        g_activeMap = {};
    }
    if (ContextHooks* hooks = FindContextHooks(context); hooks && hooks->unmap) {
        hooks->unmap(context, resource, subresource);
    }
}

void UpdateRenderTargetDimensions(ID3D11RenderTargetView* view) {
    UINT width = 0;
    UINT height = 0;
    if (view) {
        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        ID3D11Texture2D* texture = nullptr;
        if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            width = desc.Width;
            height = desc.Height;
        }
        SafeRelease(texture);
        SafeRelease(resource);
    }
    g_rtWidth.store(width);
    g_rtHeight.store(height);
}

bool StoryGameModuleLoaded() {
    return GetModuleHandleW(L"MyMerryMay_WIN_Master.dll") != nullptr ||
           GetModuleHandleW(L"MyMerryMaybe_WIN_Master.dll") != nullptr;
}

void UpdateNativeRenderResolution(HWND window) {
    if (!g_highResolution.load()) return;
    UINT width = g_configuredRenderWidth;
    UINT height = g_configuredRenderHeight;
    if (!width || !height) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!window || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        width = static_cast<UINT>(std::max<LONG>(0, monitor.rcMonitor.right - monitor.rcMonitor.left));
        height = static_cast<UINT>(std::max<LONG>(0, monitor.rcMonitor.bottom - monitor.rcMonitor.top));
    }
    if (width <= 1920 || height <= 1080) return;
    const UINT oldWidth = g_nativeRenderWidth.exchange(width);
    const UINT oldHeight = g_nativeRenderHeight.exchange(height);
    if (oldWidth != width || oldHeight != height) {
        Log("High-resolution target selected: %ux%u (%s)", width, height,
            g_configuredRenderWidth && g_configuredRenderHeight ? "configured" : "monitor");
    }
}

bool ContextUsesHighResolutionTarget(ID3D11DeviceContext* context) {
    const UINT targetWidth = g_nativeRenderWidth.load();
    const UINT targetHeight = g_nativeRenderHeight.load();
    if (!g_highResolution.load() || !targetWidth || !targetHeight) return false;
    ID3D11RenderTargetView* view = nullptr;
    context->OMGetRenderTargets(1, &view, nullptr);
    if (!view) return false;
    ID3D11Resource* resource = nullptr;
    ID3D11Texture2D* texture = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    view->GetResource(&resource);
    if (resource) resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    if (texture) texture->GetDesc(&desc);
    SafeRelease(texture);
    SafeRelease(resource);
    SafeRelease(view);
    return desc.Width == targetWidth && desc.Height == targetHeight;
}

void STDMETHODCALLTYPE HookPSSetShaderResources(ID3D11DeviceContext* context, UINT startSlot,
                                                UINT numberOfViews, ID3D11ShaderResourceView* const* views) {
    SceneScope sceneScope(context);
    if (!g_internalDraw) {
        if (auto* hooks = FindContextHooks(context)) hooks->textureSets.fetch_add(1);
    }
    if (!g_internalDraw && numberOfViews > 0) {
        const UINT endSlot = std::min(startSlot + numberOfViews, 8u);
        for (UINT slot = startSlot; slot < endSlot; ++slot) {
            UINT width = 0;
            UINT height = 0;
            GetTextureDimensions(views ? views[slot - startSlot] : nullptr, width, height);
            // System screens share the story font and UI atlases. Their own
            // large sheets are stable scene markers, so stop applying story
            // layout corrections to draws that follow them in this frame.
            if ((width == 4096 && height == 2169) || // MMM savemenu.png
                (width == 2048 && height == 3118) || // MMMB savemenu.png
                (width == 2048 && height == 2536) || // menu_chip.png / dialogs
                (width == 3584 && height == 2311) || // MMM config.png
                (width == 3840 && height == 2049) || // MMMB config.png
                (width == 2048 && height == 1514) || // backlog.png
                (width == 4096 && height == 1273) || // MMM album.png
                (width == 2560 && height == 1137) || // MMMB album.png
                (width == 3072 && height == 1137) || // movmenu.png
                (width == 2048 && height == 1201) || // clearlist.png
                (width == 2048 && height == 1221) || // music.png
                (width == 2048 && height == 2089) || // MMMB title_chip.png
                (width == 4096 && height == 365)) {  // MMMB map.png
                g_systemUiSeen.store(true);
                g_storyPillarboxActive.store(false);
            }
            TextureWidth(slot).store(width);
            TextureHeight(slot).store(height);
            if (slot == 0) {
                g_textureWidth.store(width);
                g_textureHeight.store(height);
            }
        }
    }
    if (ContextHooks* hooks = FindContextHooks(context); hooks && hooks->psSetShaderResources) {
        hooks->psSetShaderResources(context, startSlot, numberOfViews, views);
    }
    // Native D3D11 changes dispatch entries as context state changes. Refresh
    // after the runtime setter, before the following game draw.
    HookContext(context);
}

void STDMETHODCALLTYPE HookOMSetRenderTargets(ID3D11DeviceContext* context, UINT numberOfViews,
                                              ID3D11RenderTargetView* const* views,
                                              ID3D11DepthStencilView* depthView) {
    if (!g_internalDraw) {
        UpdateRenderTargetDimensions(numberOfViews && views ? views[0] : nullptr);
    }
    if (ContextHooks* hooks = FindContextHooks(context); hooks && hooks->omSetRenderTargets) {
        hooks->omSetRenderTargets(context, numberOfViews, views, depthView);
    }
    HookContext(context);
}

void STDMETHODCALLTYPE HookRSSetViewports(ID3D11DeviceContext* context, UINT numberOfViewports,
                                          const D3D11_VIEWPORT* viewports) {
    const D3D11_VIEWPORT* forwarded = viewports;
    D3D11_VIEWPORT adjusted[16]{};
    if (!g_internalDraw && numberOfViewports && viewports && numberOfViewports <= 16 &&
        ContextUsesHighResolutionTarget(context)) {
        const float scaleX = g_nativeRenderWidth.load() / 1920.0f;
        const float scaleY = g_nativeRenderHeight.load() / 1080.0f;
        bool changed = false;
        for (UINT i = 0; i < numberOfViewports; ++i) {
            adjusted[i] = viewports[i];
            if (viewports[i].Width <= 1920.5f && viewports[i].Height <= 1080.5f) {
                adjusted[i].TopLeftX *= scaleX;
                adjusted[i].TopLeftY *= scaleY;
                adjusted[i].Width *= scaleX;
                adjusted[i].Height *= scaleY;
                changed = true;
            }
        }
        if (changed) forwarded = adjusted;
    }
    if (!g_internalDraw && numberOfViewports && forwarded) {
        g_viewportX.store(forwarded[0].TopLeftX);
        g_viewportY.store(forwarded[0].TopLeftY);
        g_viewportWidth.store(forwarded[0].Width);
        g_viewportHeight.store(forwarded[0].Height);
    }
    if (ContextHooks* hooks = FindContextHooks(context); hooks && hooks->rsSetViewports) {
        hooks->rsSetViewports(context, numberOfViewports, forwarded);
    }
    HookContext(context);
}

void STDMETHODCALLTYPE HookRSSetScissorRects(ID3D11DeviceContext* context, UINT numberOfRects,
                                              const D3D11_RECT* rects) {
    const D3D11_RECT* forwarded = rects;
    D3D11_RECT adjusted[16]{};
    if (!g_internalDraw && numberOfRects && rects && numberOfRects <= 16 &&
        ContextUsesHighResolutionTarget(context)) {
        const double scaleX = g_nativeRenderWidth.load() / 1920.0;
        const double scaleY = g_nativeRenderHeight.load() / 1080.0;
        for (UINT i = 0; i < numberOfRects; ++i) {
            adjusted[i].left = static_cast<LONG>(rects[i].left * scaleX);
            adjusted[i].right = static_cast<LONG>(rects[i].right * scaleX);
            adjusted[i].top = static_cast<LONG>(rects[i].top * scaleY);
            adjusted[i].bottom = static_cast<LONG>(rects[i].bottom * scaleY);
        }
        forwarded = adjusted;
    }
    if (ContextHooks* hooks = FindContextHooks(context); hooks && hooks->rsSetScissorRects) {
        hooks->rsSetScissorRects(context, numberOfRects, forwarded);
    }
    HookContext(context);
}

int CandidateBackgroundSlot() {
    // The game's original backgrounds are 1920 pixels wide. Most are 1920x1440;
    // taller textures are animation strips made from the same 4:3 source frames.
    // Both games sample the visible background from t0. Looking through every
    // slot allowed an old, still-bound texture to classify unrelated later
    // passes as backgrounds (most visibly, a white NVL pass in Maybe).
    return TextureWidth(0).load() == 1920 && TextureHeight(0).load() >= 1400 ? 0 : -1;
}

void LogCandidateDraw(ID3D11DeviceContext* context, const char* call, UINT count) {
    const unsigned sequence = g_candidateLogCount.fetch_add(1);
    if (sequence >= 32) return;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    context->IAGetPrimitiveTopology(&topology);
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    context->VSGetShader(&vertexShader, nullptr, nullptr);
    context->PSGetShader(&pixelShader, nullptr, nullptr);
    const int slot = CandidateBackgroundSlot();
    Log("4:3 candidate #%u: %s count=%u topology=%u slot=%d texture=%ux%u VS=%p PS=%p",
        sequence + 1, call, count, static_cast<unsigned>(topology), slot,
        slot >= 0 ? TextureWidth(slot).load() : 0,
        slot >= 0 ? TextureHeight(slot).load() : 0, vertexShader, pixelShader);
    SafeRelease(vertexShader);
    SafeRelease(pixelShader);
}

void LogUiDraw(ID3D11DeviceContext* context, const char* call, UINT count) {
    const unsigned sequence = g_uiLogCount.fetch_add(1);
    if (sequence >= 96) return;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    context->IAGetPrimitiveTopology(&topology);
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    context->VSGetShader(&vertexShader, nullptr, nullptr);
    context->PSGetShader(&pixelShader, nullptr, nullptr);
    Log("post-4:3 UI #%u: %s count=%u topology=%u textures=[%ux%u,%ux%u,%ux%u,%ux%u] VS=%p PS=%p",
        sequence + 1, call, count, static_cast<unsigned>(topology),
        TextureWidth(0).load(), TextureHeight(0).load(),
        TextureWidth(1).load(), TextureHeight(1).load(),
        TextureWidth(2).load(), TextureHeight(2).load(),
        TextureWidth(3).load(), TextureHeight(3).load(),
        vertexShader, pixelShader);
    SafeRelease(vertexShader);
    SafeRelease(pixelShader);
}

unsigned UiGeometryClass() {
    const UINT width = TextureWidth(0).load();
    const UINT height = TextureHeight(0).load();
    if (width == 4096 && height == 7546) return 1u;
    if (width == 4096 && (height == 460 || height == 464)) return 2u;
    if (width == 512 && height == 923) return 4u;
    return 0u;
}

void LogBufferFloats(const char* label, ID3D11Buffer* buffer, UINT stride, UINT offset) {
    if (!buffer) {
        Log("UI geometry %s: none", label);
        return;
    }
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    BufferSnapshot snapshot{};
    const bool captured = LoadBufferSnapshot(buffer, snapshot);
    float values[12]{};
    UINT valueCount = 0;
    if (captured && snapshot.copiedBytes > offset) {
        const UINT available = std::min<UINT>(snapshot.copiedBytes - offset, sizeof(values));
        valueCount = available / sizeof(float);
        std::memcpy(values, snapshot.bytes + offset, valueCount * sizeof(float));
    }
    Log("UI geometry %s=%p bytes=%u usage=%u bind=0x%x cpu=0x%x stride=%u offset=%u snapshot=%d "
        "f=[%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g]",
        label, buffer, desc.ByteWidth, static_cast<unsigned>(desc.Usage), desc.BindFlags,
        desc.CPUAccessFlags, stride, offset, captured ? 1 : 0,
        valueCount > 0 ? values[0] : 0.0f, valueCount > 1 ? values[1] : 0.0f,
        valueCount > 2 ? values[2] : 0.0f, valueCount > 3 ? values[3] : 0.0f,
        valueCount > 4 ? values[4] : 0.0f, valueCount > 5 ? values[5] : 0.0f,
        valueCount > 6 ? values[6] : 0.0f, valueCount > 7 ? values[7] : 0.0f,
        valueCount > 8 ? values[8] : 0.0f, valueCount > 9 ? values[9] : 0.0f,
        valueCount > 10 ? values[10] : 0.0f, valueCount > 11 ? values[11] : 0.0f);

    if (captured && stride >= sizeof(float) * 2 && snapshot.copiedBytes >= offset + stride * 4) {
        for (UINT vertex = 0; vertex < 4; ++vertex) {
            float fields[9]{};
            const UINT fieldCount = std::min<UINT>(stride / sizeof(float), 9);
            std::memcpy(fields, snapshot.bytes + offset + vertex * stride,
                        fieldCount * sizeof(float));
            Log("UI geometry %s vertex%u f=[%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g]",
                label, vertex, fields[0], fields[1], fields[2], fields[3], fields[4],
                fields[5], fields[6], fields[7], fields[8]);
        }
    }
}

void LogUiGeometry(ID3D11DeviceContext* context) {
    const unsigned geometryClass = UiGeometryClass();
    if (!geometryClass) return;
    const unsigned previous = g_geometryLogMask.fetch_or(geometryClass);
    if (previous & geometryClass) return;

    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vertexBuffer = nullptr;
    UINT stride = 0;
    UINT offset = 0;
    context->IAGetInputLayout(&layout);
    context->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    Log("UI geometry class=%u texture=%ux%u input-layout=%p", geometryClass,
        TextureWidth(0).load(), TextureHeight(0).load(), layout);
    LogBufferFloats("VB0", vertexBuffer, stride, offset);
    SafeRelease(layout);
    SafeRelease(vertexBuffer);

    ID3D11Buffer* constantBuffers[2]{};
    context->VSGetConstantBuffers(0, 2, constantBuffers);
    LogBufferFloats("VS-CB0", constantBuffers[0], 0, 0);
    LogBufferFloats("VS-CB1", constantBuffers[1], 0, 0);
    SafeRelease(constantBuffers[0]);
    SafeRelease(constantBuffers[1]);
}

bool BeginBackgroundCorrection(ID3D11DeviceContext* context, D3D11_VIEWPORT& originalViewport) {
    if (g_internalDraw || static_cast<AspectMode>(g_aspectMode.load()) != AspectMode::Pillarbox) {
        return false;
    }
    if (CandidateBackgroundSlot() < 0) return false;

    // Maybe emits two source-background draws followed by two effect draws,
    // all while the same 1920x1440 texture remains bound. Correct every draw
    // that uses the first frame-local VS, but leave the later effect VS alone.
    // Correcting only one source draw lets the duplicate restore 16:9;
    // correcting the effect shader turns some NVL scenes white.
    ID3D11VertexShader* currentVertexShader = nullptr;
    context->VSGetShader(&currentVertexShader, nullptr, nullptr);
    ID3D11VertexShader* expected = nullptr;
    g_frameBackgroundVertexShader.compare_exchange_strong(expected, currentVertexShader);
    const bool sourceBackgroundShader = g_frameBackgroundVertexShader.load() == currentVertexShader;
    SafeRelease(currentVertexShader);
    if (!sourceBackgroundShader) return false;

    UINT count = 1;
    context->RSGetViewports(&count, &originalViewport);
    if (!count || originalViewport.Height <= 0.0f || originalViewport.Width / originalViewport.Height < 1.5f) {
        return false;
    }
    const float correctedWidth = originalViewport.Height * (4.0f / 3.0f);
    if (correctedWidth >= originalViewport.Width - 1.0f) return false;

    if (!g_backgroundCleared.exchange(true)) {
        ID3D11RenderTargetView* target = nullptr;
        context->OMGetRenderTargets(1, &target, nullptr);
        if (target) {
            constexpr float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
            context->ClearRenderTargetView(target, black);
            target->Release();
        }
    }

    D3D11_VIEWPORT corrected = originalViewport;
    corrected.TopLeftX += (originalViewport.Width - correctedWidth) * 0.5f;
    corrected.Width = correctedWidth;
    ContextHooks* hooks = FindContextHooks(context);
    if (!hooks || !hooks->rsSetViewports) return false;
    hooks->rsSetViewports(context, 1, &corrected);
    g_lastBackgroundAdjusted.store(true);
    g_storyPillarboxActive.store(true);
    return true;
}

bool IsUiChromeTexture() {
    const UINT width = TextureWidth(0).load();
    const UINT height = TextureHeight(0).load();
    return (width == 4096 && (height == 460 || height == 464)) || // MMM / MMMB ADV
           (width == 512 && height == 923);                       // shared NVL panel
}

bool IsAdvTextboxTexture() {
    return TextureWidth(0).load() == 4096 &&
           (TextureHeight(0).load() == 460 || TextureHeight(0).load() == 464);
}

bool IsAdvTextboxAtlasDraw(ID3D11DeviceContext* context, UINT vertexCount) {
    if (!IsAdvTextboxTexture() || vertexCount != 4) return false;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    context->IAGetPrimitiveTopology(&topology);
    // Bind for every quad sourced from the ADV UI atlas. TextboxPS limits the
    // actual colour/opacity change to the panel's atlas rectangle. This also
    // catches the engine's differently shaped fade/transition copies without
    // recolouring the name plate, AUTO/SKIP, or the remaining atlas chrome.
    return topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
}

bool TextboxStyleIsOriginal() {
    return g_textboxOpacity.load() == 100 && g_textboxColorR.load() == 255 &&
           g_textboxColorG.load() == 255 && g_textboxColorB.load() == 255;
}

struct TextboxStyleScope {
    ID3D11DeviceContext* context = nullptr;
    ID3D11PixelShader* originalShader = nullptr;
    ID3D11Buffer* originalBuffer = nullptr;
    bool active = false;

    TextboxStyleScope(ID3D11DeviceContext* drawContext, bool textboxDraw) : context(drawContext) {
        if (g_internalDraw || !textboxDraw || TextboxStyleIsOriginal()) return;
        ID3D11PixelShader* shader = g_textboxPixelShader.load(std::memory_order_acquire);
        ID3D11Buffer* buffer = g_textboxStyleBuffer.load(std::memory_order_acquire);
        if (!shader || !buffer) return;

        struct StyleConstants { float tint[3]; float opacity; } constants{
            {g_textboxColorR.load() / 255.0f, g_textboxColorG.load() / 255.0f,
             g_textboxColorB.load() / 255.0f},
            g_textboxOpacity.load() / 100.0f
        };
        context->PSGetShader(&originalShader, nullptr, nullptr);
        context->PSGetConstantBuffers(7, 1, &originalBuffer);
        context->UpdateSubresource(buffer, 0, nullptr, &constants, 0, 0);
        context->PSSetShader(shader, nullptr, 0);
        context->PSSetConstantBuffers(7, 1, &buffer);
        active = true;
    }

    ~TextboxStyleScope() {
        if (!active) return;
        context->PSSetShader(originalShader, nullptr, 0);
        context->PSSetConstantBuffers(7, 1, &originalBuffer);
        SafeRelease(originalShader);
        SafeRelease(originalBuffer);
    }
};

bool BeginUiChromeCorrection(ID3D11DeviceContext* context, D3D11_VIEWPORT& originalViewport) {
    if (g_internalDraw || static_cast<AspectMode>(g_aspectMode.load()) != AspectMode::Pillarbox ||
        g_systemUiSeen.load() || !IsUiChromeTexture()) {
        return false;
    }
    const UINT textureWidth = TextureWidth(0).load();
    const UINT textureHeight = TextureHeight(0).load();
    const bool advStoryPanel = textureWidth == 4096 &&
                               (textureHeight == 460 || textureHeight == 464);
    // The ADV panel is a reliable story marker even when the current backdrop
    // is the port's 1920x1080 black texture. It also lets a save loaded directly
    // into a black scene establish 4:3 without requiring an earlier frame.
    if (!StoryPillarboxActive() && advStoryPanel) g_storyPillarboxActive.store(true);
    if (!StoryPillarboxActive()) return false;
    UINT count = 1;
    context->RSGetViewports(&count, &originalViewport);
    if (!count || originalViewport.Height <= 0.0f || originalViewport.Width / originalViewport.Height < 1.5f) {
        return false;
    }
    const float correctedWidth = originalViewport.Height * (4.0f / 3.0f);
    if (correctedWidth >= originalViewport.Width - 1.0f) return false;
    if (TextureWidth(0).load() == 512 && TextureHeight(0).load() == 923) {
        g_nvlUiSeen.store(true);
    }
    g_messageUiSeen.store(true);
    D3D11_VIEWPORT corrected = originalViewport;
    corrected.TopLeftX += (originalViewport.Width - correctedWidth) * 0.5f;
    corrected.Width = correctedWidth;
    ContextHooks* hooks = FindContextHooks(context);
    if (!hooks || !hooks->rsSetViewports) return false;
    hooks->rsSetViewports(context, 1, &corrected);
    g_lastUiChromeAdjusted.store(true);
    return true;
}

bool UseMayNvlCorrection() {
    // Reveal glyphs can be recorded on a different deferred context, without
    // any message-panel draw. The engine body layout is available before the
    // first glyph; only the older panel-based path needs both UI markers.
    return g_engineNvlLayout.load() ||
           (g_messageUiSeen.load() && g_nvlUiSeen.load());
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex) {
    SceneScope sceneScope(context);
    if (!g_internalDraw && static_cast<AspectMode>(g_aspectMode.load()) == AspectMode::Pillarbox &&
        !g_systemUiSeen.load() && g_engineNvlLayout.load()) {
        // Panel-free NVL can likewise begin on a black frame.
        g_storyPillarboxActive.store(true);
    }
    DiagnoseDraw(context, false, vertexCount);
    if (!g_internalDraw && g_overlayVisible.load() && g_lastBackgroundAdjusted.load() &&
        CandidateBackgroundSlot() < 0) {
        LogUiDraw(context, "Draw", vertexCount);
        LogUiGeometry(context);
    }
    if (!g_internalDraw && CandidateBackgroundSlot() >= 0) {
        LogCandidateDraw(context, "Draw", vertexCount);
    }
    D3D11_VIEWPORT viewport{};
    const bool backgroundCorrected = BeginBackgroundCorrection(context, viewport);
    const bool uiCorrected = !backgroundCorrected && BeginUiChromeCorrection(context, viewport);
    const bool corrected = backgroundCorrected || uiCorrected;
    ContextHooks* hooks = FindContextHooks(context);
    if (!hooks || !hooks->draw) return;
    TextboxStyleScope textboxStyle(context, IsAdvTextboxAtlasDraw(context, vertexCount));

    ID3D11VertexShader* originalVertexShader = nullptr;
    ID3D11InputLayout* originalInputLayout = nullptr;
    ID3D11VertexShader* replacementVertexShader = nullptr;
    ID3D11GeometryShader* originalGeometryShader = nullptr;
    ID3D11GeometryShader* replacementGeometryShader = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    context->IAGetPrimitiveTopology(&topology);
    if (backgroundCorrected && vertexCount == 4 && topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) {
        replacementVertexShader = g_backgroundVertexShader.load(std::memory_order_acquire);
    } else if (!g_internalDraw && StoryPillarboxActive() && !g_systemUiSeen.load() && vertexCount == 4 &&
               topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP &&
               TextureWidth(0).load() == 4096 && TextureHeight(0).load() == 7546) {
        const bool maybeGame = GetModuleHandleW(L"MyMerryMaybe_WIN_Master.dll") != nullptr;
        if (maybeGame) {
            // Maybe does not emit May's 512x923 NVL marker. Its font geometry
            // itself distinguishes upper-screen NVL from lower-screen ADV.
            replacementGeometryShader = g_maybeFontGeometryShader.load(std::memory_order_acquire);
        } else if (UseMayNvlCorrection()) {
            if (g_nativeWindows) {
                replacementGeometryShader = g_nativeNvlGeometryShader.load(std::memory_order_acquire);
            } else {
                replacementVertexShader = g_nvlFontVertexShader.load(std::memory_order_acquire);
            }
        } else if (g_messageUiSeen.load()) {
            replacementGeometryShader = g_advNameGeometryShader.load(std::memory_order_acquire);
        }
        if (replacementVertexShader || replacementGeometryShader) g_lastTextAdjusted.store(true);
        if (g_nativeWindows) {
            // Keep separate samples for each marker combination. ADV at launch
            // must not exhaust diagnostics before a marker-free NVL passage.
            static std::atomic<unsigned> textDecisionLogs[16]{};
            const UINT decision = (g_nvlUiSeen.load() ? 1 : 0) |
                (g_messageUiSeen.load() ? 2 : 0) | (g_systemUiSeen.load() ? 4 : 0) |
                (g_engineNvlLayout.load() ? 8 : 0);
            if (textDecisionLogs[decision].fetch_add(1) < 8) {
                Log("Native text decision: context=%p startVertex=%u NVL=%d message=%d system=%d VS=%p GS=%p engineNVL=%d",
                    context, startVertex, g_nvlUiSeen.load(), g_messageUiSeen.load(),
                    g_systemUiSeen.load(), replacementVertexShader, replacementGeometryShader,
                    g_engineNvlLayout.load());
                if (!g_nvlUiSeen.load()) {
                    ID3D11Buffer* vertexBuffer = nullptr;
                    UINT stride = 0, offset = 0;
                    context->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
                    LogBufferFloats("NVL-marker-absent VB0", vertexBuffer, stride, offset);
                    SafeRelease(vertexBuffer);
                }
            }
        }
    }
    if (replacementVertexShader) {
        context->VSGetShader(&originalVertexShader, nullptr, nullptr);
        if (backgroundCorrected || (g_nativeWindows && replacementVertexShader == g_nvlFontVertexShader.load())) {
            context->IAGetInputLayout(&originalInputLayout);
            context->IASetInputLayout(backgroundCorrected ? nullptr : g_nvlFontInputLayout.load());
        }
        context->VSSetShader(replacementVertexShader, nullptr, 0);
    }
    if (replacementGeometryShader) {
        context->GSGetShader(&originalGeometryShader, nullptr, nullptr);
        context->GSSetShader(replacementGeometryShader, nullptr, 0);
    }
    // Native D3D11 can select a different draw implementation when we bind
    // the correction shaders. Forwarding through the entry captured before
    // those binds can record the wrong pipeline into a deferred command list.
    // Wine/DXVK retains the previously tested shader and forwarding path.
    if (g_nativeWindows) HookContext(context);
    hooks->draw(context, vertexCount, startVertex);
    if (replacementGeometryShader) {
        context->GSSetShader(originalGeometryShader, nullptr, 0);
        SafeRelease(originalGeometryShader);
    }
    if (replacementVertexShader) {
        context->VSSetShader(originalVertexShader, nullptr, 0);
        if (backgroundCorrected || (g_nativeWindows && replacementVertexShader == g_nvlFontVertexShader.load())) {
            context->IASetInputLayout(originalInputLayout);
        }
        SafeRelease(originalVertexShader);
        SafeRelease(originalInputLayout);
    }
    if (corrected) hooks->rsSetViewports(context, 1, &viewport);
    if (g_nativeWindows) HookContext(context);
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* context, UINT indexCount,
                                       UINT startIndex, INT baseVertex) {
    SceneScope sceneScope(context);
    DiagnoseDraw(context, true, indexCount);
    if (!g_internalDraw && g_overlayVisible.load() && g_lastBackgroundAdjusted.load() &&
        CandidateBackgroundSlot() < 0) {
        LogUiDraw(context, "DrawIndexed", indexCount);
        LogUiGeometry(context);
    }
    if (!g_internalDraw && CandidateBackgroundSlot() >= 0) {
        LogCandidateDraw(context, "DrawIndexed", indexCount);
    }
    D3D11_VIEWPORT viewport{};
    const bool backgroundCorrected = BeginBackgroundCorrection(context, viewport);
    const bool uiCorrected = !backgroundCorrected && BeginUiChromeCorrection(context, viewport);
    const bool corrected = backgroundCorrected || uiCorrected;
    ContextHooks* hooks = FindContextHooks(context);
    if (!hooks || !hooks->drawIndexed) return;
    // The observed ADV panel is non-indexed. Avoid styling unrelated indexed
    // meschip elements until their geometry can be classified independently.
    TextboxStyleScope textboxStyle(context, false);
    if (g_nativeWindows) HookContext(context);
    hooks->drawIndexed(context, indexCount, startIndex, baseVertex);
    if (corrected) hooks->rsSetViewports(context, 1, &viewport);
    if (g_nativeWindows) HookContext(context);
}

struct PipelineBackup {
    ID3D11RenderTargetView* renderTarget = nullptr;
    ID3D11DepthStencilView* depthView = nullptr;
    ID3D11InputLayout* inputLayout = nullptr;
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11Buffer* vertexConstantBuffer = nullptr;
    ID3D11ShaderResourceView* shaderResource = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11BlendState* blendState = nullptr;
    ID3D11DepthStencilState* depthState = nullptr;
    ID3D11RasterizerState* rasterizerState = nullptr;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0;
    UINT stencilReference = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    explicit PipelineBackup(ID3D11DeviceContext* context) {
        context->OMGetRenderTargets(1, &renderTarget, &depthView);
        context->IAGetInputLayout(&inputLayout);
        context->IAGetPrimitiveTopology(&topology);
        context->VSGetShader(&vertexShader, nullptr, nullptr);
        context->PSGetShader(&pixelShader, nullptr, nullptr);
        context->VSGetConstantBuffers(0, 1, &vertexConstantBuffer);
        context->PSGetShaderResources(0, 1, &shaderResource);
        context->PSGetSamplers(0, 1, &sampler);
        context->OMGetBlendState(&blendState, blendFactor, &sampleMask);
        context->OMGetDepthStencilState(&depthState, &stencilReference);
        context->RSGetState(&rasterizerState);
        context->RSGetViewports(&viewportCount, viewports);
    }

    void Restore(ID3D11DeviceContext* context) {
        context->OMSetRenderTargets(1, &renderTarget, depthView);
        context->IASetInputLayout(inputLayout);
        context->IASetPrimitiveTopology(topology);
        context->VSSetShader(vertexShader, nullptr, 0);
        context->PSSetShader(pixelShader, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &vertexConstantBuffer);
        context->PSSetShaderResources(0, 1, &shaderResource);
        context->PSSetSamplers(0, 1, &sampler);
        context->OMSetBlendState(blendState, blendFactor, sampleMask);
        context->OMSetDepthStencilState(depthState, stencilReference);
        context->RSSetState(rasterizerState);
        if (viewportCount) context->RSSetViewports(viewportCount, viewports);
    }

    ~PipelineBackup() {
        SafeRelease(renderTarget);
        SafeRelease(depthView);
        SafeRelease(inputLayout);
        SafeRelease(vertexShader);
        SafeRelease(pixelShader);
        SafeRelease(vertexConstantBuffer);
        SafeRelease(shaderResource);
        SafeRelease(sampler);
        SafeRelease(blendState);
        SafeRelease(depthState);
        SafeRelease(rasterizerState);
    }
};

class OverlayRenderer {
public:
    ~OverlayRenderer() { ReleaseAll(); }

    void ReleaseTargets() {
        SafeRelease(backBufferView_);
        activeSwapChain_ = nullptr;
        width_ = 0;
        height_ = 0;
    }

    void ReleaseAll() {
        ReleaseTargets();
        SafeRelease(panelView_);
        SafeRelease(panelTexture_);
        SafeRelease(constantBuffer_);
        SafeRelease(sampler_);
        SafeRelease(blendState_);
        SafeRelease(depthState_);
        SafeRelease(rasterizerState_);
        SafeRelease(solidPixelShader_);
        SafeRelease(pixelShader_);
        g_textboxPixelShader.store(nullptr, std::memory_order_release);
        g_textboxStyleBuffer.store(nullptr, std::memory_order_release);
        SafeRelease(textboxStyleBuffer_);
        SafeRelease(textboxPixelShader_);
        g_backgroundVertexShader.store(nullptr, std::memory_order_release);
        g_nvlFontVertexShader.store(nullptr, std::memory_order_release);
        g_nvlFontInputLayout.store(nullptr, std::memory_order_release);
        g_nativeNvlGeometryShader.store(nullptr, std::memory_order_release);
        SafeRelease(nativeNvlGeometryShader_);
        SafeRelease(nvlFontInputLayout_);
        g_advNameGeometryShader.store(nullptr, std::memory_order_release);
        g_maybeFontGeometryShader.store(nullptr, std::memory_order_release);
        SafeRelease(maybeFontGeometryShader_);
        SafeRelease(advNameGeometryShader_);
        SafeRelease(nvlFontVertexShader_);
        SafeRelease(backgroundVertexShader_);
        SafeRelease(vertexShader_);
        SafeRelease(context_);
        SafeRelease(device_);
        if (panelBitmap_) DeleteObject(panelBitmap_);
        if (panelDc_) DeleteDC(panelDc_);
        panelBitmap_ = nullptr;
        panelDc_ = nullptr;
        panelPixels_ = nullptr;
    }

    bool Ensure(IDXGISwapChain* swapChain) {
        ID3D11Device* swapChainDevice = nullptr;
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device),
                                        reinterpret_cast<void**>(&swapChainDevice)))) {
            return false;
        }
        if (!device_ || swapChainDevice != device_) {
            // The selector and the selected game create separate D3D11 devices.
            // Every renderer object is device-owned, so none may be carried over.
            ReleaseAll();
            device_ = swapChainDevice;
            swapChainDevice = nullptr;
            device_->GetImmediateContext(&context_);
            if (!CreatePermanentResources()) return false;
            Log("Overlay renderer rebound to D3D11 device %p", device_);
        }
        SafeRelease(swapChainDevice);

        const bool swapChainChanged = activeSwapChain_ != swapChain;
        if (swapChainChanged) {
            // A second swap chain can have the same dimensions and device while
            // still using a different back buffer.
            ReleaseTargets();
        }
        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        if (!backBufferView_ || desc.Width != width_ || desc.Height != height_) {
            ReleaseTargets();
            width_ = desc.Width;
            height_ = desc.Height;
            if (FAILED(device_->CreateRenderTargetView(backBuffer, nullptr, &backBufferView_))) {
                backBuffer->Release();
                return false;
            }
        }
        backBuffer->Release();
        activeSwapChain_ = swapChain;
        if (swapChainChanged) Log("Overlay renderer rebound to swap chain %p", swapChain);
        return backBufferView_ && width_ && height_;
    }

    void Render(IDXGISwapChain* swapChain) {
        if (!Ensure(swapChain)) return;

        // Sample the state directly as well as through the draw hooks. This
        // guarantees useful RT/viewport diagnostics even when a game leaves no
        // tracked setter call between hook installation and its first Present.
        ID3D11RenderTargetView* currentTarget = nullptr;
        context_->OMGetRenderTargets(1, &currentTarget, nullptr);
        UpdateRenderTargetDimensions(currentTarget);
        SafeRelease(currentTarget);
        UINT viewportCount = 1;
        D3D11_VIEWPORT currentViewport{};
        context_->RSGetViewports(&viewportCount, &currentViewport);
        if (viewportCount) {
            g_viewportX.store(currentViewport.TopLeftX);
            g_viewportY.store(currentViewport.TopLeftY);
            g_viewportWidth.store(currentViewport.Width);
            g_viewportHeight.store(currentViewport.Height);
        }
        ID3D11ShaderResourceView* currentTexture = nullptr;
        context_->PSGetShaderResources(0, 1, &currentTexture);
        if (currentTexture) {
            UpdateTextureDimensions(currentTexture);
            currentTexture->Release();
        }

        PipelineBackup backup(context_);
        g_internalDraw = true;

        if (StoryPillarboxActive() && !g_spritesOverPillarbox.load()) {
            RenderPillarboxForeground();
        }
        if (g_overlayVisible.load()) {
            RenderPanel();
        }

        backup.Restore(context_);
        g_internalDraw = false;
    }

private:
    struct Transform {
        float scale[2];
        float offset[2];
    };

    bool CompileShader(const char* source, const char* entry, const char* target, ID3DBlob** output) {
        using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                             ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                             ID3DBlob**, ID3DBlob**);
        HMODULE compiler = GetModuleHandleW(L"d3dcompiler_47.dll");
        if (!compiler) compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        const auto compile = compiler
            ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile) return false;
        ID3DBlob* errors = nullptr;
        const HRESULT result = compile(source, strlen(source), "MyMerryMayFix", nullptr, nullptr,
                                       entry, target, 0, 0, output, &errors);
        if (errors) {
            if (FAILED(result)) Log("Shader compilation failed: %s", static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        return SUCCEEDED(result);
    }

    bool CreatePermanentResources() {
        static constexpr char shaderSource[] = R"(
cbuffer TransformBuffer : register(b0) { float2 scale; float2 offset; };
cbuffer TextboxStyleBuffer : register(b7) { float3 textboxTint; float textboxOpacity; };
struct Output { float4 position : SV_Position; float4 color : COLOR0; float2 uv : TEXCOORD0; };
Output VSMain(uint id : SV_VertexID) {
    float2 positions[6] = {
        float2(-1,-1), float2(-1, 1), float2( 1,-1),
        float2( 1,-1), float2(-1, 1), float2( 1, 1)
    };
    Output o;
    float2 p = positions[id];
    o.position = float4(p * scale + offset, 0, 1);
    o.color = float4(1, 1, 1, 1);
    o.uv = float2((p.x + 1) * 0.5, (1 - p.y) * 0.5);
    return o;
}
Output BackgroundVS(uint id : SV_VertexID) {
    float2 positions[4] = {
        float2(-1, 1), float2( 1, 1),
        float2(-1,-1), float2( 1,-1)
    };
    float2 texcoords[4] = {
        float2(0,0), float2(1,0),
        float2(0,1), float2(1,1)
    };
    uint corner = id & 3;
    Output o;
    o.position = float4(positions[corner], 0, 1);
    o.color = float4(1, 1, 1, 1);
    o.uv = texcoords[corner];
    return o;
}
struct FontInput {
    float3 position : POSITION0;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};
Output NvlFontVS(FontInput input) {
    Output o;
    // Scale positions and glyph widths together. Keeping their proportions
    // prevents neighbouring Japanese glyphs from consuming each other's gap.
    // The small left offset balances the observed 4:3 margins (about 35 px
    // per side instead of roughly 50 px left and 20 px right at 1920x1080).
    float x = input.position.x * 0.85 - 0.016667;
    o.position = float4(x, input.position.y, input.position.z, 1);
    o.color = input.color;
    o.uv = input.uv;
    return o;
}
[maxvertexcount(3)]
void NativeNvlGS(triangle Output input[3], inout TriangleStream<Output> stream) {
    [unroll] for (uint i = 0; i < 3; ++i) {
        Output o = input[i];
        o.position.x = o.position.x * 0.85 - 0.016667 * o.position.w;
        stream.Append(o);
    }
}
[maxvertexcount(3)]
void AdvNameGS(triangle Output input[3], inout TriangleStream<Output> stream) {
    float centreY = (input[0].position.y + input[1].position.y + input[2].position.y) / 3.0;
    float shift = 0.0;
    // Only touch the lower message area. This keeps chapter headings and other
    // upper-screen text at the engine's native position.
    if (centreY > -0.90 && centreY < -0.35) {
        shift = -0.016667;
        if (centreY > -0.52) shift += 0.10;
    }
    [unroll] for (uint i = 0; i < 3; ++i) {
        Output o = input[i];
        o.position.x += shift;
        stream.Append(o);
    }
}
[maxvertexcount(3)]
void MaybeFontGS(triangle Output input[3], inout TriangleStream<Output> stream) {
    float centreY = (input[0].position.y + input[1].position.y + input[2].position.y) / 3.0;
    [unroll] for (uint i = 0; i < 3; ++i) {
        Output o = input[i];
        if (centreY > -0.35) {
            // Maybe's NVL mode has no data01 marker. Use the same proportional
            // inset as May for upper-screen prose.
            o.position.x = o.position.x * 0.85 - 0.016667;
        } else if (centreY > -0.90) {
            // Preserve the already-tested Maybe ADV dialogue/name placement.
            o.position.x += -0.016667;
            if (centreY > -0.52) o.position.x += 0.10;
        }
        stream.Append(o);
    }
}
Texture2D image : register(t0);
SamplerState imageSampler : register(s0);
float4 PSMain(Output input) : SV_Target { return image.Sample(imageSampler, input.uv); }
float4 SolidPS(Output input) : SV_Target { return float4(0, 0, 0, 1); }
float4 TextboxPS(Output input) : SV_Target {
    float4 pixel = image.Sample(imageSampler, input.uv) * input.color;
    // The 1920-wide ADV panel occupies the upper-left part of the shared
    // 4096x460/464 atlas. Limit styling by source UV instead of screen-space
    // geometry: the engine changes the latter while fading between messages.
    // Other atlas elements (speaker plate, AUTO/SKIP, etc.) remain untouched.
    bool textboxPixel = input.uv.x >= 0.0 && input.uv.x <= 0.470 &&
                        input.uv.y >= 0.0 && input.uv.y <= 0.680;
    if (!textboxPixel) return pixel;
    // RGB is a target colour rather than a multiplier. Deriving intensity
    // from the source retains the panel's gradient while allowing a teal
    // source to become genuinely red, purple, etc.
    if (any(textboxTint < 0.9999)) {
        float intensity = max(pixel.r, max(pixel.g, pixel.b));
        pixel.rgb = intensity * textboxTint;
    }
    pixel.a = saturate(pixel.a * textboxOpacity);
    return pixel;
}
)";
        ID3DBlob* vs = nullptr;
        ID3DBlob* backgroundVs = nullptr;
        ID3DBlob* nvlFontVs = nullptr;
        ID3DBlob* advNameGs = nullptr;
        ID3DBlob* maybeFontGs = nullptr;
        ID3DBlob* nativeNvlGs = nullptr;
        ID3DBlob* ps = nullptr;
        ID3DBlob* solidPs = nullptr;
        ID3DBlob* textboxPs = nullptr;
        if (!CompileShader(shaderSource, "VSMain", "vs_4_0", &vs) ||
            !CompileShader(shaderSource, "BackgroundVS", "vs_4_0", &backgroundVs) ||
            !CompileShader(shaderSource, "NvlFontVS", "vs_4_0", &nvlFontVs) ||
            !CompileShader(shaderSource, "AdvNameGS", "gs_4_0", &advNameGs) ||
            !CompileShader(shaderSource, "MaybeFontGS", "gs_4_0", &maybeFontGs) ||
            !CompileShader(shaderSource, "PSMain", "ps_4_0", &ps) ||
            !CompileShader(shaderSource, "SolidPS", "ps_4_0", &solidPs) ||
            !CompileShader(shaderSource, "TextboxPS", "ps_4_0", &textboxPs)) {
            SafeRelease(vs);
            SafeRelease(backgroundVs);
            SafeRelease(nvlFontVs);
            SafeRelease(advNameGs);
            SafeRelease(maybeFontGs);
            SafeRelease(ps);
            SafeRelease(solidPs);
            SafeRelease(textboxPs);
            return false;
        }
        HRESULT result = device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertexShader_);
        if (SUCCEEDED(result)) {
            result = device_->CreateVertexShader(backgroundVs->GetBufferPointer(), backgroundVs->GetBufferSize(),
                                                 nullptr, &backgroundVertexShader_);
        }
        if (SUCCEEDED(result)) {
            result = device_->CreateVertexShader(nvlFontVs->GetBufferPointer(), nvlFontVs->GetBufferSize(),
                                                 nullptr, &nvlFontVertexShader_);
        }
        if (SUCCEEDED(result) && g_nativeWindows) {
            const D3D11_INPUT_ELEMENT_DESC elements[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
            };
            result = device_->CreateInputLayout(elements, 3, nvlFontVs->GetBufferPointer(),
                                                 nvlFontVs->GetBufferSize(), &nvlFontInputLayout_);
            Log("Native NVL input layout created: result=0x%08lx", static_cast<unsigned long>(result));
        }
        if (SUCCEEDED(result)) {
            result = device_->CreateGeometryShader(advNameGs->GetBufferPointer(), advNameGs->GetBufferSize(),
                                                   nullptr, &advNameGeometryShader_);
        }
        if (SUCCEEDED(result)) {
            result = device_->CreateGeometryShader(maybeFontGs->GetBufferPointer(), maybeFontGs->GetBufferSize(),
                                                   nullptr, &maybeFontGeometryShader_);
        }
        if (SUCCEEDED(result) && g_nativeWindows) {
            if (!CompileShader(shaderSource, "NativeNvlGS", "gs_4_0", &nativeNvlGs)) result = E_FAIL;
            else result = device_->CreateGeometryShader(nativeNvlGs->GetBufferPointer(), nativeNvlGs->GetBufferSize(),
                                                        nullptr, &nativeNvlGeometryShader_);
            SafeRelease(nativeNvlGs);
        }
        if (SUCCEEDED(result)) result = device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixelShader_);
        if (SUCCEEDED(result)) result = device_->CreatePixelShader(solidPs->GetBufferPointer(), solidPs->GetBufferSize(), nullptr, &solidPixelShader_);
        if (SUCCEEDED(result)) result = device_->CreatePixelShader(textboxPs->GetBufferPointer(), textboxPs->GetBufferSize(), nullptr, &textboxPixelShader_);
        SafeRelease(vs);
        SafeRelease(backgroundVs);
        SafeRelease(nvlFontVs);
        SafeRelease(advNameGs);
        SafeRelease(maybeFontGs);
        SafeRelease(ps);
        SafeRelease(solidPs);
        SafeRelease(textboxPs);
        if (FAILED(result)) return false;
        g_backgroundVertexShader.store(backgroundVertexShader_, std::memory_order_release);
        g_nvlFontVertexShader.store(nvlFontVertexShader_, std::memory_order_release);
        g_nvlFontInputLayout.store(nvlFontInputLayout_, std::memory_order_release);
        g_nativeNvlGeometryShader.store(nativeNvlGeometryShader_, std::memory_order_release);
        g_advNameGeometryShader.store(advNameGeometryShader_, std::memory_order_release);
        g_maybeFontGeometryShader.store(maybeFontGeometryShader_, std::memory_order_release);
        g_textboxPixelShader.store(textboxPixelShader_, std::memory_order_release);

        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = sizeof(Transform);
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&bufferDesc, nullptr, &constantBuffer_))) return false;

        D3D11_BUFFER_DESC textboxBufferDesc{};
        textboxBufferDesc.ByteWidth = 16;
        textboxBufferDesc.Usage = D3D11_USAGE_DEFAULT;
        textboxBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device_->CreateBuffer(&textboxBufferDesc, nullptr, &textboxStyleBuffer_))) return false;
        g_textboxStyleBuffer.store(textboxStyleBuffer_, std::memory_order_release);

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&samplerDesc, &sampler_))) return false;

        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&blendDesc, &blendState_))) return false;

        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        depthDesc.DepthEnable = FALSE;
        depthDesc.StencilEnable = FALSE;
        if (FAILED(device_->CreateDepthStencilState(&depthDesc, &depthState_))) return false;

        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        if (FAILED(device_->CreateRasterizerState(&rasterDesc, &rasterizerState_))) return false;
        return CreatePanelResources();
    }

    bool CreatePanelResources() {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = panelWidth_;
        info.bmiHeader.biHeight = -panelHeight_;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        panelDc_ = CreateCompatibleDC(nullptr);
        panelBitmap_ = CreateDIBSection(panelDc_, &info, DIB_RGB_COLORS, &panelPixels_, nullptr, 0);
        if (!panelDc_ || !panelBitmap_) return false;
        SelectObject(panelDc_, panelBitmap_);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = panelWidth_;
        desc.Height = panelHeight_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &panelTexture_))) return false;
        return SUCCEEDED(device_->CreateShaderResourceView(panelTexture_, nullptr, &panelView_));
    }

    void SetTransform(float scaleX, float scaleY, float offsetX, float offsetY) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(constantBuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* transform = static_cast<Transform*>(mapped.pData);
            transform->scale[0] = scaleX;
            transform->scale[1] = scaleY;
            transform->offset[0] = offsetX;
            transform->offset[1] = offsetY;
            context_->Unmap(constantBuffer_, 0);
        }
    }

    void DrawTexture(ID3D11ShaderResourceView* view, ID3D11BlendState* blend) {
        constexpr float factor[4]{0, 0, 0, 0};
        context_->OMSetRenderTargets(1, &backBufferView_, nullptr);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_, nullptr, 0);
        context_->PSSetShader(pixelShader_, nullptr, 0);
        context_->VSSetConstantBuffers(0, 1, &constantBuffer_);
        context_->PSSetShaderResources(0, 1, &view);
        context_->PSSetSamplers(0, 1, &sampler_);
        context_->OMSetBlendState(blend, factor, 0xffffffffu);
        context_->OMSetDepthStencilState(depthState_, 0);
        context_->RSSetState(rasterizerState_);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
        context_->RSSetViewports(1, &viewport);
        context_->Draw(6, 0);
        ID3D11ShaderResourceView* nullView = nullptr;
        context_->PSSetShaderResources(0, 1, &nullView);
    }

    void DrawSolidRect(float scaleX, float scaleY, float offsetX, float offsetY) {
        SetTransform(scaleX, scaleY, offsetX, offsetY);
        constexpr float factor[4]{0, 0, 0, 0};
        context_->OMSetRenderTargets(1, &backBufferView_, nullptr);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_, nullptr, 0);
        context_->PSSetShader(solidPixelShader_, nullptr, 0);
        context_->VSSetConstantBuffers(0, 1, &constantBuffer_);
        ID3D11ShaderResourceView* nullView = nullptr;
        context_->PSSetShaderResources(0, 1, &nullView);
        context_->OMSetBlendState(nullptr, factor, 0xffffffffu);
        context_->OMSetDepthStencilState(depthState_, 0);
        context_->RSSetState(rasterizerState_);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
        context_->RSSetViewports(1, &viewport);
        context_->Draw(6, 0);
    }

    void RenderPillarboxForeground() {
        if (!width_ || !height_) return;
        const float contentWidth = static_cast<float>(height_) * (4.0f / 3.0f);
        const float barWidth = (static_cast<float>(width_) - contentWidth) * 0.5f;
        if (barWidth <= 0.5f) return;
        const float scaleX = barWidth / static_cast<float>(width_);
        DrawSolidRect(scaleX, 1.0f, -1.0f + scaleX, 0.0f);
        DrawSolidRect(scaleX, 1.0f,  1.0f - scaleX, 0.0f);
    }

    void DrawPanelText() {
        RECT all{0, 0, panelWidth_, panelHeight_};
        HBRUSH background = CreateSolidBrush(RGB(20, 24, 31));
        FillRect(panelDc_, &all, background);
        DeleteObject(background);
        SetBkMode(panelDc_, TRANSPARENT);
        SetTextColor(panelDc_, RGB(235, 240, 247));
        HFONT titleFont = CreateFontW(25, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT bodyFont = CreateFontW(19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(panelDc_, titleFont);
        TextOutW(panelDc_, 24, 18, L"My Merry May Fix 1.0.0", 22);
        SelectObject(panelDc_, bodyFont);

        wchar_t line[256]{};
        const LONG selected = g_overlaySelection.load();
        auto drawSlider = [&](int y, LONG value, LONG maximum, COLORREF color) {
            RECT track{430, y + 5, 625, y + 17};
            HBRUSH trackBrush = CreateSolidBrush(RGB(48, 57, 69));
            FillRect(panelDc_, &track, trackBrush);
            DeleteObject(trackBrush);
            RECT fill = track;
            fill.right = fill.left + static_cast<LONG>(
                (fill.right - fill.left) * std::clamp(value, 0L, maximum) / maximum);
            HBRUSH fillBrush = CreateSolidBrush(color);
            FillRect(panelDc_, &fill, fillBrush);
            DeleteObject(fillBrush);
            HBRUSH border = CreateSolidBrush(RGB(112, 126, 143));
            FrameRect(panelDc_, &track, border);
            DeleteObject(border);
        };
        swprintf(line, 256, L"%lc Aspect correction:  %ls", selected == 0 ? L'>' : L' ', AspectModeName());
        SetTextColor(panelDc_, selected == 0 ? RGB(102, 224, 220) : RGB(235, 240, 247));
        TextOutW(panelDc_, 24, 63, line, static_cast<int>(wcslen(line)));
        swprintf(line, 256, L"%lc Sprites over bars: %ls", selected == 1 ? L'>' : L' ',
                 g_spritesOverPillarbox.load() ? L"On" : L"Off");
        SetTextColor(panelDc_, selected == 1 ? RGB(102, 224, 220) : RGB(235, 240, 247));
        TextOutW(panelDc_, 24, 94, line, static_cast<int>(wcslen(line)));

        swprintf(line, 256, L"%lc Textbox opacity:  %ld%% %ls", selected == 2 ? L'>' : L' ',
                 g_textboxOpacity.load(), g_textboxOpacity.load() == 100 ? L"(original)" : L"");
        SetTextColor(panelDc_, selected == 2 ? RGB(102, 224, 220) : RGB(235, 240, 247));
        TextOutW(panelDc_, 24, 125, line, static_cast<int>(wcslen(line)));
        drawSlider(125, g_textboxOpacity.load(), 200, RGB(58, 201, 196));

        swprintf(line, 256, L"%lc Textbox color R:  %ld", selected == 3 ? L'>' : L' ', g_textboxColorR.load());
        SetTextColor(panelDc_, selected == 3 ? RGB(102, 224, 220) : RGB(235, 240, 247));
        TextOutW(panelDc_, 24, 156, line, static_cast<int>(wcslen(line)));
        drawSlider(156, g_textboxColorR.load(), 255, RGB(220, 74, 74));

        swprintf(line, 256, L"%lc Textbox color G:  %ld", selected == 4 ? L'>' : L' ', g_textboxColorG.load());
        SetTextColor(panelDc_, selected == 4 ? RGB(102, 224, 220) : RGB(235, 240, 247));
        TextOutW(panelDc_, 24, 187, line, static_cast<int>(wcslen(line)));
        drawSlider(187, g_textboxColorG.load(), 255, RGB(65, 205, 105));

        swprintf(line, 256, L"%lc Textbox color B:  %ld", selected == 5 ? L'>' : L' ', g_textboxColorB.load());
        SetTextColor(panelDc_, selected == 5 ? RGB(102, 224, 220) : RGB(235, 240, 247));
        TextOutW(panelDc_, 24, 218, line, static_cast<int>(wcslen(line)));
        drawSlider(218, g_textboxColorB.load(), 255, RGB(72, 125, 230));

        swprintf(line, 256, L"%lc Voiced text uses normal speed: %ls", selected == 6 ? L'>' : L' ',
                 g_matchVoicedTextSpeed.load() ? L"On" : L"Off");
        SetTextColor(panelDc_, selected == 6 ? RGB(102, 224, 220) : RGB(235, 240, 247));
        TextOutW(panelDc_, 24, 249, line, static_cast<int>(wcslen(line)));

        swprintf(line, 256, L"%lc Keep voice until next voiced line: %ls", selected == 7 ? L'>' : L' ',
                 g_continueVoiceUntilNext.load() ? L"On" : L"Off");
        SetTextColor(panelDc_, selected == 7 ? RGB(102, 224, 220) : RGB(235, 240, 247));
        TextOutW(panelDc_, 24, 280, line, static_cast<int>(wcslen(line)));

        SetTextColor(panelDc_, RGB(235, 240, 247));
        TextOutW(panelDc_, 24, 311, L"  Pillarbox color:   Black", 26);
        const std::wstring hotkey = HotkeyName(g_hotkey.load());
        swprintf(line, 256, L"  Overlay hotkey:    %ls", hotkey.c_str());
        TextOutW(panelDc_, 24, 342, line, static_cast<int>(wcslen(line)));

        SetTextColor(panelDc_, RGB(170, 181, 194));
        if (!g_highResolution.load()) {
            swprintf(line, 256, L"  High-resolution rendering: Off");
        } else if (g_nativeRenderWidth.load() && g_nativeRenderHeight.load()) {
            swprintf(line, 256, L"  High-resolution rendering: On (%u x %u)",
                     g_nativeRenderWidth.load(), g_nativeRenderHeight.load());
        } else {
            swprintf(line, 256, L"  High-resolution rendering: On (inactive at this resolution)");
        }
        TextOutW(panelDc_, 24, 384, line, static_cast<int>(wcslen(line)));

        SetTextColor(panelDc_, RGB(130, 142, 157));
        constexpr wchar_t controls[] = L"Up/Down: select  Left/Right: change  Esc: close";
        TextOutW(panelDc_, 24, 428, controls, static_cast<int>(wcslen(controls)));
        SelectObject(panelDc_, oldFont);
        DeleteObject(titleFont);
        DeleteObject(bodyFont);

        auto* pixels = static_cast<unsigned char*>(panelPixels_);
        for (int i = 0; i < panelWidth_ * panelHeight_; ++i) pixels[i * 4 + 3] = 238;
    }

    void RenderPanel() {
        DrawPanelText();
        context_->UpdateSubresource(panelTexture_, 0, nullptr, panelPixels_, panelWidth_ * 4, 0);
        const float scaleX = std::min(1.0f, static_cast<float>(panelWidth_) / width_);
        const float scaleY = std::min(1.0f, static_cast<float>(panelHeight_) / height_);
        SetTransform(scaleX, scaleY, -1.0f + scaleX + 0.02f, 1.0f - scaleY - 0.035f);
        DrawTexture(panelView_, blendState_);
    }

    static constexpr int panelWidth_ = 660;
    static constexpr int panelHeight_ = 469;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11VertexShader* vertexShader_ = nullptr;
    ID3D11VertexShader* backgroundVertexShader_ = nullptr;
    ID3D11VertexShader* nvlFontVertexShader_ = nullptr;
    ID3D11InputLayout* nvlFontInputLayout_ = nullptr;
    ID3D11GeometryShader* nativeNvlGeometryShader_ = nullptr;
    ID3D11GeometryShader* advNameGeometryShader_ = nullptr;
    ID3D11GeometryShader* maybeFontGeometryShader_ = nullptr;
    ID3D11PixelShader* pixelShader_ = nullptr;
    ID3D11PixelShader* solidPixelShader_ = nullptr;
    ID3D11PixelShader* textboxPixelShader_ = nullptr;
    ID3D11Buffer* constantBuffer_ = nullptr;
    ID3D11Buffer* textboxStyleBuffer_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11BlendState* blendState_ = nullptr;
    ID3D11DepthStencilState* depthState_ = nullptr;
    ID3D11RasterizerState* rasterizerState_ = nullptr;
    ID3D11RenderTargetView* backBufferView_ = nullptr;
    ID3D11Texture2D* panelTexture_ = nullptr;
    ID3D11ShaderResourceView* panelView_ = nullptr;
    HDC panelDc_ = nullptr;
    HBITMAP panelBitmap_ = nullptr;
    void* panelPixels_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    IDXGISwapChain* activeSwapChain_ = nullptr; // Identity only; Present owns its lifetime.
};

OverlayRenderer g_renderer;

bool QueryCursor(CURSORINFO& info) {
    info = {};
    info.cbSize = sizeof(info);
    return GetCursorInfo(&info) != FALSE;
}

bool IsUsableGameCursor(HCURSOR cursor) {
    return cursor && cursor != LoadCursorW(nullptr, IDC_ARROW);
}

void RememberGameCursor() {
    if (!g_restoreCursor.load() || !IsGameWindowActive()) return;
    CURSORINFO info{};
    if (QueryCursor(info) && (info.flags & CURSOR_SHOWING) &&
        IsUsableGameCursor(info.hCursor)) {
        g_gameCursor.store(info.hCursor);
    }
}

void RestoreGameCursorAfterMovement() {
    if (!g_restoreCursor.load() || !g_cursorRestorePending ||
        !IsGameWindowActive()) return;

    CURSORINFO info{};
    if (QueryCursor(info) && (info.flags & CURSOR_SHOWING) &&
        IsUsableGameCursor(info.hCursor)) {
        // The engine restored its cursor itself while processing WM_MOUSEMOVE.
        g_gameCursor.store(info.hCursor);
        g_cursorRestorePending.store(false);
        return;
    }
    const HCURSOR gameCursor = g_gameCursor.load();
    if (!IsUsableGameCursor(gameCursor)) return;

    // Restore the exact custom cursor previously observed from the game.  Do
    // not ask the window procedure to choose a cursor: under Wine that path
    // selects IDC_ARROW and produces the blinking generic cursor.
    SetCursor(gameCursor);
    if (!QueryCursor(info) || !(info.flags & CURSOR_SHOWING)) {
        // ShowCursor is a counter. Raise it only as far as required for the
        // cached game cursor to become visible, avoiding a permanent offset.
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            ShowCursor(TRUE);
            if (QueryCursor(info) && (info.flags & CURSOR_SHOWING)) break;
        }
        SetCursor(gameCursor);
    }
    if (QueryCursor(info) && (info.flags & CURSOR_SHOWING) &&
        info.hCursor == gameCursor) {
        g_cursorRestorePending.store(false);
        Log("Custom game cursor restored after mouse movement");
    }
}

LRESULT CALLBACK HookWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_KILLFOCUS ||
        (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) ||
        (message == WM_ACTIVATEAPP && !wParam)) {
        RememberGameCursor();
        g_cursorRestorePending.store(true);
        g_forceCursorVisible.store(false);
    } else if (!g_overlayVisible.load() &&
               (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)) {
        // The engine intentionally hides its cursor when keyboard navigation
        // begins. Keep it hidden until the user moves the mouse again.
        RememberGameCursor();
        g_cursorRestorePending.store(true);
        g_forceCursorVisible.store(false);
    }
    if (message == WM_KEYDOWN) {
        const bool repeated = (lParam & (1u << 30)) != 0;
        if (wParam == g_hotkey.load()) {
            if (repeated) return 0;
            const bool visible = !g_overlayVisible.load();
            g_overlayVisible.store(visible);
            if (visible) {
                g_candidateLogCount.store(0);
                g_uiLogCount.store(0);
                g_geometryLogMask.store(0);
            }
            return 0;
        }
        if (g_overlayVisible.load()) {
            if (wParam == VK_ESCAPE) {
                g_overlayVisible.store(false);
                return 0;
            }
            if (!repeated && (wParam == VK_UP || wParam == VK_DOWN)) {
                LONG selection = g_overlaySelection.load();
                selection += (wParam == VK_DOWN) ? 1 : -1;
                if (selection < 0) selection = 7;
                if (selection > 7) selection = 0;
                g_overlaySelection.store(selection);
                return 0;
            }
            if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                const LONG selection = g_overlaySelection.load();
                // Aspect mode and sprite priority are discrete toggles. Key
                // repeat is useful only for the four numeric sliders.
                if (repeated && (selection < 2 || selection >= 6)) return 0;
                const LONG direction = wParam == VK_RIGHT ? 1 : -1;
                if (selection == 0) {
                    LONG mode = g_aspectMode.load();
                    mode += direction;
                    if (mode < 0) mode = 1;
                    if (mode > 1) mode = 0;
                    g_aspectMode.store(mode);
                    if (mode == static_cast<LONG>(AspectMode::Off))
                        g_storyPillarboxActive.store(false);
                    SaveAspectMode();
                    Log("Aspect mode changed to %ld", mode);
                } else if (selection == 1) {
                    const bool enabled = !g_spritesOverPillarbox.load();
                    g_spritesOverPillarbox.store(enabled);
                    SaveSpritesOverPillarbox();
                    Log("Sprites over pillarbox changed to %d", enabled);
                } else if (selection == 6) {
                    const bool enabled = !g_matchVoicedTextSpeed.load();
                    g_matchVoicedTextSpeed.store(enabled);
                    SaveVoiceTextSpeed();
                    Log("Voiced text normal-speed matching changed to %d", enabled);
                } else if (selection == 7) {
                    const bool enabled = !g_continueVoiceUntilNext.load();
                    g_continueVoiceUntilNext.store(enabled);
                    SaveVoiceContinuation();
                    Log("Voice continuation until next voiced line changed to %d", enabled);
                } else {
                    std::atomic<LONG>* value = nullptr;
                    LONG maximum = 255;
                    if (selection == 2) {
                        value = &g_textboxOpacity;
                        maximum = 200;
                    } else if (selection == 3) {
                        value = &g_textboxColorR;
                    } else if (selection == 4) {
                        value = &g_textboxColorG;
                    } else if (selection == 5) {
                        value = &g_textboxColorB;
                    }
                    if (value) {
                        value->store(std::clamp(value->load() + direction * 5, 0L, maximum));
                        SaveTextboxStyle();
                        Log("Textbox style changed: opacity=%ld rgb=%ld,%ld,%ld",
                            g_textboxOpacity.load(), g_textboxColorR.load(),
                            g_textboxColorG.load(), g_textboxColorB.load());
                    }
                }
                g_candidateLogCount.store(0);
                g_uiLogCount.store(0);
                g_geometryLogMask.store(0);
                DiagnoseContexts();
                return 0;
            }
        }
    }
    if (g_overlayVisible.load()) {
        switch (message) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;
        case WM_MOUSEMOVE:
            ReacquireMouseDevices();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            return 0;
        default:
            break;
        }
    }
    if (message == WM_MOUSEMOVE && g_restoreCursor.load() &&
        g_cursorRestorePending.load() && IsGameWindowActive()) {
        // From this movement until the first real click, keep the visible game
        // cursor from being cleared again by the engine's keyboard-mode state.
        g_forceCursorVisible.store(true);
        RequestMouseWake();
    } else if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
               message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN) {
        g_forceCursorVisible.store(false);
        g_cursorRestorePending.store(false);
    }
    const LRESULT result = g_originalWndProc
        ? CallWindowProcW(g_originalWndProc, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_MOUSEMOVE) {
        ReacquireMouseDevices();
        RestoreGameCursorAfterMovement();
        if (!g_cursorRestorePending.load()) RememberGameCursor();
    } else if (message == WM_SETFOCUS ||
               (message == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE) ||
               (message == WM_ACTIVATEAPP && wParam)) {
        ReacquireMouseDevices();
    }
    return result;
}

void HookWindow(HWND window) {
    if (!window) return;
    const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
    if (window == g_window && current == HookWindowProc) return;

    // The selector and game reuse the HWND, but the game installs a new window
    // procedure during handoff. Treat that as a new hook even when HWND matches.
    g_window = window;
    SetLastError(0);
    auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC,
                                                               reinterpret_cast<LONG_PTR>(HookWindowProc)));
    if (previous || GetLastError() == 0) {
        g_originalWndProc = previous;
        Log("Window procedure hooked");
    }
}

void HookContext(ID3D11DeviceContext* context) {
    if (!context) return;
    void** vtable = *reinterpret_cast<void***>(context);
    ContextHooks* entry = nullptr;
    for (auto& existing : g_contextHooks) {
        if (existing.vtable.load(std::memory_order_acquire) == vtable) {
            entry = &existing;
            break;
        }
    }
    const bool newlyTracked = !entry;
    if (!entry) for (auto& candidate : g_contextHooks) {
        void** expected = nullptr;
        if (candidate.vtable.compare_exchange_strong(expected, vtable, std::memory_order_acq_rel)) {
            entry = &candidate;
            break;
        }
    }
    if (!entry) {
        Log("Unable to hook D3D11 context %p: context hook table is full", context);
        return;
    }

    bool changed = false;
    auto refresh = [&](size_t slot, auto replacement, auto& original) {
        if (vtable[slot] == reinterpret_cast<void*>(replacement)) return true;
        // Forward to the newly selected runtime dispatch function, rather than
        // the stale function saved at context creation.
        auto previous = original;
        original = nullptr;
        const bool result = HookVtable(context, slot, replacement, original);
        if (!result) original = previous;
        changed = true;
        return result;
    };
    const bool ps = refresh(8, HookPSSetShaderResources, entry->psSetShaderResources);
    const bool indexed = refresh(12, HookDrawIndexed, entry->drawIndexed);
    const bool draw = refresh(13, HookDraw, entry->draw);
    const bool map = refresh(14, HookMap, entry->map);
    const bool unmap = refresh(15, HookUnmap, entry->unmap);
    const bool targets = refresh(33, HookOMSetRenderTargets, entry->omSetRenderTargets);
    const bool viewports = refresh(44, HookRSSetViewports, entry->rsSetViewports);
    const bool scissors = refresh(45, HookRSSetScissorRects, entry->rsSetScissorRects);
    if (g_nativeWindows) refresh(114, HookFinishCommandList, entry->finishCommandList);
    if (g_nativeWindows) {
        refresh(11, HookVSSetShader, entry->vsSetShader);
        refresh(17, HookIASetInputLayout, entry->iaSetInputLayout);
        refresh(18, HookIASetVertexBuffers, entry->iaSetVertexBuffers);
        refresh(23, HookGSSetShader, entry->gsSetShader);
        refresh(24, HookIASetPrimitiveTopology, entry->iaSetPrimitiveTopology);
    }
    if (!newlyTracked) {
        static std::atomic<unsigned> refreshLogs{0};
        if (changed && refreshLogs.fetch_add(1) < 32) {
            Log("Native context dispatch refreshed: context=%p PSSetSRV=%d DrawIndexed=%d Draw=%d Map=%d Unmap=%d OMSetRT=%d RSSetVP=%d RSSetScissor=%d",
                context, ps, indexed, draw, map, unmap, targets, viewports, scissors);
        }
        return;
    }
    Log("D3D11 context hooked: context=%p vtable=%p PSSetSRV=%d DrawIndexed=%d Draw=%d Map=%d Unmap=%d OMSetRT=%d RSSetVP=%d RSSetScissor=%d",
        context, vtable, ps, indexed, draw, map, unmap, targets, viewports, scissors);
    ID3D11Device* owner = nullptr;
    context->GetDevice(&owner);
    Log("Diagnostic context owner: context=%p device=%p type=%u (0=immediate,1=deferred)",
        context, owner, static_cast<unsigned>(context->GetType()));
    SafeRelease(owner);
}

HRESULT STDMETHODCALLTYPE HookCreateDeferredContext(ID3D11Device* device, UINT flags,
                                                    ID3D11DeviceContext** deferredContext) {
    const HRESULT result = g_originalCreateDeferredContext(device, flags, deferredContext);
    if (SUCCEEDED(result) && deferredContext && *deferredContext) {
        HookContext(*deferredContext);
        Log("CreateDeferredContext captured context %p", *deferredContext);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* device,
                                               const D3D11_TEXTURE2D_DESC* description,
                                               const D3D11_SUBRESOURCE_DATA* initialData,
                                               ID3D11Texture2D** texture) {
    if (!g_originalCreateTexture2D || !description) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC adjusted = *description;
    const UINT targetWidth = g_nativeRenderWidth.load();
    const UINT targetHeight = g_nativeRenderHeight.load();
    const bool engineTarget = g_highResolution.load() && StoryGameModuleLoaded() &&
        targetWidth > 1920 && targetHeight > 1080 && !initialData &&
        description->Width == 1920 && description->Height == 1080 &&
        description->Usage == D3D11_USAGE_DEFAULT && description->ArraySize == 1 &&
        (description->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL));
    if (engineTarget) {
        adjusted.Width = targetWidth;
        adjusted.Height = targetHeight;
    }
    const HRESULT result = g_originalCreateTexture2D(device, engineTarget ? &adjusted : description,
                                                     initialData, texture);
    if (engineTarget) {
        static std::atomic<unsigned> logs{0};
        if (logs.fetch_add(1) < 32) {
            Log("High-resolution texture: 1920x1080 -> %ux%u bind=0x%x format=%u samples=%u result=0x%08lx",
                targetWidth, targetHeight, description->BindFlags,
                static_cast<unsigned>(description->Format), description->SampleDesc.Count,
                static_cast<unsigned long>(result));
        }
    }
    return result;
}

void HookDevice(ID3D11Device* device) {
    if (!device) return;
    const bool texture = HookVtable(device, 5, HookCreateTexture2D, g_originalCreateTexture2D);
    const bool deferred = HookVtable(device, 27, HookCreateDeferredContext, g_originalCreateDeferredContext);
    Log("D3D11 device hooked: device=%p CreateTexture2D=%d deferred-context=%d", device,
        texture, deferred);
}

HRESULT WINAPI HookD3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
                                     HMODULE software, UINT flags,
                                     const D3D_FEATURE_LEVEL* featureLevels,
                                     UINT featureLevelCount, UINT sdkVersion,
                                     ID3D11Device** device,
                                     D3D_FEATURE_LEVEL* selectedFeatureLevel,
                                     ID3D11DeviceContext** immediateContext) {
    const HRESULT result = g_originalD3D11CreateDevice(
        adapter, driverType, software, flags, featureLevels, featureLevelCount,
        sdkVersion, device, selectedFeatureLevel, immediateContext);
    if (SUCCEEDED(result) && immediateContext && *immediateContext) {
        if (device && *device) HookDevice(*device);
        HookContext(*immediateContext);
        Log("D3D11CreateDevice captured immediate context %p", *immediateContext);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT interval, UINT flags) {
    static ULONGLONG lastDiagnostic = 0;
    const ULONGLONG now = GetTickCount64();
    const bool diagnose = g_logging.load() && now - lastDiagnostic >= 5000;
    if (diagnose) lastDiagnostic = now;
    DXGI_SWAP_CHAIN_DESC swapChainDescription{};
    if (SUCCEEDED(swapChain->GetDesc(&swapChainDescription))) {
        // Some game modules replace GWLP_WNDPROC after creating the swap chain.
        // Revalidate at Present so the overlay hotkey survives that handoff.
        HookWindow(swapChainDescription.OutputWindow);
    }
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        HookContext(context);
        if (diagnose) {
            void** deviceTable = *reinterpret_cast<void***>(device);
            void** contextTable = *reinterpret_cast<void***>(context);
            Log("Diagnostic Present: swapchain=%p device=%p immediate=%p deferred-create-hook=%d PSSetSRV-hook=%d Draw-hook=%d DrawIndexed-hook=%d",
                swapChain, device, context,
                deviceTable[27] == reinterpret_cast<void*>(HookCreateDeferredContext),
                contextTable[8] == reinterpret_cast<void*>(HookPSSetShaderResources),
                contextTable[13] == reinterpret_cast<void*>(HookDraw),
                contextTable[12] == reinterpret_cast<void*>(HookDrawIndexed));
            DiagnoseContexts();
        }
        SafeRelease(context);
        SafeRelease(device);
    }
    g_renderer.Render(swapChain);
    g_backgroundCleared.store(false);
    const HRESULT result = g_originalPresent(swapChain, interval, flags);
    g_lastBackgroundAdjusted.store(false);
    g_lastUiChromeAdjusted.store(false);
    g_lastTextAdjusted.store(false);
    g_nvlUiSeen.store(false);
    g_messageUiSeen.store(false);
    g_systemUiSeen.store(false);
    g_frameBackgroundVertexShader.store(nullptr);
    return result;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* swapChain, UINT count, UINT width,
                                            UINT height, DXGI_FORMAT format, UINT flags) {
    g_renderer.ReleaseTargets();
    const HRESULT result = g_originalResizeBuffers(swapChain, count, width, height, format, flags);
    if (SUCCEEDED(result)) {
        DXGI_SWAP_CHAIN_DESC desc{};
        ID3D11Texture2D* backBuffer = nullptr;
        D3D11_TEXTURE2D_DESC backBufferDesc{};
        if (SUCCEEDED(swapChain->GetDesc(&desc)) &&
            SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                           reinterpret_cast<void**>(&backBuffer)))) {
            backBuffer->GetDesc(&backBufferDesc);
            RECT client{};
            GetClientRect(desc.OutputWindow, &client);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(desc.OutputWindow, MONITOR_DEFAULTTONEAREST), &monitor);
            Log("DXGI resized: request=%ux%u desc=%ux%u backbuffer=%ux%u client=%ldx%ld monitor=%ldx%ld windowed=%d",
                width, height, desc.BufferDesc.Width, desc.BufferDesc.Height,
                backBufferDesc.Width, backBufferDesc.Height,
                client.right - client.left, client.bottom - client.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left,
                monitor.rcMonitor.bottom - monitor.rcMonitor.top, desc.Windowed);
        }
        SafeRelease(backBuffer);
    }
    return result;
}

void LogSwapChainResolution(IDXGISwapChain* swapChain, const char* event) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return;
    ID3D11Texture2D* backBuffer = nullptr;
    D3D11_TEXTURE2D_DESC backBufferDesc{};
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                      reinterpret_cast<void**>(&backBuffer)))) {
        backBuffer->GetDesc(&backBufferDesc);
    }
    RECT client{};
    GetClientRect(desc.OutputWindow, &client);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(desc.OutputWindow, MONITOR_DEFAULTTONEAREST), &monitor);
    Log("DXGI resolution %s: desc=%ux%u backbuffer=%ux%u client=%ldx%ld monitor=%ldx%ld windowed=%d scaling=%u",
        event, desc.BufferDesc.Width, desc.BufferDesc.Height,
        backBufferDesc.Width, backBufferDesc.Height,
        client.right - client.left, client.bottom - client.top,
        monitor.rcMonitor.right - monitor.rcMonitor.left,
        monitor.rcMonitor.bottom - monitor.rcMonitor.top,
        desc.Windowed, static_cast<unsigned>(desc.BufferDesc.Scaling));
    SafeRelease(backBuffer);
}

void HookSwapChain(IDXGISwapChain* swapChain) {
    g_storyPillarboxActive.store(false);
    HookVtable(swapChain, 8, HookPresent, g_originalPresent);
    HookVtable(swapChain, 13, HookResizeBuffers, g_originalResizeBuffers);
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(swapChain->GetDesc(&desc))) HookWindow(desc.OutputWindow);
    LogSwapChainResolution(swapChain, "hooked");
    Log("DXGI swap chain hooked");
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* factory, IUnknown* device,
                                              DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) {
    if (desc) UpdateNativeRenderResolution(desc->OutputWindow);
    const HRESULT result = g_originalCreateSwapChain(factory, device, desc, swapChain);
    if (SUCCEEDED(result) && swapChain && *swapChain) HookSwapChain(*swapChain);
    return result;
}

void HookFactory(IDXGIFactory* factory) {
    if (HookVtable(factory, 10, HookCreateSwapChain, g_originalCreateSwapChain)) {
        Log("DXGI factory hooked");
    }
}

HRESULT WINAPI HookCreateDXGIFactory1(REFIID interfaceId, void** factory) {
    const HRESULT result = g_originalCreateDXGIFactory1(interfaceId, factory);
    if (SUCCEEDED(result) && factory && *factory) HookFactory(static_cast<IDXGIFactory*>(*factory));
    return result;
}

void PatchModule(HMODULE module);

void PatchLoadedGameModule(HMODULE module, DWORD flags = 0) {
    if (!module || module == g_self || (flags & (LOAD_LIBRARY_AS_DATAFILE |
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE |
        DONT_RESOLVE_DLL_REFERENCES))) return;
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    if (_wcsicmp(name, L"MyMerryMay_WIN_Master.dll") == 0 ||
        _wcsicmp(name, L"MyMerryMaybe_WIN_Master.dll") == 0 ||
        _wcsicmp(name, L"MyMerryMaySel_WIN_Master.dll") == 0) {
        PatchModule(module);
        Log("Windows loader patched game module before returning: %ls", name);
    }
}

HMODULE WINAPI HookLoadLibraryW(LPCWSTR name) {
    HMODULE module = LoadLibraryW(name);
    PatchLoadedGameModule(module);
    return module;
}
HMODULE WINAPI HookLoadLibraryA(LPCSTR name) {
    HMODULE module = LoadLibraryA(name);
    PatchLoadedGameModule(module);
    return module;
}
HMODULE WINAPI HookLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE module = LoadLibraryExW(name, file, flags);
    PatchLoadedGameModule(module, flags);
    return module;
}
HMODULE WINAPI HookLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    HMODULE module = LoadLibraryExA(name, file, flags);
    PatchLoadedGameModule(module, flags);
    return module;
}

int WINAPI HookShowCursor(BOOL show) {
    if (!g_originalShowCursor) return show ? 0 : -1;
    if (!show && g_restoreCursor.load() && g_forceCursorVisible.load() &&
        IsGameWindowActive()) {
        // A negative result tells the engine's usual hide-until-negative loop
        // that the cursor is already hidden, without changing the real count.
        return -1;
    }
    return g_originalShowCursor(show);
}

HCURSOR WINAPI HookSetCursor(HCURSOR cursor) {
    if (!g_originalSetCursor) return nullptr;
    const bool forcing = g_restoreCursor.load() && g_forceCursorVisible.load() &&
                         IsGameWindowActive();
    if (!forcing && IsUsableGameCursor(cursor)) g_gameCursor.store(cursor);
    if (forcing) {
        const HCURSOR gameCursor = g_gameCursor.load();
        if (IsUsableGameCursor(gameCursor)) cursor = gameCursor;
    }
    return g_originalSetCursor(cursor);
}

void PatchWindowsLoaderImports(HMODULE module) {
    // Our own imports remain untouched, so wrappers call the actual loader.
    if (!module || module == g_self) return;
    for (const char* dll : {"kernel32.dll", "kernelbase.dll"}) {
        void* ignored = nullptr;
        ReplaceImport(module, dll, "LoadLibraryW", reinterpret_cast<void*>(HookLoadLibraryW), &ignored);
        ignored = nullptr;
        ReplaceImport(module, dll, "LoadLibraryA", reinterpret_cast<void*>(HookLoadLibraryA), &ignored);
        ignored = nullptr;
        ReplaceImport(module, dll, "LoadLibraryExW", reinterpret_cast<void*>(HookLoadLibraryExW), &ignored);
        ignored = nullptr;
        ReplaceImport(module, dll, "LoadLibraryExA", reinterpret_cast<void*>(HookLoadLibraryExA), &ignored);
    }
}

void PatchModule(HMODULE module) {
    if (module == g_self) return;
    // Store forwarding targets before exposing either hook in an import slot.
    ReplaceImport(module, "dxgi.dll", "CreateDXGIFactory1",
                  reinterpret_cast<void*>(HookCreateDXGIFactory1),
                  reinterpret_cast<void**>(&g_originalCreateDXGIFactory1));

    ReplaceImport(module, "d3d11.dll", "D3D11CreateDevice",
                  reinterpret_cast<void*>(HookD3D11CreateDevice),
                  reinterpret_cast<void**>(&g_originalD3D11CreateDevice));
    const bool showCursorHooked = ReplaceImport(
        module, "user32.dll", "ShowCursor", reinterpret_cast<void*>(HookShowCursor),
        reinterpret_cast<void**>(&g_originalShowCursor));
    const bool setCursorHooked = ReplaceImport(
        module, "user32.dll", "SetCursor", reinterpret_cast<void*>(HookSetCursor),
        reinterpret_cast<void**>(&g_originalSetCursor));
    static std::atomic<bool> cursorHooksLogged{false};
    if (showCursorHooked && setCursorHooked && !cursorHooksLogged.exchange(true))
        Log("Game ShowCursor/SetCursor imports hooked");
    // Install the body-layout observer on both native Windows and Wine.  It is
    // needed to identify panel-free NVL passages before their glyph draws.
    // ExperimentalWrapWidth separately gates the optional width mutation by
    // the INI switch and the exact observed ADV layout.
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    InstallExperimentalWrapping(module, path);
    InstallVoiceTextSpeed(module, path);
    InstallVoiceContinuation(module, path);
    InstallXAudioTrace(module, path);
    if (module == GetModuleHandleW(nullptr) || wcsstr(path, L"_WIN_Master.dll"))
        PatchWindowsLoaderImports(module);
}

void PatchLoadedModules() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            PatchModule(entry.hModule);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

DWORD WINAPI InitializationThread(void*) {
    Log("MyMerryMayFix starting: version 1.0.0, build 47 (2026-10-11); native-Windows=%d", g_nativeWindows);
    // Keep polling because the launcher loads the selected Master DLL after this
    // proxy may already have been initialized.
    for (unsigned attempt = 0; attempt < 240 && g_running.load(); ++attempt) {
        PatchLoadedModules();
        Sleep(250);
    }
    return 0;
}

} // namespace

void Initialize(HMODULE selfModule) {
    g_self = selfModule;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_nativeWindows = !ntdll || !GetProcAddress(ntdll, "wine_get_version");
    g_iniPath = ModuleSiblingPath(L"MyMerryMayFix.ini");
    g_logPath = ModuleSiblingPath(L"MyMerryMayFix.log");
    LoadConfiguration();
    g_running.store(true);
    // Install the loader interception before the launcher loads its selector.
    // XAudio2 is created by that selector and shared with the chosen game, so
    // polling only after the game module appears is too late for audio tracing.
    PatchModule(GetModuleHandleW(nullptr));
    for (const wchar_t* name : {L"MyMerryMaySel_WIN_Master.dll", L"MyMerryMay_WIN_Master.dll",
                               L"MyMerryMaybe_WIN_Master.dll"}) {
        if (HMODULE module = GetModuleHandleW(name)) PatchModule(module);
    }
    Log("Launcher imports patched synchronously (%s)",
        g_nativeWindows ? "Windows" : "Wine/Proton");
    HANDLE thread = CreateThread(nullptr, 0, InitializationThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}

void Shutdown() {
    g_running.store(false);
    if (g_window && g_originalWndProc) {
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    }
}

bool IsOverlayVisible() {
    return g_overlayVisible.load();
}

bool IsCursorRestoreEnabled() {
    return g_restoreCursor.load();
}

bool IsGameWindowActive() {
    if (!g_window) return false;
    const HWND foreground = GetForegroundWindow();
    return foreground == g_window || GetAncestor(foreground, GA_ROOT) == GetAncestor(g_window, GA_ROOT);
}

void ReportMouseReacquire(LONG lostResult, LONG acquireResult, LONG retryResult) {
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) < 32) {
        Log("DirectInput mouse reacquire: lost=0x%08lx acquire=0x%08lx retry=0x%08lx",
            static_cast<unsigned long>(lostResult), static_cast<unsigned long>(acquireResult),
            static_cast<unsigned long>(retryResult));
    }
}

void ReportMouseWake(const char* api, const char* edge) {
    Log("DirectInput mouse wake pulse: %s %s", api, edge);
}

} // namespace mmmfix

