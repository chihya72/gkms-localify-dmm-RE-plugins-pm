#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <map>
#include <variant>
#include <memory>
#include <type_traits>
#include <vector>
#include <algorithm>

// ABI declarations correspond to NVIDIA/DLSS include/nvsdk_ngx_params.h and
// nvsdk_ngx_defs.h (NGX 1.5). This bridge owns parameter objects. Preserve the
// overload order: MSVC emits a different vtable than manually guessed slots.
namespace ngx {
using Result = unsigned int;
struct Parameters {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual Result Get(const char*, unsigned long long*) const = 0;
    virtual Result Get(const char*, float*) const = 0;
    virtual Result Get(const char*, double*) const = 0;
    virtual Result Get(const char*, unsigned int*) const = 0;
    virtual Result Get(const char*, int*) const = 0;
    virtual Result Get(const char*, ID3D11Resource**) const = 0;
    virtual Result Get(const char*, ID3D12Resource**) const = 0;
    virtual Result Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};
struct Handle;
// Local parameter map, following RenoDX's OwnedParameters design. Numeric
// getters accept the different NGX numeric types; missing keys stay missing.
class OwnedParameters final : public Parameters {
#ifdef GKMS_NR_TEST_MAIN
public:
    mutable std::map<std::string, unsigned> readCounts;
private:
#endif
    using Value = std::variant<unsigned long long, long long, double, void*>;
    std::map<std::string, Value, std::less<>> values;
    template<class T> void Store(const char* name, T value) {
        if (!name) return;
        if constexpr (std::is_pointer_v<T>) values[name] = static_cast<void*>(value);
        else if constexpr (std::is_floating_point_v<T>) values[name] = static_cast<double>(value);
        else if constexpr (std::is_signed_v<T>) values[name] = static_cast<long long>(value);
        else values[name] = static_cast<unsigned long long>(value);
    }
    template<class T> Result Load(const char* name, T* output) const {
        if (!name || !output) return 0xBAD00005;
#ifdef GKMS_NR_TEST_MAIN
        ++readCounts[name];
#endif
        const auto entry = values.find(name); if (entry == values.end()) return 0xBAD0000A;
        return std::visit([&](auto value) -> Result {
            using V = decltype(value);
            if constexpr (std::is_pointer_v<T> && std::is_pointer_v<V>) { *output = static_cast<T>(value); return 1; }
            else if constexpr (!std::is_pointer_v<T> && !std::is_pointer_v<V>) { *output = static_cast<T>(value); return 1; }
            else return 0xBAD0000A;
        }, entry->second);
    }
public:
    void Set(const char* n, unsigned long long v) override { Store(n,v); }
    void Set(const char* n, float v) override { Store(n,v); }
    void Set(const char* n, double v) override { Store(n,v); }
    void Set(const char* n, unsigned v) override { Store(n,v); }
    void Set(const char* n, int v) override { Store(n,v); }
    void Set(const char* n, ID3D11Resource* v) override { Store(n,v); }
    void Set(const char* n, ID3D12Resource* v) override { Store(n,v); }
    void Set(const char* n, void* v) override { Store(n,v); }
    Result Get(const char* n, unsigned long long* v) const override { return Load(n,v); }
    Result Get(const char* n, float* v) const override { return Load(n,v); }
    Result Get(const char* n, double* v) const override { return Load(n,v); }
    Result Get(const char* n, unsigned* v) const override { return Load(n,v); }
    Result Get(const char* n, int* v) const override { return Load(n,v); }
    Result Get(const char* n, ID3D11Resource** v) const override { return Load(n,v); }
    Result Get(const char* n, ID3D12Resource** v) const override { return Load(n,v); }
    Result Get(const char* n, void** v) const override { return Load(n,v); }
    void Reset() override { values.clear(); }
};
inline void Check(Result result, const char* operation) {
    if (result == 1) return;
    char message[256]; sprintf_s(message, "%s: NGX result 0x%08X", operation, result);
    throw std::runtime_error(message);
}
template<class T> T Export(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(std::string("Missing NGX export: ") + name);
    return reinterpret_cast<T>(address);
}
inline Result __cdecl ScalingRatio(Parameters* parameters) {
    if (!parameters) return 0xBAD00005;
    parameters->Set("DLSSNR.ScalingRatio", 1.0f); return 1;
}
class Runtime {
    struct ModuleLifetime {
        unsigned users{};
        // Direct NR Shutdown1 invalidates other features in the same DLL even
        // when they were created on another private device. Keep those devices
        // alive and shut the module down only after its final session retires.
        std::vector<ID3D12Device*> devices;
    };
    inline static std::map<HMODULE, ModuleLifetime> lifetimes;
    HMODULE module{}, caller{};
    bool initialized{};
    ID3D12Device* device{};
    using CallInit = Result(__cdecl*)(FARPROC, unsigned long long, const wchar_t*, ID3D12Device*, unsigned, const Parameters*);
    using CallCreate = Result(__cdecl*)(FARPROC, ID3D12GraphicsCommandList*, unsigned, const Parameters*, Handle**);
    using CallEvaluate = Result(__cdecl*)(FARPROC, ID3D12GraphicsCommandList*, const Handle*, const Parameters*, void*);
    using CallRelease = Result(__cdecl*)(FARPROC, Handle*);
    using CallShutdown = Result(__cdecl*)(FARPROC, ID3D12Device*);
    FARPROC create{}, evaluate{}, release{}, shutdown{};
    CallCreate callCreate{}; CallEvaluate callEvaluate{}; CallRelease callRelease{}; CallShutdown callShutdown{};
    OwnedParameters owned;
public:
    Parameters* parameters{};
    Handle* handle{};
    Runtime() = default;
    Runtime(const Runtime&) = delete;
    ~Runtime() {
        ResetFeature();
        if (initialized && shutdown) {
            auto lifetime = lifetimes.find(module);
            if (lifetime != lifetimes.end() && --lifetime->second.users == 0) {
                callShutdown(shutdown, device);
                for (auto retained : lifetime->second.devices) retained->Release();
                lifetimes.erase(lifetime);
            }
        }
        if (module) FreeLibrary(module);
        if (caller) FreeLibrary(caller);
    }
    void Initialize(ID3D12Device* target, const std::wstring& directory, const std::wstring& storage) {
        const auto snippet = std::filesystem::path(directory) / L"nvngx_dlssnr.dll";
        module = LoadLibraryExW(snippet.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw std::runtime_error("Cannot load configured NR runtime");
        HMODULE owner{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ScalingRatio), &owner)) throw std::runtime_error("Cannot locate NR bridge module");
        wchar_t path[32768]{};
        auto length = GetModuleFileNameW(owner, path, 32768);
        if (!length || length == 32768) throw std::runtime_error("Cannot resolve NR bridge path");
        const auto helper = std::filesystem::path(path).parent_path() / L"nvngx.dll_dlssnr.dll";
        caller = LoadLibraryExW(helper.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!caller) throw std::runtime_error("NR caller helper missing beside native bridge");
        auto init = Export<FARPROC>(module, "NVSDK_NGX_D3D12_Init_Ext");
        create = Export<FARPROC>(module, "NVSDK_NGX_D3D12_CreateFeature"); evaluate = Export<FARPROC>(module, "NVSDK_NGX_D3D12_EvaluateFeature");
        release = Export<FARPROC>(module, "NVSDK_NGX_D3D12_ReleaseFeature"); shutdown = Export<FARPROC>(module, "NVSDK_NGX_D3D12_Shutdown1");
        const auto callInit = Export<CallInit>(caller, "GkmsNrCallInit");
        callCreate = Export<CallCreate>(caller, "GkmsNrCallCreate"); callEvaluate = Export<CallEvaluate>(caller, "GkmsNrCallEvaluate");
        callRelease = Export<CallRelease>(caller, "GkmsNrCallRelease"); callShutdown = Export<CallShutdown>(caller, "GkmsNrCallShutdown");
        device = target;
        auto& lifetime = lifetimes[module];
        if (std::find(lifetime.devices.begin(), lifetime.devices.end(), device) == lifetime.devices.end()) {
            Check(callInit(init, 0x876232cULL, storage.c_str(), device, 0x15, nullptr), "Direct NR initialize");
            lifetime.devices.push_back(device); device->AddRef();
        }
        ++lifetime.users;
        initialized = true; parameters = &owned;
    }
    void ResetFeature() { if (handle && release) callRelease(release, handle); handle = nullptr; }
    void CreateFeature(ID3D12GraphicsCommandList* list, unsigned width, unsigned height, unsigned preset) {
        auto p = parameters;
        for (const auto name : {"Width", "OutWidth", "DLSSNR.Width", "DLSSNR.InputWidth", "DLSSNR.OutputWidth", "DLSSNR.Output.Width"}) p->Set(name, width);
        for (const auto name : {"Height", "OutHeight", "DLSSNR.Height", "DLSSNR.InputHeight", "DLSSNR.OutputHeight", "DLSSNR.Output.Height"}) p->Set(name, height);
        p->Set("CreationNodeMask", 1u); p->Set("VisibilityNodeMask", 1u);
        p->Set("PerfQualityValue", 0); p->Set("DLSS.Feature.Create.Flags", 0);
        p->Set("DLSSNR.Hint.Render.Preset", static_cast<int>(preset));
        p->Set("DLSSNR.Upscaling", 0u); p->Set("DLSSNR.Scale", 1.0f); p->Set("DLSSNR.ScalingRatio", 1.0f);
        p->Set("DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&ScalingRatio));
        // Feature 18/NR keys come from experimental open-source integrations.
        Check(callCreate(create, list, 18, p, &handle), "Direct NR create feature 18");
        if (!handle) throw std::runtime_error("NGX returned an empty NR feature handle");
    }
    void EvaluateFeature(ID3D12GraphicsCommandList* list) { Check(callEvaluate(evaluate, list, handle, parameters, nullptr), "Direct NR evaluate"); }
};
}
