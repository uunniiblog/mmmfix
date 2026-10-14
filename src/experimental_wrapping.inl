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
std::atomic<bool> g_wrapGuardReady[2]{};
struct WrapGuardAddresses {
    UINT site;
    UINT advanceCheck;
    UINT acceptGlyph;
};
constexpr WrapGuardAddresses kWrapGuardAddresses[2] = {
    {0xd02cb, 0xcdf2a, 0xd0765}, // May
    {0xd04db, 0xce13a, 0xd0975}  // Maybe
};
PVOID g_wrapDiagnosticHandler = nullptr;
std::atomic<unsigned long long> g_wrapDiagnosticSequence{0};
struct WrapDiagnosticTrace {
    unsigned long long sequence = 0;
    unsigned game = 0;
    UINT originalWidth = 0;
    UINT adjustedWidth = 0;
    bool active = false;
};
thread_local WrapDiagnosticTrace g_wrapDiagnosticTrace;

LONG CALLBACK WrapExceptionDiagnostic(EXCEPTION_POINTERS* exception) {
    if (!g_wrappingDiagnostics.load() || !g_logging.load() || !exception ||
        !exception->ExceptionRecord || !exception->ContextRecord ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t rip = static_cast<uintptr_t>(exception->ContextRecord->Rip);
    for (unsigned game = 0; game < 2; ++game) {
        const auto& state = g_wrapModules[game];
        const uintptr_t base = reinterpret_cast<uintptr_t>(state.module);
        if (!base || rip < base + state.routine || rip >= base + state.routine + 0xb00)
            continue;
        const auto& trace = g_wrapDiagnosticTrace;
        const auto* record = exception->ExceptionRecord;
        const uintptr_t fault = record->NumberParameters >= 2
            ? static_cast<uintptr_t>(record->ExceptionInformation[1]) : 0;
        Log("Wrap crash: game=%s tid=%lu seq=%llu active=%d width=%u->%u rip=0x%llx fault=%p rax=0x%llx rcx=0x%llx rsp=%p",
            game ? "Maybe" : "May", GetCurrentThreadId(), trace.sequence,
            trace.active && trace.game == game, trace.originalWidth, trace.adjustedWidth,
            static_cast<unsigned long long>(rip - base), reinterpret_cast<void*>(fault),
            static_cast<unsigned long long>(exception->ContextRecord->Rax),
            static_cast<unsigned long long>(exception->ContextRecord->Rcx),
            reinterpret_cast<void*>(exception->ContextRecord->Rsp));
        // This May routine stores the glyph index and line-width accounting in
        // these stack slots. Read them only at the verified failing instruction.
        if (rip == base + kWrapGuardAddresses[game].site + 15) {
            const auto* stack = reinterpret_cast<const BYTE*>(exception->ContextRecord->Rsp);
            UINT index = 0, advance = 0, glyphWidth = 0, limit = 0, lineBase = 0;
            memcpy(&index, stack + 0x20, sizeof(index));
            memcpy(&advance, stack + 0x30, sizeof(advance));
            memcpy(&lineBase, stack + 0x3c, sizeof(lineBase));
            memcpy(&glyphWidth, stack + 0x58, sizeof(glyphWidth));
            memcpy(&limit, stack + 0x70, sizeof(limit));
            const auto* flags = reinterpret_cast<const BYTE*>(base +
                (game ? 0x12fe570 : 0x12fd530));
            const auto* count = reinterpret_cast<const UINT*>(base +
                (game ? 0x12f2f48 : 0x12f1f88));
            Log("Wrap crash line state: glyph-index=%u current-advance=%u line-base=%u glyph-width=%u limit=%u glyph-count=%u first-flags=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x",
                index, advance, lineBase, glyphWidth, limit, *count, flags[0], flags[1], flags[2],
                flags[3], flags[4], flags[5], flags[6], flags[7]);
        }
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallWrapExceptionDiagnostic() {
    if (g_wrappingDiagnostics.load() && g_logging.load() && !g_wrapDiagnosticHandler) {
        g_wrapDiagnosticHandler = AddVectoredExceptionHandler(1, WrapExceptionDiagnostic);
        Log("Wrap crash diagnostics installed=%d", g_wrapDiagnosticHandler != nullptr);
    }
}

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
    const UINT requestedWidth = g_wrapWidth.load();
    if (!g_wrapEnabled.load() || !requestedWidth ||
        g_aspectMode.load() != static_cast<LONG>(AspectMode::Pillarbox) ||
        !IsObservedAdvLayout(width, caller, origin, arg3)) return width;
    return std::min(width, requestedWidth);
}

uintptr_t RunExperimentalWrap(unsigned game, void* returnAddress, UINT width, int arg2, int arg3) {
    auto& state = g_wrapModules[game];
    const uintptr_t rva = reinterpret_cast<uintptr_t>(returnAddress) - reinterpret_cast<uintptr_t>(state.module);
    UINT caller = 0;
    for (UINT i = 0; i < 6; ++i) if (rva == state.calls[i] + 5) caller = i + 1;
    ObserveBodyLayout(width, caller, arg2, arg3);
    // Never narrow dialogue unless its first-glyph underflow is guarded.
    // The engine can carry the previous segment's advance into a new segment
    // whose glyph index starts at zero.
    const UINT adjusted = caller && g_wrapGuardReady[game].load()
        ? ExperimentalWrapWidth(width, caller, arg2, arg3) : width;
    if (caller && state.samples[caller - 1].fetch_add(1) < 32)
        Log("Wrap experiment: game=%s caller=%u rva=0x%llx width=%u -> %u arg2=%d arg3=%d aspect=%ld ADV-match=%d",
            game ? "Maybe" : "May", caller, static_cast<unsigned long long>(rva - 5), width, adjusted,
            arg2, arg3, g_aspectMode.load(), IsObservedAdvLayout(width, caller, arg2, arg3));
    const bool diagnostic = g_wrappingDiagnostics.load() && g_logging.load() &&
        IsObservedAdvLayout(width, caller, arg2, arg3);
    if (diagnostic) {
        g_wrapDiagnosticTrace = {g_wrapDiagnosticSequence.fetch_add(1) + 1, game,
                                 width, adjusted, true};
        Log("Wrap call enter: game=%s tid=%lu seq=%llu width=%u->%u arg2=%d arg3=%d",
            game ? "Maybe" : "May", GetCurrentThreadId(), g_wrapDiagnosticTrace.sequence,
            width, adjusted, arg2, arg3);
    }
    const uintptr_t result = state.original(adjusted, arg2, arg3);
    if (diagnostic) {
        if (game == 0) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(state.module);
            const auto* flags = reinterpret_cast<const BYTE*>(base + 0x12fd530);
            const auto* count = reinterpret_cast<const UINT*>(base + 0x12f1f88);
            const auto* glyphs = reinterpret_cast<const UINT*>(base + 0x12fe190);
            Log("Wrap call leave: game=May tid=%lu seq=%llu result=0x%llx glyph-count=%u first-flags=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x first-glyphs=%x,%x,%x,%x",
                GetCurrentThreadId(), g_wrapDiagnosticTrace.sequence,
                static_cast<unsigned long long>(result), *count, flags[0], flags[1],
                flags[2], flags[3], flags[4], flags[5], flags[6], flags[7],
                glyphs[0], glyphs[1], glyphs[2], glyphs[3]);
        } else {
            Log("Wrap call leave: game=Maybe tid=%lu seq=%llu result=0x%llx",
                GetCurrentThreadId(), g_wrapDiagnosticTrace.sequence,
                static_cast<unsigned long long>(result));
        }
        g_wrapDiagnosticTrace.active = false;
    }
    return result;
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

bool PrepareFirstGlyphWrapGuard(unsigned game, BYTE* base, BYTE* relay) {
    // Both games inspect the preceding glyph on overflow without checking
    // whether this resumed segment begins at index zero. The upstream width
    // check below normally inserts a break glyph first; if that fails, accept
    // glyph zero instead of reading flags[-1] and crashing.
    const auto& addresses = kWrapGuardAddresses[game];
    const BYTE expected[] = {0x8b, 0x44, 0x24, 0x20, 0xff, 0xc8};
    if (memcmp(base + addresses.site, expected, sizeof(expected))) return false;
    BYTE guard[] = {
        0x8b, 0x44, 0x24, 0x20, // mov eax,[rsp+20h] (glyph index)
        0x85, 0xc0,             // test eax,eax
        0x75, 0x05,             // jnz original dec eax
        0xe9, 0, 0, 0, 0,       // accept glyph zero without prior-glyph lookup
        0xff, 0xc8,             // original dec eax for nonzero index
        0xe9, 0, 0, 0, 0        // resume original routine
    };
    BYTE* target = relay + 32;
    const auto acceptDisplacement = static_cast<int32_t>(
        (base + addresses.acceptGlyph) - (target + 13));
    const auto resumeDisplacement = static_cast<int32_t>(
        (base + addresses.site + sizeof(expected)) - (target + sizeof(guard)));
    memcpy(guard + 9, &acceptDisplacement, sizeof(acceptDisplacement));
    memcpy(guard + 16, &resumeDisplacement, sizeof(resumeDisplacement));
    memcpy(target, guard, sizeof(guard));
    return true;
}

bool InstallFirstGlyphWrapGuard(unsigned game, BYTE* base, BYTE* relay) {
    BYTE* site = base + kWrapGuardAddresses[game].site;
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    BYTE patch[] = {0xe9, 0, 0, 0, 0, 0x90};
    const auto displacement = static_cast<int32_t>(
        (relay + 32) - (site + 5));
    memcpy(patch + 1, &displacement, sizeof(displacement));
    memcpy(site, patch, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(site, sizeof(patch), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    return true;
}

bool PrepareAdvAdvanceCheck(unsigned game, BYTE* base, BYTE* relay) {
    // This earlier check compares the current pen position plus the next
    // glyph against the layout's right edge. On overflow, the engine appends
    // its real break glyph before the new text segment is wrapped. Restrict
    // the replacement bound to the observed ADV layout (origin 285, edge
    // 1420); NVL and other UI layouts retain their native values.
    const BYTE expected[] = {0x0f, 0xb7, 0x4c, 0x0a, 0x14};
    const auto& addresses = kWrapGuardAddresses[game];
    if (memcmp(base + addresses.advanceCheck, expected, sizeof(expected))) return false;
    BYTE code[] = {
        0x50,                         // push rax
        0x0f,0xbf,0x44,0x0a,0x04,     // movsx eax,word [rdx+rcx+4] (origin)
        0x3d,0x1d,0x01,0x00,0x00,     // cmp eax,285
        0x58,                         // pop rax
        0x0f,0xb7,0x4c,0x0a,0x14,     // original: movzx ecx,word [rdx+rcx+14h]
        0x75,0x46,                    // jne done
        0x81,0xf9,0x8c,0x05,0x00,0x00,// cmp ecx,1420
        0x75,0x3e,                    // jne done
        0x41,0x52,                    // push r10
        0x49,0xba,0,0,0,0,0,0,0,0,    // mov r10,&g_wrapEnabled
        0x41,0x80,0x3a,0x00,          // cmp byte [r10],0
        0x74,0x2a,                    // je restore
        0x49,0xba,0,0,0,0,0,0,0,0,    // mov r10,&g_aspectMode
        0x41,0x83,0x3a,0x01,          // cmp dword [r10],1
        0x75,0x1a,                    // jne restore
        0x49,0xba,0,0,0,0,0,0,0,0,    // mov r10,&g_wrapWidth
        0x45,0x8b,0x12,              // mov r10d,[r10]
        0x45,0x85,0xd2,              // test r10d,r10d (zero = observe only)
        0x74,0x08,                    // jz restore
        0x41,0x39,0xca,              // cmp r10d,ecx
        0x73,0x03,                    // jae restore
        0x44,0x89,0xd1,              // mov ecx,r10d
        0x41,0x5a,                    // restore: pop r10
        0xe9,0,0,0,0                 // done: resume engine comparison
    };
    static_assert(sizeof(code) == 94);
    const auto enabledPointer = reinterpret_cast<uintptr_t>(&g_wrapEnabled);
    const auto aspectPointer = reinterpret_cast<uintptr_t>(&g_aspectMode);
    const auto widthPointer = reinterpret_cast<uintptr_t>(&g_wrapWidth);
    memcpy(code + 31, &enabledPointer, sizeof(enabledPointer));
    memcpy(code + 47, &aspectPointer, sizeof(aspectPointer));
    memcpy(code + 63, &widthPointer, sizeof(widthPointer));
    BYTE* target = relay + 128;
    const auto resumeDisplacement = static_cast<int32_t>(
        (base + addresses.advanceCheck + sizeof(expected)) - (target + sizeof(code)));
    memcpy(code + 90, &resumeDisplacement, sizeof(resumeDisplacement));
    memcpy(target, code, sizeof(code));
    return true;
}

bool InstallAdvAdvanceCheck(unsigned game, BYTE* base, BYTE* relay) {
    BYTE* site = base + kWrapGuardAddresses[game].advanceCheck;
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    BYTE patch[] = {0xe9,0,0,0,0};
    const auto displacement = static_cast<int32_t>((relay + 128) - (site + 5));
    memcpy(patch + 1, &displacement, sizeof(displacement));
    memcpy(site, patch, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(site, sizeof(patch), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    return true;
}

void InstallExperimentalWrapping(HMODULE module, const wchar_t* path) {
    // Install both guards even when wrapping starts disabled so the overlay can
    // enable it live. The hot path reads Enabled, Width, and aspect mode.
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
    const bool guardPrepared = PrepareFirstGlyphWrapGuard(game, base, relay) &&
        PrepareAdvAdvanceCheck(game, base, relay);
    DWORD ignored;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &ignored)) {
        VirtualProtect(first, length, oldProtect, &ignored);
        VirtualFree(relay, 0, MEM_RELEASE);
        ReleaseSRWLockExclusive(&g_wrapInstallLock);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), relay, 128 + 94);
    const bool firstGlyphInstalled = guardPrepared && InstallFirstGlyphWrapGuard(game, base, relay);
    const bool advanceInstalled = firstGlyphInstalled && InstallAdvAdvanceCheck(game, base, relay);
    g_wrapGuardReady[game].store(firstGlyphInstalled && advanceInstalled);
    Log("%s ADV live wrapping guards installed=%d/%d",
        game ? "Maybe" : "May", advanceInstalled, firstGlyphInstalled);
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
    Log("Layout observer installed %s; ADV Enabled=%d Width=%u; platform=%s",
        game ? "Maybe" : "May", g_wrapEnabled.load(), g_wrapWidth.load(),
        g_nativeWindows ? "Windows" : "Wine/Proton");
    ReleaseSRWLockExclusive(&g_wrapInstallLock);
}

