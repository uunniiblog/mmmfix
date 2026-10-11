#include "../src/mod.cpp"
#include <cassert>

UINT observedWidth;
int observed2, observed3;
uintptr_t Observe(UINT width, int arg2, int arg3) {
    observedWidth = width; observed2 = arg2; observed3 = arg3;
    return 0x123456789abcdef0ULL;
}
int main() {
    using namespace mmmfix;
    g_logging.store(false);
    g_nativeWindows = true;
    g_wrapEnabled = true; g_wrapWidth = 1320; g_wrapCaller = 0;
    g_aspectMode.store(1);
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 1320);
    assert(ExperimentalWrapWidth(1620, 1, 180, 0) == 1620); // NVL
    assert(ExperimentalWrapWidth(1500, 4, 0, 0) == 1500); // backlog
    assert(ExperimentalWrapWidth(1920, 2, 0, 0) == 1920); // other text
    assert(ExperimentalWrapWidth(1420, 1, 180, 0) == 1420); // unknown layout
    assert(ExperimentalWrapWidth(1420, 1, 285, 1) == 1420);
    g_wrapWidth = 700;
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 700);
    assert(ExperimentalWrapWidth(1620, 1, 180, 0) == 1620);
    g_wrapWidth = 2000;
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 1420);
    g_wrapCaller = 2; g_wrapWidth = 1320;
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 1420);
    g_wrapCaller = 1; g_wrapWidth = 0;
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 1420);
    g_wrapWidth = 1320; g_aspectMode.store(0);
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 1420);
    g_aspectMode.store(1); g_nativeWindows = false;
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 1420);
    g_nativeWindows = true; g_wrapEnabled = false;
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 1420);
    g_wrapEnabled = true;
    ObserveBodyLayout(1620, 1, 180, 0);
    assert(g_engineNvlLayout.load());
    ObserveBodyLayout(1500, 4, 0, 0);
    assert(g_engineNvlLayout.load());
    ObserveBodyLayout(1420, 1, 285, 0);
    assert(!g_engineNvlLayout.load());
    g_wrapEnabled = false;
    ObserveBodyLayout(1620, 1, 180, 0);
    assert(g_engineNvlLayout.load()); // Independent of ADV wrapping switch.
    g_nativeWindows = false;
    ObserveBodyLayout(1420, 1, 285, 0);
    assert(!g_engineNvlLayout.load()); // Wine observes ADV/NVL without width changes.
    ObserveBodyLayout(1620, 1, 180, 0);
    assert(g_engineNvlLayout.load());
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 1420);
    g_nativeWindows = true; g_wrapEnabled = true;
    // Validate actual supplied PE signatures/call offsets without resolving
    // imports or executing any game initialization.
    for (unsigned game = 0; game < 2; ++game) {
        const wchar_t* path = game ? L"../../MyMerryMaybe_WIN_Master.dll" : L"../../MyMerryMay_WIN_Master.dll";
        HMODULE module = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
        assert(module);
        wchar_t fullPath[MAX_PATH]{};
        GetModuleFileNameW(module, fullPath, MAX_PATH);
        g_nativeWindows = game == 0; // Exercise observer installation with Wine branch too.
        InstallExperimentalWrapping(module, fullPath);
        auto& state = g_wrapModules[game];
        assert(state.original && state.relay && state.module == module);
        BYTE* base = reinterpret_cast<BYTE*>(module);
        for (UINT call : state.calls) {
            int32_t displacement;
            memcpy(&displacement, base + call + 1, 4);
            assert(base + call + 5 + displacement == state.relay);
        }
        auto original = state.original;
        InstallExperimentalWrapping(module, fullPath);
        assert(state.original == original);
        state.original = Observe;
        const auto result = RunExperimentalWrap(game, base + state.calls[0] + 5, 1420, 285, 0);
        assert(observedWidth == (game == 0 ? 1320u : 1420u) && observed2 == 285 && observed3 == 0);
        assert(result == 0x123456789abcdef0ULL);
        // Execute the actual relay as well, checking its absolute jump and ABI.
        assert(reinterpret_cast<EngineWrapFn>(state.relay)(1800, -5, 8) == result);
        assert(observedWidth == 1800 && observed2 == -5 && observed3 == 8);
        RunExperimentalWrap(game, base + state.calls[0] + 5, 1800, 3, 0);
        assert(observedWidth == 1800);
        state.original = original;
        FreeLibrary(module);
    }
    std::puts("Wrap policy, argument forwarding, and both real module hook signatures passed");
}


