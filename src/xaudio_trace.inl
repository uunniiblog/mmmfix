// Diagnostic tracing at the final XAudio2 source-voice layer. This deliberately
// does not change playback: it records whether a clip is stopped, flushed,
// destroyed, muted, or simply reaches its submitted buffer's end.

using XAudio2CreateFn = HRESULT (WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
using XAudio2CreateWithVersionInfoFn = HRESULT (WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR, DWORD);
using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);
using XAudioCreateSourceVoiceFn = HRESULT (STDMETHODCALLTYPE*)(
    IXAudio2*, IXAudio2SourceVoice**, const WAVEFORMATEX*, UINT32, float,
    IXAudio2VoiceCallback*, const XAUDIO2_VOICE_SENDS*, const XAUDIO2_EFFECT_CHAIN*);
using XAudioStopEngineFn = void (STDMETHODCALLTYPE*)(IXAudio2*);
using XAudioSetVolumeFn = HRESULT (STDMETHODCALLTYPE*)(IXAudio2Voice*, float, UINT32);
using XAudioDestroyVoiceFn = void (STDMETHODCALLTYPE*)(IXAudio2Voice*);
using XAudioStartVoiceFn = HRESULT (STDMETHODCALLTYPE*)(IXAudio2SourceVoice*, UINT32, UINT32);
using XAudioStopVoiceFn = HRESULT (STDMETHODCALLTYPE*)(IXAudio2SourceVoice*, UINT32, UINT32);
using XAudioSubmitBufferFn = HRESULT (STDMETHODCALLTYPE*)(
    IXAudio2SourceVoice*, const XAUDIO2_BUFFER*, const XAUDIO2_BUFFER_WMA*);
using XAudioFlushBuffersFn = HRESULT (STDMETHODCALLTYPE*)(IXAudio2SourceVoice*);
using XAudioGetStateFn = void (STDMETHODCALLTYPE*)(IXAudio2SourceVoice*, XAUDIO2_VOICE_STATE*, UINT32);

GetProcAddressFn g_originalXAudioGetProcAddress = nullptr;
XAudio2CreateFn g_originalXAudio2Create = nullptr;
XAudio2CreateWithVersionInfoFn g_originalXAudio2CreateWithVersionInfo = nullptr;
XAudioCreateSourceVoiceFn g_originalXAudioCreateSourceVoice = nullptr;
XAudioStopEngineFn g_originalXAudioStopEngine = nullptr;
XAudioSetVolumeFn g_originalXAudioSetVolume = nullptr;
XAudioDestroyVoiceFn g_originalXAudioDestroyVoice = nullptr;
XAudioStartVoiceFn g_originalXAudioStartVoice = nullptr;
XAudioStopVoiceFn g_originalXAudioStopVoice = nullptr;
XAudioSubmitBufferFn g_originalXAudioSubmitBuffer = nullptr;
XAudioFlushBuffersFn g_originalXAudioFlushBuffers = nullptr;
std::atomic<unsigned> g_xaudioTraceEvents{0};

struct XAudioTraceCallback;
struct XAudioTraceVoice {
    std::atomic<IXAudio2SourceVoice*> voice{nullptr};
    XAudioTraceCallback* callback = nullptr;
    UINT32 sampleRate = 0;
    UINT32 averageBytesPerSecond = 0;
    WORD channels = 0;
    std::atomic<uint32_t> lastVolumeBits{0xffffffffu};
};
XAudioTraceVoice g_xaudioTraceVoices[128];
SRWLOCK g_xaudioTraceLock = SRWLOCK_INIT;

#ifdef _MSC_VER
#define MMM_XAUDIO_CALLER _ReturnAddress()
#else
#define MMM_XAUDIO_CALLER __builtin_return_address(0)
#endif

