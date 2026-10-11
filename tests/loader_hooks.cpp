#include "../src/mod.cpp"
#include <cassert>
int main() {
    using namespace mmmfix;
    g_logging.store(false);
    g_nativeWindows = true;
    g_wrapEnabled = true; // Unsupported fixture must skip the experimental hook.
    // The inert fixture has the game's module name and graphics imports.
    HMODULE fixture = HookLoadLibraryW(L"loader-fixture/MyMerryMay_WIN_Master.dll");
    assert(fixture);
    using Query = void* (*)();
    auto device = reinterpret_cast<Query>(GetProcAddress(fixture, "ImportedDeviceCreate"));
    auto factory = reinterpret_cast<Query>(GetProcAddress(fixture, "ImportedFactoryCreate"));
    assert(device && factory);
    assert(device() == reinterpret_cast<void*>(HookD3D11CreateDevice));
    assert(factory() == reinterpret_cast<void*>(HookCreateDXGIFactory1));
    assert(g_originalD3D11CreateDevice && g_originalCreateDXGIFactory1);
    // Repeated patching must preserve the real forwarding targets.
    const auto original = g_originalD3D11CreateDevice;
    PatchLoadedGameModule(fixture);
    assert(g_originalD3D11CreateDevice == original);
    FreeLibrary(fixture);
    std::puts("Loader patches graphics imports before returning; forwarding targets preserved");
}
