#include "mod.h"

#include <xinput.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace mmmfix {
namespace {

using GetXInputStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
GetXInputStateFn ResolveXInput() {
    static std::once_flag once;
    static GetXInputStateFn function = nullptr;
    std::call_once(once, [] {
        for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            HMODULE module = LoadLibraryW(name);
            if (!module) continue;
            function = reinterpret_cast<GetXInputStateFn>(GetProcAddress(module, "XInputGetState"));
            if (function) break;
            FreeLibrary(module);
        }
    });
    return function;
}

bool FirstConnectedPad(XINPUT_STATE& state, DWORD* slot = nullptr) {
    const auto getState = ResolveXInput();
    if (!getState) return false;
    for (DWORD index = 0; index < 4; ++index) {
        if (getState(index, &state) == ERROR_SUCCESS) {
            if (slot) *slot = index;
            return true;
        }
    }
    return false;
}

// Steam Input's action handles are opaque. Values in this range belong only to
// this bridge, so unknown handles can safely be forwarded to the Steam DLL.
constexpr uint64_t kActionPrefix = 0x4d4d464958000000ull;
constexpr uint64_t kControllerHandle = 0x4d4d464958000100ull;
constexpr uint64_t kActionSetHandle = 0x4d4d464958000200ull;

unsigned ActionId(const char* name) {
    if (!name) return 0;
    static constexpr const char* names[] = {
        "A", "B", "x", "y", "STA", "SEL", "L1", "R1", "L2", "R2",
        "L3", "R3", "UP", "Down", "RIGHT", "LEFT"
    };
    for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); ++index)
        if (_stricmp(name, names[index]) == 0) return index + 1;
    return 0;
}