void DescribeXAudioCaller(void* address, char* text, size_t textSize) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(address);
    for (const wchar_t* moduleName : {L"MyMerryMay_WIN_Master.dll",
                                      L"MyMerryMaybe_WIN_Master.dll",
                                      L"MyMerryMaySel_WIN_Master.dll"}) {
        HMODULE module = GetModuleHandleW(moduleName);
        if (!module) continue;
        auto* base = reinterpret_cast<BYTE*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
        if (caller >= begin && caller < begin + nt->OptionalHeader.SizeOfImage) {
            const char* shortName = wcsstr(moduleName, L"Maybe") ? "Maybe" :
                                    (wcsstr(moduleName, L"Sel") ? "Selector" : "May");
            snprintf(text, textSize, "%s+0x%llx", shortName,
                     static_cast<unsigned long long>(caller - begin));
            return;
        }
    }
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &owner)) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(owner, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        char narrow[96]{};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
        snprintf(text, textSize, "%s+0x%llx", narrow,
                 static_cast<unsigned long long>(caller - reinterpret_cast<uintptr_t>(owner)));
    } else {
        snprintf(text, textSize, "%p", address);
    }
}

bool AllowXAudioTraceEvent() {
    return g_xaudioTraceEvents.fetch_add(1, std::memory_order_relaxed) < 8192;
}

bool IsDialogueXAudioVoice(IXAudio2SourceVoice* voice) {
    return voice && g_dialogueSourceVoice.load(std::memory_order_acquire) == voice;
}

