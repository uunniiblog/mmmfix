#include "../src/mod.cpp"
#include <cassert>

HRESULT STDMETHODCALLTYPE FinishForTest(ID3D11DeviceContext*, BOOL, ID3D11CommandList**) { return S_OK; }
void STDMETHODCALLTYPE ChangedDrawForTest(ID3D11DeviceContext*, UINT, UINT) {}
void STDMETHODCALLTYPE BindVerticesForTest(ID3D11DeviceContext* context, UINT, UINT,
    ID3D11Buffer* const*, const UINT*, const UINT*) {
    auto table = *reinterpret_cast<void***>(context);
    table[13] = reinterpret_cast<void*>(ChangedDrawForTest);
}

int main() {
    using namespace mmmfix;
    g_logging.store(false);
    g_nativeWindows = true;
    void* tableA[115]{};
    void* tableB[115]{};
    void** objectA = tableA;
    void** objectB = tableB;
    auto* a = reinterpret_cast<ID3D11DeviceContext*>(&objectA);
    auto* b = reinterpret_cast<ID3D11DeviceContext*>(&objectB);
    g_contextHooks[0].vtable.store(tableA);
    g_contextHooks[1].vtable.store(tableB);
    {
        SceneScope scope(a);
        g_nvlUiSeen.store(true);
        g_messageUiSeen.store(true);
        TextureWidth(0).store(4096);
    }
    {
        SceneScope scope(b);
        assert(!g_nvlUiSeen.load());
        assert(TextureWidth(0).load() == 0);
        g_systemUiSeen.store(true);
    }
    // Present's global resets must not change a deferred recording's state.
    // NVL reveal uses a context without a panel, while completed text can use
    // another context with UI. Both must choose the same native correction.
    g_engineNvlLayout.store(true);
    {
        SceneScope scope(a);
        assert(UseMayNvlCorrection());
    }
    {
        SceneScope scope(b);
        assert(!g_messageUiSeen.load() && !g_nvlUiSeen.load());
        assert(UseMayNvlCorrection());
        g_engineNvlLayout.store(false);
        assert(!UseMayNvlCorrection()); // ADV/unknown still requires panel cues.
        g_engineNvlLayout.store(true);
    }
    g_nvlUiSeen.store(false);
    {
        SceneScope scope(a);
        assert(g_nvlUiSeen.load());
        assert(!g_systemUiSeen.load());
        assert(TextureWidth(0).load() == 4096);
    }
    tableA[8] = reinterpret_cast<void*>(HookPSSetShaderResources);
    tableA[12] = reinterpret_cast<void*>(HookDrawIndexed);
    tableA[13] = reinterpret_cast<void*>(HookDraw);
    tableA[14] = reinterpret_cast<void*>(HookMap);
    tableA[15] = reinterpret_cast<void*>(HookUnmap);
    tableA[33] = reinterpret_cast<void*>(HookOMSetRenderTargets);
    tableA[44] = reinterpret_cast<void*>(HookRSSetViewports);
    tableA[114] = reinterpret_cast<void*>(HookFinishCommandList);
    g_contextHooks[0].finishCommandList = FinishForTest;
    assert(SUCCEEDED(HookFinishCommandList(a, TRUE, nullptr)));
    {
        SceneScope scope(a);
        assert(!g_nvlUiSeen.load());
        assert(!g_messageUiSeen.load());
    }
    {
        SceneScope scope(b);
        assert(g_systemUiSeen.load());
    }
    // Simulate native runtime dispatch specialization after binding glyph VB.
    g_contextHooks[0].iaSetVertexBuffers = BindVerticesForTest;
    HookIASetVertexBuffers(a, 0, 0, nullptr, nullptr, nullptr);
    assert(tableA[13] == reinterpret_cast<void*>(HookDraw));
    assert(g_contextHooks[0].draw == ChangedDrawForTest);
    // Wine bypasses context routing and retains the original global state.
    g_nativeWindows = false;
    g_messageUiSeen.store(false);
    assert(UseMayNvlCorrection()); // Shared cue selects Wine's existing shader path.
    g_engineNvlLayout.store(false);
    assert(!UseMayNvlCorrection());
    g_messageUiSeen.store(true);
    g_nvlUiSeen.store(true);
    {
        SceneScope scope(a);
        assert(g_nvlUiSeen.load());
        g_nvlUiSeen.store(false);
    }
    assert(!g_nvlUiSeen.load());
    std::puts("Scene isolation, command-list reset, vertex-binding dispatch refresh, and Wine fallback passed");
}

