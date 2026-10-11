// Included inside mod.cpp's anonymous namespace. Shared body-layout observer;
// redirect verified direct calls, leaving the engine's function and unwind
// metadata intact. The original three integer arguments are preserved.
using EngineWrapFn = uintptr_t (*)(UINT, int, int);
struct WrapModule {
    HMODULE module = nullptr;
    EngineWrapFn original = nullptr;
    BYTE* relay = nullptr;
    UINT routine;
    UINT calls[6];
    std::atomic<unsigned> samples[6]{};
};
WrapModule g_wrapModules[2] = {
    {nullptr, nullptr, nullptr, 0xcfc70, {0xd09b5, 0xd19af, 0xd2c5a, 0xd43b9, 0xd44e7, 0xd45b8}},
    {nullptr, nullptr, nullptr, 0xcfe80, {0xd0bc5, 0xd1bbf, 0xd2e6a, 0xd45c9, 0xd46f7, 0xd47c8}}
};
SRWLOCK g_wrapInstallLock = SRWLOCK_INIT;

bool IsObservedAdvLayout(UINT width, UINT caller, int origin, int arg3) {
    // Caller 1 is shared by ADV and NVL. The live May captures identify ADV
    // as (1420,285,0), and NVL as (1620,180,0). Fail closed on other layouts,
    // including unconfirmed Maybe variants, instead of narrowing unrelated UI.
    return caller == 1 && width == 1420 && origin == 285 && arg3 == 0;
}

void ObserveBodyLayout(UINT width, UINT caller, int origin, int arg3) {
    if (caller != 1) return;
    // Only caller 1 selects the active dialogue body. Backlog/other callers
    // must neither select NVL nor erase the current dialogue mode.
    const bool nvl = width == 1620 && origin == 180 && arg3 == 0;
    if (g_engineNvlLayout.exchange(nvl) != nvl)
        Log("Body layout changed: engineNVL=%d width=%u origin=%d arg3=%d", nvl, width, origin, arg3);
}

UINT ExperimentalWrapWidth(UINT width, UINT caller, int origin, int arg3) {
    if (!g_wrapEnabled || !g_wrapWidth ||
        g_aspectMode.load() != static_cast<LONG>(AspectMode::Pillarbox) ||
        (g_wrapCaller != 0 && g_wrapCaller != caller) ||
        !IsObservedAdvLayout(width, caller, origin, arg3)) return width;
    return std::min(width, g_wrapWidth);
}

uintptr_t RunExperimentalWrap(unsigned game, void* returnAddress, UINT width, int arg2, int arg3) {
    auto& state = g_wrapModules[game];
    const uintptr_t rva = reinterpret_cast<uintptr_t>(returnAddress) - reinterpret_cast<uintptr_t>(state.module);
    UINT caller = 0;
    for (UINT i = 0; i < 6; ++i) if (rva == state.calls[i] + 5) caller = i + 1;
    ObserveBodyLayout(width, caller, arg2, arg3);
    const UINT adjusted = caller ? ExperimentalWrapWidth(width, caller, arg2, arg3) : width;
    if (caller && state.samples[caller - 1].fetch_add(1) < 32)
        Log("Wrap experiment: game=%s caller=%u rva=0x%llx width=%u -> %u arg2=%d arg3=%d aspect=%ld ADV-match=%d",
            game ? "Maybe" : "May", caller, static_cast<unsigned long long>(rva - 5), width, adjusted,
            arg2, arg3, g_aspectMode.load(), IsObservedAdvLayout(width, caller, arg2, arg3));
    return state.original(adjusted, arg2, arg3);
}
#ifdef _MSC_VER
#define MMM_WRAP_RETURN_ADDRESS _ReturnAddress()
#else
#define MMM_WRAP_RETURN_ADDRESS __builtin_return_address(0)
#endif
__declspec(noinline) uintptr_t HookMayWrap(UINT width, int arg2, int arg3) {
    return RunExperimentalWrap(0, MMM_WRAP_RETURN_ADDRESS, width, arg2, arg3);
}
__declspec(noinline) uintptr_t HookMaybeWrap(UINT width, int arg2, int arg3) {
    return RunExperimentalWrap(1, MMM_WRAP_RETURN_ADDRESS, width, arg2, arg3);
}
#undef MMM_WRAP_RETURN_ADDRESS