bool ActionPressed(unsigned id, const XINPUT_STATE& state) {
    const WORD buttons = state.Gamepad.wButtons;
    switch (id) {
    case 1: return (buttons & XINPUT_GAMEPAD_A) != 0;
    case 2: return (buttons & XINPUT_GAMEPAD_B) != 0;
    case 3: return (buttons & XINPUT_GAMEPAD_X) != 0;
    case 4: return (buttons & XINPUT_GAMEPAD_Y) != 0;
    case 5: return (buttons & XINPUT_GAMEPAD_START) != 0;
    case 6: return (buttons & XINPUT_GAMEPAD_BACK) != 0;
    case 7: return (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    case 8: return (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    case 9: return state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    case 10: return state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    case 11: return (buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    case 12: return (buttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    case 13: return (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0;
    case 14: return (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    case 15: return (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
    case 16: return (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    default: return false;
    }
}

struct DigitalActionData {
    bool state;
    bool active;
};
static_assert(sizeof(DigitalActionData) == 2);

using InitFn = bool(STDMETHODCALLTYPE*)(void*, bool);
using ConnectedFn = int(STDMETHODCALLTYPE*)(void*, uint64_t*);
using HandleFn = uint64_t(STDMETHODCALLTYPE*)(void*, const char*);
using CurrentSetFn = uint64_t(STDMETHODCALLTYPE*)(void*, uint64_t);
using InputTypeFn = int(STDMETHODCALLTYPE*)(void*, uint64_t);
using DigitalFn = DigitalActionData*(STDMETHODCALLTYPE*)(
    void*, DigitalActionData*, uint64_t, uint64_t);

struct SteamInterfaceHooks {
    void** vtable = nullptr;
    InitFn init = nullptr;
    ConnectedFn connected = nullptr;
    HandleFn actionSet = nullptr;
    CurrentSetFn currentSet = nullptr;
    InputTypeFn inputType = nullptr;
    HandleFn digitalHandle = nullptr;
    DigitalFn digitalData = nullptr;
};
SteamInterfaceHooks g_input;
SteamInterfaceHooks g_controller;
std::atomic<bool> g_actionDataUsed{false};
std::atomic<ULONGLONG> g_lastActionRead{0};
std::atomic<bool> g_lastActionState[17]{};

template <typename Function>
bool HookVtable(void* object, size_t index, Function replacement, Function& original) {
    void** vtable = *reinterpret_cast<void***>(object);
    void** slot = &vtable[index];
    if (*slot == reinterpret_cast<void*>(replacement)) return true;
    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection)) return false;
    original = reinterpret_cast<Function>(*slot);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(replacement));
    DWORD unused = 0;
    VirtualProtect(slot, sizeof(void*), oldProtection, &unused);
    return true;
}

bool BridgeInit(SteamInterfaceHooks& hooks, void* self, bool explicitRunFrame) {
    const bool originalResult = hooks.init ? hooks.init(self, explicitRunFrame) : false;
    XINPUT_STATE state{};
    const bool connected = FirstConnectedPad(state);
    ReportControllerDiagnostic("Steam Input Init original=%d XInput-connected=%d",
        originalResult, connected);
    return originalResult || connected;
}

int BridgeConnected(SteamInterfaceHooks& hooks, void* self, uint64_t* output) {
    XINPUT_STATE state{};
    if (IsXInputEnabled() && FirstConnectedPad(state)) {
        if (output) output[0] = kControllerHandle;
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 4)
            ReportControllerDiagnostic("Steam Input controller enumeration: synthetic controller present");
        return 1;
    }
    return hooks.connected ? hooks.connected(self, output) : 0;
}

uint64_t BridgeActionSet(SteamInterfaceHooks& hooks, void* self, const char* name) {
    if (name && _stricmp(name, "GameControls") == 0) {
        ReportControllerDiagnostic("Steam Input action set requested: %s", name);
        return kActionSetHandle;
    }
    return hooks.actionSet ? hooks.actionSet(self, name) : 0;
}

uint64_t BridgeCurrentSet(SteamInterfaceHooks& hooks, void* self, uint64_t controller) {
    XINPUT_STATE state{};
    if (IsXInputEnabled() && FirstConnectedPad(state)) return kActionSetHandle;
    return hooks.currentSet ? hooks.currentSet(self, controller) : 0;
}

int BridgeInputType(SteamInterfaceHooks& hooks, void* self, uint64_t controller) {
    if (controller == kControllerHandle && IsXInputEnabled()) {
        XINPUT_STATE state{};
        if (FirstConnectedPad(state)) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true))
                ReportControllerDiagnostic("Steam Input synthetic controller type: Xbox One (3)");
            return 3; // k_ESteamInputType_XBoxOneController
        }
    }
    return hooks.inputType ? hooks.inputType(self, controller) : 0;
}

uint64_t BridgeDigitalHandle(SteamInterfaceHooks& hooks, void* self, const char* name) {
    const unsigned id = ActionId(name);
    if (id) {
        ReportControllerDiagnostic("Steam Input digital action requested: %s id=%u", name, id);
        return kActionPrefix | id;
    }
    return hooks.digitalHandle ? hooks.digitalHandle(self, name) : 0;
}

DigitalActionData* BridgeDigitalData(SteamInterfaceHooks& hooks, void* self,
                                     DigitalActionData* output, uint64_t controller,
                                     uint64_t action) {
    if (output &&
        (action & ~0xffull) == kActionPrefix &&
        (action & 0xffull) >= 1 && (action & 0xffull) <= 16) {
        XINPUT_STATE state{};
        if (IsXInputEnabled() && FirstConnectedPad(state)) {
            const unsigned id = static_cast<unsigned>(action & 0xffull);
            output->state = !IsOverlayVisible() && IsGameWindowActive() &&
                ActionPressed(id, state);
            output->active = true;
            g_lastActionRead.store(GetTickCount64());
            if (g_lastActionState[id].exchange(output->state) != output->state)
                ReportControllerDiagnostic("Steam Input action id=%u pressed=%d",
                    id, output->state);
            if (!g_actionDataUsed.exchange(true))
                ReportControllerDiagnostic("Steam Input native action data active; DirectInput digital bridge suppressed");
            return output;
        }
    }
    return hooks.digitalData ? hooks.digitalData(self, output, controller, action) : output;
}

bool STDMETHODCALLTYPE HookInputInit(void* self, bool runFrame) {
    return BridgeInit(g_input, self, runFrame);
}
bool STDMETHODCALLTYPE HookControllerInit(void* self, bool runFrame) {
    return BridgeInit(g_controller, self, runFrame);
}
int STDMETHODCALLTYPE HookInputConnected(void* self, uint64_t* output) {
    return BridgeConnected(g_input, self, output);
}
int STDMETHODCALLTYPE HookControllerConnected(void* self, uint64_t* output) {
    return BridgeConnected(g_controller, self, output);
}
uint64_t STDMETHODCALLTYPE HookInputActionSet(void* self, const char* name) {
    return BridgeActionSet(g_input, self, name);
}
uint64_t STDMETHODCALLTYPE HookControllerActionSet(void* self, const char* name) {
    return BridgeActionSet(g_controller, self, name);
}
uint64_t STDMETHODCALLTYPE HookInputCurrentSet(void* self, uint64_t controller) {
    return BridgeCurrentSet(g_input, self, controller);
}
uint64_t STDMETHODCALLTYPE HookControllerCurrentSet(void* self, uint64_t controller) {
    return BridgeCurrentSet(g_controller, self, controller);
}
int STDMETHODCALLTYPE HookInputType(void* self, uint64_t controller) {
    return BridgeInputType(g_input, self, controller);
}
uint64_t STDMETHODCALLTYPE HookInputDigitalHandle(void* self, const char* name) {
    return BridgeDigitalHandle(g_input, self, name);
}
uint64_t STDMETHODCALLTYPE HookControllerDigitalHandle(void* self, const char* name) {
    return BridgeDigitalHandle(g_controller, self, name);
}
DigitalActionData* STDMETHODCALLTYPE HookInputDigitalData(
    void* self, DigitalActionData* output, uint64_t controller, uint64_t action) {
    return BridgeDigitalData(g_input, self, output, controller, action);
}
DigitalActionData* STDMETHODCALLTYPE HookControllerDigitalData(
    void* self, DigitalActionData* output, uint64_t controller, uint64_t action) {
    return BridgeDigitalData(g_controller, self, output, controller, action);
}

} // namespace

