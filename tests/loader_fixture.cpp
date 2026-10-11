#include <d3d11.h>
#include <dxgi.h>
extern "C" void* __imp_D3D11CreateDevice;
extern "C" void* __imp_CreateDXGIFactory1;
extern "C" __declspec(dllexport) void* ImportedDeviceCreate() {
    return __imp_D3D11CreateDevice;
}
extern "C" __declspec(dllexport) void* ImportedFactoryCreate() {
    return __imp_CreateDXGIFactory1;
}