BYTE* AllocateWrapRelay(BYTE* base) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    // Seek a free region above this module, within the rel32 call range.
    uintptr_t cursor = reinterpret_cast<uintptr_t>(base);
    const uintptr_t limit = cursor + 0x70000000;
    while (cursor < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region))) break;
        const uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (end <= cursor) break;
        const uintptr_t aligned = (cursor + info.dwAllocationGranularity - 1) &
                                  ~(static_cast<uintptr_t>(info.dwAllocationGranularity) - 1);
        if (region.State == MEM_FREE && aligned + 4096 <= end && aligned + 4096 < limit) {
            if (void* result = VirtualAlloc(reinterpret_cast<void*>(aligned), 4096,
                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<BYTE*>(result);
        }
        cursor = end;
    }
    return nullptr;
}

void InstallExperimentalWrapping(HMODULE module, const wchar_t* path) {
    // Observe the body layout even when the ADV width experiment is disabled.
    // Only ExperimentalWrapWidth changes arguments, and still requires Enabled.
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    unsigned game;
    if (!_wcsicmp(name, L"MyMerryMay_WIN_Master.dll")) game = 0;
    else if (!_wcsicmp(name, L"MyMerryMaybe_WIN_Master.dll")) game = 1;
    else return;
    AcquireSRWLockExclusive(&g_wrapInstallLock);
    auto& state = g_wrapModules[game];
    if (state.module == module) { ReleaseSRWLockExclusive(&g_wrapInstallLock); return; }
    BYTE* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    // The complete three argument stores, push and stack allocation. None of
    // these instructions is relocated; this signature only identifies code.
    const BYTE signature[] = {0x44,0x89,0x44,0x24,0x18,0x89,0x54,0x24,0x10,
        0x89,0x4c,0x24,0x08,0x57,0x48,0x81,0xec,0xd0,0x01,0x00,0x00};
    bool valid = nt->OptionalHeader.SizeOfImage > state.calls[5] + 5 &&
        !memcmp(base + state.routine, signature, sizeof(signature));
    for (UINT call : state.calls) {
        if (!valid) break;
        int32_t displacement = 0;
        memcpy(&displacement, base + call + 1, sizeof(displacement));
        valid = base[call] == 0xe8 &&
            static_cast<int64_t>(call) + 5 + displacement == state.routine;
    }
    if (!valid) {
        Log("Wrap experiment: %s unsupported routine/caller signatures; skipped", game ? "Maybe" : "May");
        // Remember rejection to avoid repeating it on each polling scan.
        state.module = module;
        ReleaseSRWLockExclusive(&g_wrapInstallLock);
        return;
    }
    BYTE* relay = AllocateWrapRelay(base);
    DWORD oldProtect = 0;
    // All six call sites occupy this small contiguous range. Protect it before
    // publishing anything, so failure cannot leave a partial installation.
    BYTE* first = base + state.calls[0];
    const SIZE_T length = state.calls[5] + 5 - state.calls[0];
    if (!relay || !VirtualProtect(first, length, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (relay) VirtualFree(relay, 0, MEM_RELEASE);
        Log("Wrap experiment: allocation/protection failed; skipped");
        ReleaseSRWLockExclusive(&g_wrapInstallLock);
        return;
    }
    relay[0] = 0xff; relay[1] = 0x25; // JMP [RIP+0], preserves caller return address
    memset(relay + 2, 0, 4);
    auto hook = game ? HookMaybeWrap : HookMayWrap;
    memcpy(relay + 6, &hook, sizeof(hook));
    DWORD ignored;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &ignored)) {
        VirtualProtect(first, length, oldProtect, &ignored);
        VirtualFree(relay, 0, MEM_RELEASE);
        ReleaseSRWLockExclusive(&g_wrapInstallLock);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), relay, 14);
    state.original = reinterpret_cast<EngineWrapFn>(base + state.routine);
    state.module = module;
    state.relay = relay;
    for (UINT call : state.calls) {
        const int32_t displacement = static_cast<int32_t>(reinterpret_cast<intptr_t>(relay) -
                                                         reinterpret_cast<intptr_t>(base + call + 5));
        memcpy(base + call + 1, &displacement, sizeof(displacement));
    }
    VirtualProtect(first, length, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), first, length);
    Log("Layout observer installed %s; ADV Enabled=%d Width=%u Caller=%u; platform=%s",
        game ? "Maybe" : "May", g_wrapEnabled, g_wrapWidth, g_wrapCaller,
        g_nativeWindows ? "Windows" : "Wine/Proton");
    ReleaseSRWLockExclusive(&g_wrapInstallLock);
}