bool IsSteamActionBridgeActive() {
    const ULONGLONG lastRead = g_lastActionRead.load();
    return lastRead != 0 && GetTickCount64() - lastRead < 250;
}

void InstallSteamInputBridge(void* interfacePointer, const char* version) {
    if (!IsXInputEnabled() || !interfacePointer || !version) return;
    SteamInterfaceHooks* hooks = nullptr;
    bool input = false;
    if (strcmp(version, "SteamInput006") == 0) {
        hooks = &g_input;
        input = true;
    } else if (strcmp(version, "SteamController008") == 0) {
        hooks = &g_controller;
    } else {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(interfacePointer);
    if (hooks->vtable == vtable) return;
    if ((input && g_controller.vtable == vtable) ||
        (!input && g_input.vtable == vtable)) {
        ReportControllerDiagnostic("Steam Input versions share a vtable; skipping incompatible second hook");
        return;
    }
    if (hooks->vtable) {
        ReportControllerDiagnostic("Steam Input interface vtable changed; leaving new object unmodified");
        return;
    }
    // Slot numbers were checked against the game's bundled Steam API flat
    // wrappers for SteamInput006 and SteamController008, respectively.
    const bool installed = input
        ? (HookVtable(interfacePointer, 0, HookInputInit, hooks->init) &&
           HookVtable(interfacePointer, 6, HookInputConnected, hooks->connected) &&
           HookVtable(interfacePointer, 9, HookInputActionSet, hooks->actionSet) &&
           HookVtable(interfacePointer, 11, HookInputCurrentSet, hooks->currentSet) &&
           HookVtable(interfacePointer, 16, HookInputDigitalHandle, hooks->digitalHandle) &&
           HookVtable(interfacePointer, 17, HookInputDigitalData, hooks->digitalData) &&
           HookVtable(interfacePointer, 37, HookInputType, hooks->inputType))
        : (HookVtable(interfacePointer, 0, HookControllerInit, hooks->init) &&
           HookVtable(interfacePointer, 3, HookControllerConnected, hooks->connected) &&
           HookVtable(interfacePointer, 4, HookControllerActionSet, hooks->actionSet) &&
           HookVtable(interfacePointer, 6, HookControllerCurrentSet, hooks->currentSet) &&
           HookVtable(interfacePointer, 13, HookControllerDigitalHandle, hooks->digitalHandle) &&
           HookVtable(interfacePointer, 12, HookControllerDigitalData, hooks->digitalData));
    if (installed) hooks->vtable = vtable;
    ReportControllerDiagnostic("Steam Input interface %s object=%p vtable=%p hooks=%d",
        version, interfacePointer, vtable, installed);
}

void PollSelectorControllerPrompt(HWND window) {
    if (!IsXInputEnabled() || !window || IsOverlayVisible() || !IsGameWindowActive() ||
        !GetModuleHandleW(L"MyMerryMaySel_WIN_Master.dll") ||
        GetModuleHandleW(L"MyMerryMay_WIN_Master.dll") ||
        GetModuleHandleW(L"MyMerryMaybe_WIN_Master.dll")) return;
    static WORD previousButtons = 0;
    static bool prompted = false;
    XINPUT_STATE state{};
    if (!FirstConnectedPad(state)) return;
    const WORD buttons = state.Gamepad.wButtons;
    const bool newPress = (buttons & ~previousButtons) != 0;
    previousButtons = buttons;
    if (prompted || !newPress || IsSteamActionBridgeActive()) return;
    // The selector's opening "press any button" screen does not respond to
    // the DirectInput keyboard-state bridge. Supply a normal window key event;
    // TranslateMessage will also generate WM_CHAR if that is what it needs.
    const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC)) << 16;
    if (PostMessageW(window, WM_KEYDOWN, VK_RETURN, scan)) {
        PostMessageW(window, WM_KEYUP, VK_RETURN, scan | (1u << 30) | (1u << 31));
        prompted = true;
        ReportControllerDiagnostic("Selector opening controller press posted Enter key messages");
    }
}

} // namespace mmmfix
