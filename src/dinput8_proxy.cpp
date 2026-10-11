#include "mod.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <unknwn.h>
#include <dinput.h>

#include <cstring>
#include <atomic>

namespace {

HMODULE g_realDinput8 = nullptr;

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, IUnknown*);
using AcquireFn = HRESULT(STDMETHODCALLTYPE*)(void*);
using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

struct Input8Hooks {
    void** vtable = nullptr;
    CreateDeviceFn createDevice = nullptr;
};

struct InputDeviceHooks {
    void* device = nullptr;
    void** vtable = nullptr;
    bool mouse = false;
    AcquireFn acquire = nullptr;
    GetDeviceStateFn getDeviceState = nullptr;
    GetDeviceDataFn getDeviceData = nullptr;
};

Input8Hooks g_input8Hooks[2];
InputDeviceHooks g_inputDeviceHooks[8];
std::atomic<bool> g_mouseNeedsAcquire{false};
// 0 = idle, 1 = synthesize a harmless middle-button press, 2 = release it.
std::atomic<unsigned> g_mouseWakePhase{0};
std::atomic<unsigned> g_mouseWakeReports{0};

void ReportMouseWake(const char* api, const char* edge) {
    if (g_mouseWakeReports.fetch_add(1) < 16)
        mmmfix::ReportMouseWake(api, edge);
}

void ApplyMouseWakeToState(InputDeviceHooks* hooks, DWORD size, LPVOID data,
                           HRESULT result) {
    if (!hooks || !hooks->mouse || FAILED(result) || !data ||
        size < sizeof(DIMOUSESTATE)) return;
    auto* state = static_cast<DIMOUSESTATE*>(data);
    unsigned expected = 1;
    if (g_mouseWakePhase.compare_exchange_strong(expected, 2)) {
        state->rgbButtons[2] = 0x80;
        ReportMouseWake("state", "press");
    } else {
        expected = 2;
        if (g_mouseWakePhase.compare_exchange_strong(expected, 0)) {
            state->rgbButtons[2] = 0;
            ReportMouseWake("state", "release");
        }
    }
}

template <typename Function>
bool HookVtable(void* object, size_t index, Function replacement, Function& original) {
    if (!object) return false;
    void** vtable = *reinterpret_cast<void***>(object);
    void** slot = &vtable[index];
    if (*slot == reinterpret_cast<void*>(replacement)) return true;
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    original = reinterpret_cast<Function>(*slot);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(replacement));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return true;
}

Input8Hooks* FindInput8(void* object) {
    void** vtable = *reinterpret_cast<void***>(object);
    for (auto& hooks : g_input8Hooks) if (hooks.vtable == vtable) return &hooks;
    return nullptr;
}

InputDeviceHooks* FindInputDevice(void* object) {
    for (auto& hooks : g_inputDeviceHooks) if (hooks.device == object) return &hooks;
    return nullptr;
}

bool IsLostInput(HRESULT result) {
    return result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED;
}

HRESULT ReacquireMouseAndRetryState(InputDeviceHooks* hooks, void* device,
                                    DWORD size, LPVOID data, HRESULT result) {
    if (!hooks || !hooks->mouse || !hooks->acquire ||
        !mmmfix::IsCursorRestoreEnabled() || !IsLostInput(result)) return result;
    g_mouseNeedsAcquire.store(true);
    if (!mmmfix::IsGameWindowActive()) return result;
    const HRESULT acquired = hooks->acquire(device);
    const HRESULT retried = SUCCEEDED(acquired)
        ? hooks->getDeviceState(device, size, data) : result;
    if (SUCCEEDED(retried)) g_mouseNeedsAcquire.store(false);
    mmmfix::ReportMouseReacquire(result, acquired, retried);
    return retried;
}

