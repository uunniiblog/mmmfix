// The engine's per-channel stop wrapper performs two destructive audio calls
// before clearing its bookkeeping. Hooking only the second call is too late:
// the first one already silences the playing voice. Intercept the wrapper at
// entry and skip it only for dialogue channel 3's ordinary line-advance caller.
using EngineChannelStopFn = void (*)(int);

struct VoiceContinueModule {
    HMODULE module = nullptr;
    EngineChannelStopFn original = nullptr;
    BYTE* trampoline = nullptr;
    UINT stopWrapper;
    UINT lineAdvanceCaller;
    UINT newVoiceCaller;
    UINT preserveStateContinuation;
    UINT channelStateTable;
    std::atomic<unsigned> samples{0};
};

VoiceContinueModule g_voiceContinueModules[2] = {
    {nullptr, nullptr, nullptr, 0x7ba30, 0x7d90c, 0x7c7fb, 0x7dd82, 0x2e6600},
    {nullptr, nullptr, nullptr, 0x7c360, 0x7e23c, 0x7d12b, 0x7e6b2, 0x2e75e0}
};
SRWLOCK g_voiceContinueInstallLock = SRWLOCK_INIT;

#ifdef _MSC_VER
#define MMM_VOICE_RETURN_ADDRESS _ReturnAddress()
#define MMM_VOICE_RETURN_SLOT reinterpret_cast<void**>(_AddressOfReturnAddress())
#else
#define MMM_VOICE_RETURN_ADDRESS __builtin_return_address(0)
#define MMM_VOICE_RETURN_SLOT reinterpret_cast<void**>( \
    reinterpret_cast<BYTE*>(__builtin_frame_address(0)) + sizeof(void*))
#endif

bool RunChannelStop(unsigned game, int channel, void* returnAddress) {
    auto& state = g_voiceContinueModules[game];
    const uintptr_t base = reinterpret_cast<uintptr_t>(state.module);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(returnAddress);
    const uintptr_t callerRva = caller >= base ? caller - base : 0;
    const bool suppress = g_continueVoiceUntilNext.load() && channel == 3 &&
                          callerRva == state.lineAdvanceCaller;
    if (g_continueVoiceUntilNext.load() && channel == 3 &&
        callerRva == state.newVoiceCaller) {
        g_dialogueVoiceCreatePending.store(1, std::memory_order_release);
        Log("Voice continuation: %s channel-3 replacement armed for XAudio source association",
            game ? "Maybe" : "May");
    }
    const unsigned sample = state.samples.fetch_add(1);
    if (sample < 256) {
        Log("Voice channel-stop: game=%s caller-rva=0x%llx channel=%d keep-until-next=%d action=%s",
            game ? "Maybe" : "May", static_cast<unsigned long long>(callerRva), channel,
            g_continueVoiceUntilNext.load(),
            suppress ? "suppress-line-advance-and-state-reset" : "pass");
    }
    // Replacement voices use different callers, so they still run the full
    // wrapper and replace the preceding clip. Scene/load/all-channel cleanup
    // likewise remains untouched.
    if (!suppress) {
        state.original(channel);
    } else {
        // The update routine cleared this nonzero active/continuation flag at
        // caller-0x19 just before entering the stop wrapper. Restore it before
        // returning past both channel cleanup and the zero-timer volume update;
        // otherwise playback survives but is immediately muted to volume zero.
        BYTE* channelState = reinterpret_cast<BYTE*>(state.module) + state.channelStateTable +
                             static_cast<SIZE_T>(channel) * 0xa8;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(channelState + 0x54), 1);
    }
    return suppress;
}

__declspec(noinline) void HookMayChannelStop(int channel) {
    void** returnSlot = MMM_VOICE_RETURN_SLOT;
    if (RunChannelStop(0, channel, *returnSlot)) {
        *returnSlot = reinterpret_cast<BYTE*>(g_voiceContinueModules[0].module) +
                      g_voiceContinueModules[0].preserveStateContinuation;
    }
}

__declspec(noinline) void HookMaybeChannelStop(int channel) {
    void** returnSlot = MMM_VOICE_RETURN_SLOT;
    if (RunChannelStop(1, channel, *returnSlot)) {
        *returnSlot = reinterpret_cast<BYTE*>(g_voiceContinueModules[1].module) +
                      g_voiceContinueModules[1].preserveStateContinuation;
    }
}

