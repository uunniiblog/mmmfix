#define DIRECTINPUT_VERSION 0x0800
#define INITGUID
#include <windows.h>
#include <dinput.h>

#include <cstdio>

int main() {
    HMODULE proxy = LoadLibraryW(L"dinput8.dll");
    if (!proxy) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    auto create = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(proxy, "DirectInput8Create"));
    if (!create) {
        std::printf("GetProcAddress failed: %lu\n", GetLastError());
        return 2;
    }
    IDirectInput8W* input = nullptr;
    const HRESULT result = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                  IID_IDirectInput8W, reinterpret_cast<void**>(&input), nullptr);
    std::printf("DirectInput8Create returned 0x%08lx\n", static_cast<unsigned long>(result));
    if (input) input->Release();
    FreeLibrary(proxy);
    return FAILED(result) ? 3 : 0;
}