HRESULT STDMETHODCALLTYPE HookGetDeviceState(void* device, DWORD size, LPVOID data) {
    InputDeviceHooks* hooks = FindInputDevice(device);
    if (!hooks || !hooks->getDeviceState) return DIERR_GENERIC;
    HRESULT result = hooks->getDeviceState(device, size, data);
    result = ReacquireMouseAndRetryState(hooks, device, size, data, result);
    ApplyMouseWakeToState(hooks, size, data, result);
    if (mmmfix::IsOverlayVisible() && SUCCEEDED(result) && data) std::memset(data, 0, size);
    return result;
}

HRESULT STDMETHODCALLTYPE HookGetDeviceData(void* device, DWORD objectSize,
                                            LPDIDEVICEOBJECTDATA data, LPDWORD entries,
                                            DWORD flags) {
    InputDeviceHooks* hooks = FindInputDevice(device);
    if (!hooks || !hooks->getDeviceData) return DIERR_GENERIC;
    const DWORD capacity = entries ? *entries : 0;
    HRESULT result = hooks->getDeviceData(device, objectSize, data, entries, flags);
    if (hooks->mouse && hooks->acquire && mmmfix::IsCursorRestoreEnabled() && IsLostInput(result)) {
        g_mouseNeedsAcquire.store(true);
        if (!mmmfix::IsGameWindowActive()) {
            if (mmmfix::IsOverlayVisible() && entries) *entries = 0;
            return result;
        }
        const HRESULT acquired = hooks->acquire(device);
        const HRESULT retried = SUCCEEDED(acquired)
            ? hooks->getDeviceData(device, objectSize, data, entries, flags) : result;
        mmmfix::ReportMouseReacquire(result, acquired, retried);
        result = retried;
        if (SUCCEEDED(retried)) g_mouseNeedsAcquire.store(false);
    }
    if (hooks->mouse && SUCCEEDED(result) && data && entries &&
        objectSize >= sizeof(DIDEVICEOBJECTDATA) && *entries < capacity) {
        unsigned expected = 1;
        DWORD buttonData = 0;
        const char* edge = nullptr;
        if (g_mouseWakePhase.compare_exchange_strong(expected, 2)) {
            buttonData = 0x80;
            edge = "press";
        } else {
            expected = 2;
            if (g_mouseWakePhase.compare_exchange_strong(expected, 0))
                edge = "release";
        }
        if (edge) {
            auto* event = reinterpret_cast<DIDEVICEOBJECTDATA*>(
                reinterpret_cast<unsigned char*>(data) + objectSize * (*entries));
            std::memset(event, 0, sizeof(*event));
            event->dwOfs = DIMOFS_BUTTON2;
            event->dwData = buttonData;
            ++*entries;
            ReportMouseWake("data", edge);
        }
    }
    if (mmmfix::IsOverlayVisible() && entries && SUCCEEDED(result)) *entries = 0;
    return result;
}