#undef MMM_VOICE_RETURN_SLOT
#undef MMM_VOICE_RETURN_ADDRESS

void InstallVoiceContinuation(HMODULE module, const wchar_t* path) {
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    unsigned game;
    if (!_wcsicmp(name, L"MyMerryMay_WIN_Master.dll")) game = 0;
    else if (!_wcsicmp(name, L"MyMerryMaybe_WIN_Master.dll")) game = 1;
    else return;

    AcquireSRWLockExclusive(&g_voiceContinueInstallLock);
    auto& state = g_voiceContinueModules[game];
    if (state.module == module) {
        ReleaseSRWLockExclusive(&g_voiceContinueInstallLock);
        return;
    }

    BYTE* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    // Copy complete instructions through IMUL. The continuation needs the RAX
    // result, so the trampoline jumps via R11 instead of clobbering RAX.
    const BYTE prologue[] = {
        0x89,0x4c,0x24,0x08,                   // mov [rsp+08],ecx
        0x48,0x83,0xec,0x38,                   // sub rsp,38
        0x8b,0x44,0x24,0x40,                   // mov eax,[rsp+40]
        0x48,0x69,0xc0,0x38,0x0b,0x02,0x00    // imul rax,rax,20b38
    };
    constexpr SIZE_T patchLength = sizeof(prologue);
    if (state.stopWrapper + patchLength > nt->OptionalHeader.SizeOfImage ||
        memcmp(base + state.stopWrapper, prologue, patchLength)) {
        Log("Voice continuation: %s unsupported wrapper signature; skipped",
            game ? "Maybe" : "May");
        state.module = module;
        ReleaseSRWLockExclusive(&g_voiceContinueInstallLock);
        return;
    }

    BYTE* trampoline = AllocateWrapRelay(base);
    BYTE* target = base + state.stopWrapper;
    DWORD oldProtect = 0;
    if (!trampoline || !VirtualProtect(target, patchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (trampoline) VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("Voice continuation: %s allocation/protection failed; skipped",
            game ? "Maybe" : "May");
        ReleaseSRWLockExclusive(&g_voiceContinueInstallLock);
        return;
    }

    memcpy(trampoline, prologue, patchLength);
    trampoline[19] = 0x49; trampoline[20] = 0xbb; // mov r11, continuation
    void* continuation = target + patchLength;
    memcpy(trampoline + 21, &continuation, sizeof(continuation));
    trampoline[29] = 0x41; trampoline[30] = 0xff; trampoline[31] = 0xe3; // jmp r11
    DWORD ignored = 0;
    if (!VirtualProtect(trampoline, 4096, PAGE_EXECUTE_READ, &ignored)) {
        VirtualProtect(target, patchLength, oldProtect, &ignored);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("Voice continuation: %s trampoline protection failed; skipped",
            game ? "Maybe" : "May");
        ReleaseSRWLockExclusive(&g_voiceContinueInstallLock);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), trampoline, 32);

    state.original = reinterpret_cast<EngineChannelStopFn>(trampoline);
    state.module = module;
    state.trampoline = trampoline;
    auto hook = game ? HookMaybeChannelStop : HookMayChannelStop;
    target[0] = 0x48; target[1] = 0xb8; // mov rax, hook
    memcpy(target + 2, &hook, sizeof(hook));
    target[10] = 0xff; target[11] = 0xe0; // jmp rax
    memset(target + 12, 0x90, patchLength - 12);
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);
    Log("Voice continuation installed %s wrapper=0x%x line-advance-caller=0x%x new-voice-caller=0x%x preserve-state=0x%x state-table=0x%x; keep-until-next=%d; platform=%s",
        game ? "Maybe" : "May", state.stopWrapper, state.lineAdvanceCaller,
        state.newVoiceCaller, state.preserveStateContinuation, state.channelStateTable,
        g_continueVoiceUntilNext.load(), g_nativeWindows ? "Windows" : "Wine/Proton");
    ReleaseSRWLockExclusive(&g_voiceContinueInstallLock);
}
