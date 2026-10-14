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
    g_wrapEnabled = true; g_wrapWidth = 1320;
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
    g_wrapWidth = 0;
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 1420);
    g_wrapWidth = 1320; g_aspectMode.store(0);
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 1420);
    g_aspectMode.store(1); g_nativeWindows = false;
    assert(ExperimentalWrapWidth(1420, 1, 285, 0) == 1320);
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
        g_wrapEnabled.store(false); // Hooks must install while disabled for live enabling.
        InstallExperimentalWrapping(module, fullPath);
        auto& state = g_wrapModules[game];
        assert(state.original && state.relay && state.module == module);
        assert(g_wrapGuardReady[game].load());
        BYTE* base = reinterpret_cast<BYTE*>(module);
        const auto& guard = kWrapGuardAddresses[game];
        assert(base[guard.site] == 0xe9 && base[guard.site + 5] == 0x90);
        int32_t displacement;
        memcpy(&displacement, base + guard.site + 1, 4);
        assert(base + guard.site + 5 + displacement == state.relay + 32);
        BYTE* firstGlyphGuard = state.relay + 32;
        assert(firstGlyphGuard[6] == 0x75 && firstGlyphGuard[7] == 0x05);
        memcpy(&displacement, firstGlyphGuard + 9, 4);
        assert(firstGlyphGuard + 13 + displacement == base + guard.acceptGlyph);
        memcpy(&displacement, firstGlyphGuard + 16, 4);
        assert(firstGlyphGuard + 20 + displacement == base + guard.site + 6);
        assert(base[guard.advanceCheck] == 0xe9);
        memcpy(&displacement, base + guard.advanceCheck + 1, 4);
        assert(base + guard.advanceCheck + 5 + displacement == state.relay + 128);
        BYTE* advanceCheck = state.relay + 128;
        assert(advanceCheck[17] == 0x75 && advanceCheck[18] == 0x46);
        assert(advanceCheck[25] == 0x75 && advanceCheck[26] == 0x3e);
        uintptr_t pointer;
        memcpy(&pointer, advanceCheck + 31, sizeof(pointer));
        assert(pointer == reinterpret_cast<uintptr_t>(&g_wrapEnabled));
        memcpy(&pointer, advanceCheck + 47, sizeof(pointer));
        assert(pointer == reinterpret_cast<uintptr_t>(&g_aspectMode));
        memcpy(&pointer, advanceCheck + 63, sizeof(pointer));
        assert(pointer == reinterpret_cast<uintptr_t>(&g_wrapWidth));
        memcpy(&displacement, advanceCheck + 90, 4);
        assert(advanceCheck + 94 + displacement == base + guard.advanceCheck + 5);
        for (UINT call : state.calls) {
            memcpy(&displacement, base + call + 1, 4);
            assert(base + call + 5 + displacement == state.relay);
        }
        auto original = state.original;
        InstallExperimentalWrapping(module, fullPath);
        assert(state.original == original);
        state.original = Observe;
        const auto result = RunExperimentalWrap(game, base + state.calls[0] + 5, 1420, 285, 0);
        assert(observedWidth == 1420u);
        g_wrapEnabled.store(true);
        RunExperimentalWrap(game, base + state.calls[0] + 5, 1420, 285, 0);
        assert(observedWidth == 1320u && observed2 == 285 && observed3 == 0);
        g_wrapWidth.store(1200);
        RunExperimentalWrap(game, base + state.calls[0] + 5, 1420, 285, 0);
        assert(observedWidth == 1200u);
        g_wrapEnabled.store(false);
        RunExperimentalWrap(game, base + state.calls[0] + 5, 1420, 285, 0);
        assert(observedWidth == 1420u);
        g_wrapWidth.store(1320);
        assert(result == 0x123456789abcdef0ULL);
        // Execute the actual relay as well, checking its absolute jump and ABI.
        assert(reinterpret_cast<EngineWrapFn>(state.relay)(1800, -5, 8) == result);
        assert(observedWidth == 1800 && observed2 == -5 && observed3 == 8);
        RunExperimentalWrap(game, base + state.calls[0] + 5, 1800, 3, 0);
        assert(observedWidth == 1800);
        state.original = original;
        FreeLibrary(module);
    }
    std::puts("Wrap policy, upstream break checks, first-glyph guards, argument forwarding, and both real module hook signatures passed");
}