void HookInputDevice(void* device, bool mouse) {
    if (!device) return;
    void** vtable = *reinterpret_cast<void***>(device);
    for (auto& hooks : g_inputDeviceHooks) if (hooks.device == device) return;
    for (auto& hooks : g_inputDeviceHooks) {
        if (hooks.device) continue;
        hooks.device = device;
        hooks.vtable = vtable;
        hooks.mouse = mouse;
        // Keyboard and mouse objects can share a vtable. Reuse the forwarding
        // functions captured by the first object instead of attempting to hook
        // the same slots again and leaving this record without originals.
        InputDeviceHooks* shared = nullptr;
        for (auto& other : g_inputDeviceHooks) {
            if (&other != &hooks && other.device && other.vtable == vtable) {
                shared = &other;
                break;
            }
        }
        if (shared) {
            hooks.acquire = shared->acquire;
            hooks.getDeviceState = shared->getDeviceState;
            hooks.getDeviceData = shared->getDeviceData;
        } else {
            // Acquire itself does not need interception; retain its original
            // vtable entry so a failed state read can invoke it directly.
            hooks.acquire = reinterpret_cast<AcquireFn>(vtable[7]);
            HookVtable(device, 9, HookGetDeviceState, hooks.getDeviceState);
            HookVtable(device, 10, HookGetDeviceData, hooks.getDeviceData);
        }
        return;
    }
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(void* input, REFGUID deviceGuid,
                                           void** output, IUnknown* outer) {
    Input8Hooks* hooks = FindInput8(input);
    if (!hooks || !hooks->createDevice) return DIERR_GENERIC;
    const HRESULT result = hooks->createDevice(input, deviceGuid, output, outer);
    // GUID_SysMouse = {6F1D2B60-D5A0-11CF-BFC7-444553540000}.
    static constexpr GUID systemMouse{0x6f1d2b60, 0xd5a0, 0x11cf,
                                      {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    if (SUCCEEDED(result) && output && *output)
        HookInputDevice(*output, IsEqualGUID(deviceGuid, systemMouse));
    return result;
}

void HookInput8(void* input) {
    if (!input) return;
    void** vtable = *reinterpret_cast<void***>(input);
    for (auto& hooks : g_input8Hooks) if (hooks.vtable == vtable) return;
    for (auto& hooks : g_input8Hooks) {
        if (hooks.vtable) continue;
        hooks.vtable = vtable;
        HookVtable(input, 3, HookCreateDevice, hooks.createDevice);
        return;
    }
}

FARPROC ResolveReal(const char* name) {
    if (!g_realDinput8) {
        wchar_t systemDirectory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (!length || length >= MAX_PATH - 13) {
            return nullptr;
        }
        lstrcatW(systemDirectory, L"\\dinput8.dll");
        g_realDinput8 = LoadLibraryW(systemDirectory);
    }
    return g_realDinput8 ? GetProcAddress(g_realDinput8, name) : nullptr;
}

} // namespace

namespace mmmfix {

void ReacquireMouseDevices() {
    if (!IsCursorRestoreEnabled() || !IsGameWindowActive() ||
        !g_mouseNeedsAcquire.exchange(false)) return;
    bool acquiredAny = false;
    for (auto& hooks : g_inputDeviceHooks) {
        if (!hooks.device || !hooks.mouse || !hooks.acquire) continue;
        const HRESULT acquired = hooks.acquire(hooks.device);
        ReportMouseReacquire(DIERR_NOTACQUIRED, acquired, acquired);
        if (SUCCEEDED(acquired)) acquiredAny = true;
    }
    if (!acquiredAny) g_mouseNeedsAcquire.store(true);
}

void RequestMouseWake() {
    unsigned expected = 0;
    g_mouseWakePhase.compare_exchange_strong(expected, 1);
}

} // namespace mmmfix

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE instance,
    DWORD version,
    REFIID interfaceId,
    LPVOID* output,
    IUnknown* outer) {
    using Function = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    const auto function = reinterpret_cast<Function>(ResolveReal("DirectInput8Create"));
    if (!function) return E_FAIL;
    const HRESULT result = function(instance, version, interfaceId, output, outer);
    if (SUCCEEDED(result) && output && *output) HookInput8(*output);
    return result;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() {
    using Function = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Function>(ResolveReal("DllCanUnloadNow"));
    return function ? function() : S_FALSE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(
    REFCLSID classId,
    REFIID interfaceId,
    LPVOID* output) {
    using Function = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    const auto function = reinterpret_cast<Function>(ResolveReal("DllGetClassObject"));
    return function ? function(classId, interfaceId, output) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllRegisterServer() {
    using Function = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Function>(ResolveReal("DllRegisterServer"));
    return function ? function() : E_NOTIMPL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllUnregisterServer() {
    using Function = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Function>(ResolveReal("DllUnregisterServer"));
    return function ? function() : E_NOTIMPL;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // Resolve the real DLL before any exported function is called. The full
        // System32 path is important when this DLL is selected by a Wine override.
        ResolveReal("DirectInput8Create");
        mmmfix::Initialize(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        mmmfix::Shutdown();
    }
    return TRUE;
}