void TraceXAudioEvent(IXAudio2SourceVoice* voice, const char* event, void* caller,
                      const char* detail = "", bool queryState = true) {
    if (!AllowXAudioTraceEvent()) return;
    char callerText[160]{};
    DescribeXAudioCaller(caller, callerText, sizeof(callerText));
    XAUDIO2_VOICE_STATE state{};
    if (voice && queryState) {
        auto*** object = reinterpret_cast<void***>(voice);
        auto getState = reinterpret_cast<XAudioGetStateFn>((*object)[25]);
        getState(voice, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    }
    Log("XAudio2 voice=%p event=%s caller=%s queued=%u current-context=%p %s",
        voice, event, callerText, state.BuffersQueued, state.pCurrentBufferContext, detail);
}

XAudioTraceVoice* FindXAudioTraceVoice(IXAudio2SourceVoice* voice) {
    for (auto& entry : g_xaudioTraceVoices) {
        if (entry.voice.load(std::memory_order_acquire) == voice) return &entry;
    }
    return nullptr;
}

struct XAudioTraceCallback final : IXAudio2VoiceCallback {
    IXAudio2VoiceCallback* original;
    std::atomic<IXAudio2SourceVoice*> voice{nullptr};

    explicit XAudioTraceCallback(IXAudio2VoiceCallback* callback) : original(callback) {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytesRequired) override {
        if (original) original->OnVoiceProcessingPassStart(bytesRequired);
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {
        if (original) original->OnVoiceProcessingPassEnd();
    }
    void STDMETHODCALLTYPE OnStreamEnd() override {
        IXAudio2SourceVoice* source = voice.load();
        if (IsDialogueXAudioVoice(source))
            TraceXAudioEvent(source, "callback-stream-end", MMM_XAUDIO_CALLER, "", false);
        if (original) original->OnStreamEnd();
    }
    void STDMETHODCALLTYPE OnBufferStart(void* context) override {
        char detail[80]{};
        snprintf(detail, sizeof(detail), "context=%p", context);
        IXAudio2SourceVoice* source = voice.load();
        if (IsDialogueXAudioVoice(source))
            TraceXAudioEvent(source, "callback-buffer-start", MMM_XAUDIO_CALLER, detail, false);
        if (original) original->OnBufferStart(context);
    }
    void STDMETHODCALLTYPE OnBufferEnd(void* context) override {
        char detail[80]{};
        snprintf(detail, sizeof(detail), "context=%p", context);
        IXAudio2SourceVoice* source = voice.load();
        if (IsDialogueXAudioVoice(source))
            TraceXAudioEvent(source, "callback-buffer-end", MMM_XAUDIO_CALLER, detail, false);
        if (original) original->OnBufferEnd(context);
    }
    void STDMETHODCALLTYPE OnLoopEnd(void* context) override {
        if (original) original->OnLoopEnd(context);
    }
    void STDMETHODCALLTYPE OnVoiceError(void* context, HRESULT error) override {
        char detail[96]{};
        snprintf(detail, sizeof(detail), "context=%p error=0x%08lx", context,
                 static_cast<unsigned long>(error));
        TraceXAudioEvent(voice.load(), "callback-error", MMM_XAUDIO_CALLER, detail, false);
        if (original) original->OnVoiceError(context, error);
    }
};

HRESULT STDMETHODCALLTYPE HookXAudioSetVolume(IXAudio2Voice* baseVoice, float volume,
                                               UINT32 operationSet) {
    auto* voice = reinterpret_cast<IXAudio2SourceVoice*>(baseVoice);
    uint32_t bits = 0;
    memcpy(&bits, &volume, sizeof(bits));
    XAudioTraceVoice* entry = FindXAudioTraceVoice(voice);
    const uint32_t previous = entry ? entry->lastVolumeBits.exchange(bits) : 0xffffffffu;
    float appliedVolume = volume;
    bool held = false;
    if (IsDialogueXAudioVoice(voice) && g_continueVoiceUntilNext.load()) {
        uint32_t heldBits = g_dialogueHeldVolumeBits.load(std::memory_order_acquire);
        float heldVolume = 0.0f;
        memcpy(&heldVolume, &heldBits, sizeof(heldVolume));
        if (volume > heldVolume) {
            g_dialogueHeldVolumeBits.store(bits, std::memory_order_release);
        } else if (volume < heldVolume) {
            appliedVolume = heldVolume;
            held = true;
        }
    }
    if (bits != previous || held) {
        char detail[96]{};
        snprintf(detail, sizeof(detail), "requested=%.6g applied=%.6g operation-set=%u action=%s",
                 volume, appliedVolume, operationSet, held ? "hold-dialogue" : "pass");
        TraceXAudioEvent(voice, "set-volume", MMM_XAUDIO_CALLER, detail);
    }
    return g_originalXAudioSetVolume(baseVoice, appliedVolume, operationSet);
}

void STDMETHODCALLTYPE HookXAudioDestroyVoice(IXAudio2Voice* baseVoice) {
    auto* voice = reinterpret_cast<IXAudio2SourceVoice*>(baseVoice);
    TraceXAudioEvent(voice, "destroy", MMM_XAUDIO_CALLER);
    XAudioTraceCallback* callback = nullptr;
    if (XAudioTraceVoice* entry = FindXAudioTraceVoice(voice)) callback = entry->callback;
    g_originalXAudioDestroyVoice(baseVoice);
    IXAudio2SourceVoice* expectedDialogue = voice;
    if (g_dialogueSourceVoice.compare_exchange_strong(expectedDialogue, nullptr)) {
        g_dialogueHeldVolumeBits.store(0, std::memory_order_release);
        Log("Voice continuation: dialogue XAudio source destroyed voice=%p", voice);
    }
    if (XAudioTraceVoice* entry = FindXAudioTraceVoice(voice)) {
        entry->voice.store(nullptr, std::memory_order_release);
        entry->callback = nullptr;
    }
    delete callback;
}

HRESULT STDMETHODCALLTYPE HookXAudioStartVoice(IXAudio2SourceVoice* voice, UINT32 flags,
                                                UINT32 operationSet) {
    char detail[96]{};
    snprintf(detail, sizeof(detail), "flags=0x%x operation-set=%u", flags, operationSet);
    TraceXAudioEvent(voice, "start", MMM_XAUDIO_CALLER, detail);
    return g_originalXAudioStartVoice(voice, flags, operationSet);
}

HRESULT STDMETHODCALLTYPE HookXAudioStopVoice(IXAudio2SourceVoice* voice, UINT32 flags,
                                               UINT32 operationSet) {
    char detail[96]{};
    snprintf(detail, sizeof(detail), "flags=0x%x operation-set=%u", flags, operationSet);
    TraceXAudioEvent(voice, "stop", MMM_XAUDIO_CALLER, detail);
    return g_originalXAudioStopVoice(voice, flags, operationSet);
}

HRESULT STDMETHODCALLTYPE HookXAudioSubmitBuffer(IXAudio2SourceVoice* voice,
                                                  const XAUDIO2_BUFFER* buffer,
                                                  const XAUDIO2_BUFFER_WMA* wma) {
    char detail[192]{};
    XAudioTraceVoice* entry = FindXAudioTraceVoice(voice);
    const double seconds = buffer && entry && entry->averageBytesPerSecond
        ? static_cast<double>(buffer->AudioBytes) / entry->averageBytesPerSecond : 0.0;
    snprintf(detail, sizeof(detail),
             "bytes=%u duration=%.3fs flags=0x%x play-begin=%u play-length=%u loops=%u context=%p",
             buffer ? buffer->AudioBytes : 0, seconds, buffer ? buffer->Flags : 0,
             buffer ? buffer->PlayBegin : 0, buffer ? buffer->PlayLength : 0,
             buffer ? buffer->LoopCount : 0, buffer ? buffer->pContext : nullptr);
    if (IsDialogueXAudioVoice(voice))
        TraceXAudioEvent(voice, "submit", MMM_XAUDIO_CALLER, detail);
    return g_originalXAudioSubmitBuffer(voice, buffer, wma);
}

HRESULT STDMETHODCALLTYPE HookXAudioFlushBuffers(IXAudio2SourceVoice* voice) {
    TraceXAudioEvent(voice, "flush", MMM_XAUDIO_CALLER);
    return g_originalXAudioFlushBuffers(voice);
}

void HookXAudioSourceVoice(IXAudio2SourceVoice* voice) {
    if (!voice) return;
    HookVtable(voice, 12, HookXAudioSetVolume, g_originalXAudioSetVolume);
    HookVtable(voice, 18, HookXAudioDestroyVoice, g_originalXAudioDestroyVoice);
    HookVtable(voice, 19, HookXAudioStartVoice, g_originalXAudioStartVoice);
    HookVtable(voice, 20, HookXAudioStopVoice, g_originalXAudioStopVoice);
    HookVtable(voice, 21, HookXAudioSubmitBuffer, g_originalXAudioSubmitBuffer);
    HookVtable(voice, 22, HookXAudioFlushBuffers, g_originalXAudioFlushBuffers);
}

HRESULT STDMETHODCALLTYPE HookXAudioCreateSourceVoice(
    IXAudio2* engine, IXAudio2SourceVoice** output, const WAVEFORMATEX* format,
    UINT32 flags, float maxRatio, IXAudio2VoiceCallback* callback,
    const XAUDIO2_VOICE_SENDS* sends, const XAUDIO2_EFFECT_CHAIN* effects) {
    auto* proxy = new XAudioTraceCallback(callback);
    const HRESULT result = g_originalXAudioCreateSourceVoice(
        engine, output, format, flags, maxRatio, proxy, sends, effects);
    if (FAILED(result) || !output || !*output) {
        delete proxy;
        return result;
    }
    IXAudio2SourceVoice* voice = *output;
    proxy->voice.store(voice);
    XAudioTraceVoice* record = nullptr;
    AcquireSRWLockExclusive(&g_xaudioTraceLock);
    for (auto& entry : g_xaudioTraceVoices) {
        IXAudio2SourceVoice* expected = nullptr;
        if (entry.voice.compare_exchange_strong(expected, voice)) {
            record = &entry;
            entry.callback = proxy;
            entry.sampleRate = format ? format->nSamplesPerSec : 0;
            entry.averageBytesPerSecond = format ? format->nAvgBytesPerSec : 0;
            entry.channels = format ? format->nChannels : 0;
            entry.lastVolumeBits.store(0xffffffffu);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_xaudioTraceLock);
    HookXAudioSourceVoice(voice);
    const bool associateDialogue = format && format->nChannels == 1 &&
        g_dialogueVoiceCreatePending.exchange(0, std::memory_order_acq_rel) != 0;
    if (associateDialogue) {
        g_dialogueSourceVoice.store(voice, std::memory_order_release);
        g_dialogueHeldVolumeBits.store(0, std::memory_order_release);
        Log("Voice continuation: dialogue XAudio source associated voice=%p rate=%lu channels=%u",
            voice, static_cast<unsigned long>(format->nSamplesPerSec), format->nChannels);
    }
    char detail[160]{};
    snprintf(detail, sizeof(detail), "rate=%lu avg-bytes=%lu channels=%u bits=%u callback=%p tracked=%d",
             static_cast<unsigned long>(format ? format->nSamplesPerSec : 0),
             static_cast<unsigned long>(format ? format->nAvgBytesPerSec : 0),
             format ? format->nChannels : 0, format ? format->wBitsPerSample : 0,
             static_cast<void*>(callback), record != nullptr);
    TraceXAudioEvent(voice, "create", MMM_XAUDIO_CALLER, detail);
    return result;
}

void STDMETHODCALLTYPE HookXAudioStopEngine(IXAudio2* engine) {
    if (AllowXAudioTraceEvent()) {
        char caller[160]{};
        DescribeXAudioCaller(MMM_XAUDIO_CALLER, caller, sizeof(caller));
        Log("XAudio2 engine=%p event=stop-engine caller=%s", engine, caller);
    }
    g_originalXAudioStopEngine(engine);
}

void HookXAudioEngine(IXAudio2* engine) {
    if (!engine) return;
    const bool sourceHooked = HookVtable(engine, 5, HookXAudioCreateSourceVoice,
                                         g_originalXAudioCreateSourceVoice);
    const bool stopHooked = HookVtable(engine, 9, HookXAudioStopEngine,
                                       g_originalXAudioStopEngine);
    Log("XAudio2 engine captured: engine=%p create-source-hook=%d stop-engine-hook=%d",
        engine, sourceHooked, stopHooked);
}

HRESULT WINAPI HookXAudio2Create(IXAudio2** output, UINT32 flags, XAUDIO2_PROCESSOR processor) {
    const HRESULT result = g_originalXAudio2Create(output, flags, processor);
    Log("XAudio2Create result=0x%08lx engine=%p flags=0x%x processor=0x%x",
        static_cast<unsigned long>(result), output ? *output : nullptr, flags, processor);
    if (SUCCEEDED(result) && output) HookXAudioEngine(*output);
    return result;
}

HRESULT WINAPI HookXAudio2CreateWithVersionInfo(IXAudio2** output, UINT32 flags,
                                                 XAUDIO2_PROCESSOR processor, DWORD version) {
    const HRESULT result = g_originalXAudio2CreateWithVersionInfo(output, flags, processor, version);
    Log("XAudio2CreateWithVersionInfo result=0x%08lx engine=%p flags=0x%x processor=0x%x version=%lu",
        static_cast<unsigned long>(result), output ? *output : nullptr, flags, processor,
        static_cast<unsigned long>(version));
    if (SUCCEEDED(result) && output) HookXAudioEngine(*output);
    return result;
}

FARPROC WINAPI HookXAudioGetProcAddress(HMODULE module, LPCSTR name) {
    FARPROC result = g_originalXAudioGetProcAddress(module, name);
    if (reinterpret_cast<uintptr_t>(name) <= 0xffff) return result;
    if (!strcmp(name, "XAudio2Create") && result) {
        g_originalXAudio2Create = reinterpret_cast<XAudio2CreateFn>(result);
        Log("XAudio2 trace intercepted XAudio2Create lookup: module=%p target=%p", module, result);
        return reinterpret_cast<FARPROC>(HookXAudio2Create);
    }
    if (!strcmp(name, "XAudio2CreateWithVersionInfo") && result) {
        g_originalXAudio2CreateWithVersionInfo =
            reinterpret_cast<XAudio2CreateWithVersionInfoFn>(result);
        Log("XAudio2 trace intercepted XAudio2CreateWithVersionInfo lookup: module=%p target=%p",
            module, result);
        return reinterpret_cast<FARPROC>(HookXAudio2CreateWithVersionInfo);
    }
    return result;
}

void InstallXAudioTrace(HMODULE module, const wchar_t* path) {
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    if (_wcsicmp(name, L"MyMerryMay_WIN_Master.dll") &&
        _wcsicmp(name, L"MyMerryMaybe_WIN_Master.dll") &&
        _wcsicmp(name, L"MyMerryMaySel_WIN_Master.dll")) return;
    bool hooked = false;
    for (const char* dll : {"kernel32.dll", "kernelbase.dll"}) {
        hooked |= ReplaceImport(module, dll, "GetProcAddress",
                                reinterpret_cast<void*>(HookXAudioGetProcAddress),
                                reinterpret_cast<void**>(&g_originalXAudioGetProcAddress));
    }
    static HMODULE loggedModules[3]{};
    const unsigned slot = wcsstr(name, L"Maybe") ? 1 : (wcsstr(name, L"Sel") ? 2 : 0);
    if (loggedModules[slot] != module) {
        loggedModules[slot] = module;
        Log("XAudio2 trace GetProcAddress hook: game=%s installed=%d module=%p",
            slot == 1 ? "Maybe" : (slot == 2 ? "Selector" : "May"), hooked, module);
    }
}

#undef MMM_XAUDIO_CALLER
