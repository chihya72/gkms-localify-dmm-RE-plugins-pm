#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

// The NR snippet checks its caller's module path for "nvngx.dll".
// This private forwarder is built as nvngx.dll_dlssnr.dll, following the
// independently published OptiScaler/NR media PoC approach. No import-table
// patch, driver hook or edit to the NVIDIA binary is needed.
#define EXPORT extern "C" __declspec(dllexport) __declspec(noinline)
using Result = unsigned;
EXPORT Result __cdecl GkmsNrCallInit(FARPROC target, unsigned long long app, const wchar_t* path,
    ID3D12Device* device, unsigned version, const void* parameters) {
    using Call = Result(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, unsigned, const void*);
    volatile auto result = reinterpret_cast<Call>(target)(app, path, device, version, parameters); return result;
}
EXPORT Result __cdecl GkmsNrCallCreate(FARPROC target, ID3D12GraphicsCommandList* list, unsigned feature,
    const void* parameters, void** handle) {
    using Call = Result(__cdecl*)(ID3D12GraphicsCommandList*, unsigned, const void*, void**);
    volatile auto result = reinterpret_cast<Call>(target)(list, feature, parameters, handle); return result;
}
EXPORT Result __cdecl GkmsNrCallEvaluate(FARPROC target, ID3D12GraphicsCommandList* list, const void* handle,
    const void* parameters, void* progress) {
    using Call = Result(__cdecl*)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
    volatile auto result = reinterpret_cast<Call>(target)(list, handle, parameters, progress); return result;
}
EXPORT Result __cdecl GkmsNrCallRelease(FARPROC target, void* handle) {
    using Call = Result(__cdecl*)(void*);
    volatile auto result = reinterpret_cast<Call>(target)(handle); return result;
}
EXPORT Result __cdecl GkmsNrCallShutdown(FARPROC target, ID3D12Device* device) {
    using Call = Result(__cdecl*)(ID3D12Device*);
    volatile auto result = reinterpret_cast<Call>(target)(device); return result;
}
