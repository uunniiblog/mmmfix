// Included inside mod.cpp's anonymous namespace. The two supported game DLLs
// calculate a voice-derived message duration before calling the common text
// setup routine. Unvoiced dialogue passes zero for that argument, selecting
// the normal user-configured text speed. Redirect only those verified calls
// and optionally replace the voice duration with zero.
using EngineMessageSetupFn = int (*)(void*, int, int, int, int, int);

struct VoiceTextModule {
    HMODULE module = nullptr;
    EngineMessageSetupFn original = nullptr;
    BYTE* relay = nullptr;
    UINT routine;
    UINT calls[2];
    std::atomic<unsigned> samples{0};
};

VoiceTextModule g_voiceTextModules[2] = {
    {nullptr, nullptr, nullptr, 0xd08f0, {0x89cef, 0x89d38}},
    {nullptr, nullptr, nullptr, 0xd0b00, {0x8a61f, 0x8a668}}
};
SRWLOCK g_voiceTextInstallLock = SRWLOCK_INIT;

int RunVoiceTextSetup(unsigned game, void* text, int message, int layout,
                      int voiceDuration, int arg5, int arg6) {
    auto& state = g_voiceTextModules[game];
    const int adjustedDuration = g_matchVoicedTextSpeed.load() ? 0 : voiceDuration;
    const unsigned sample = state.samples.fetch_add(1);
    if (sample < 32 && voiceDuration != 0) {
        Log("Voice text timing: game=%s duration=%d -> %d match-normal=%d",
            game ? "Maybe" : "May", voiceDuration, adjustedDuration,
            g_matchVoicedTextSpeed.load());
    }
    return state.original(text, message, layout, adjustedDuration, arg5, arg6);
}

__declspec(noinline) int HookMayVoiceText(void* text, int message, int layout,
                                         int voiceDuration, int arg5, int arg6) {
    return RunVoiceTextSetup(0, text, message, layout, voiceDuration, arg5, arg6);
}

__declspec(noinline) int HookMaybeVoiceText(void* text, int message, int layout,
                                           int voiceDuration, int arg5, int arg6) {
    return RunVoiceTextSetup(1, text, message, layout, voiceDuration, arg5, arg6);
}

void InstallVoiceTextSpeed(HMODULE module, const wchar_t* path) {
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    unsigned game;
    if (!_wcsicmp(name, L"MyMerryMay_WIN_Master.dll")) game = 0;
    else if (!_wcsicmp(name, L"MyMerryMaybe_WIN_Master.dll")) game = 1;
    else return;

    AcquireSRWLockExclusive(&g_voiceTextInstallLock);
    auto& state = g_voiceTextModules[game];
    if (state.module == module) {
        ReleaseSRWLockExclusive(&g_voiceTextInstallLock);
        return;
    }

    BYTE* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const BYTE signature[] = {
        0x44,0x89,0x4c,0x24,0x20, 0x44,0x89,0x44,0x24,0x18,
        0x89,0x54,0x24,0x10, 0x48,0x89,0x4c,0x24,0x08,
        0x48,0x83,0xec,0x58
    };
    bool valid = nt->OptionalHeader.SizeOfImage > state.calls[1] + 5 &&
                 !memcmp(base + state.routine, signature, sizeof(signature));
    for (UINT call : state.calls) {
        if (!valid) break;
        int32_t displacement = 0;
        memcpy(&displacement, base + call + 1, sizeof(displacement));
        valid = base[call] == 0xe8 &&
            static_cast<int64_t>(call) + 5 + displacement == state.routine;
    }
    if (!valid) {
        Log("Voice text timing: %s unsupported routine/caller signatures; skipped",
            game ? "Maybe" : "May");
        state.module = module;
        ReleaseSRWLockExclusive(&g_voiceTextInstallLock);
        return;
    }

    BYTE* relay = AllocateWrapRelay(base);
    DWORD oldProtect = 0;
    BYTE* first = base + state.calls[0];
    const SIZE_T length = state.calls[1] + 5 - state.calls[0];
    if (!relay || !VirtualProtect(first, length, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (relay) VirtualFree(relay, 0, MEM_RELEASE);
        Log("Voice text timing: allocation/protection failed; skipped");
        ReleaseSRWLockExclusive(&g_voiceTextInstallLock);
        return;
    }

    relay[0] = 0xff;
    relay[1] = 0x25; // JMP [RIP+0], preserving the original caller return address.
    memset(relay + 2, 0, 4);
    auto hook = game ? HookMaybeVoiceText : HookMayVoiceText;
    memcpy(relay + 6, &hook, sizeof(hook));
    DWORD ignored = 0;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &ignored)) {
        VirtualProtect(first, length, oldProtect, &ignored);
        VirtualFree(relay, 0, MEM_RELEASE);
        ReleaseSRWLockExclusive(&g_voiceTextInstallLock);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), relay, 14);

    state.original = reinterpret_cast<EngineMessageSetupFn>(base + state.routine);
    state.module = module;
    state.relay = relay;
    for (UINT call : state.calls) {
        const int32_t displacement = static_cast<int32_t>(
            reinterpret_cast<intptr_t>(relay) - reinterpret_cast<intptr_t>(base + call + 5));
        memcpy(base + call + 1, &displacement, sizeof(displacement));
    }
    VirtualProtect(first, length, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), first, length);
    Log("Voice text timing installed %s; match-normal=%d; platform=%s",
        game ? "Maybe" : "May", g_matchVoicedTextSpeed.load(),
        g_nativeWindows ? "Windows" : "Wine/Proton");
    ReleaseSRWLockExclusive(&g_voiceTextInstallLock);
}
