#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <climits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include "NgxApi.h"
#include "ColorPipeline.h"
#include "ExtendedShaders.h"
using Microsoft::WRL::ComPtr;
#define API extern "C" __declspec(dllexport)
namespace {
struct Configuration {
    uint32_t size, neural, preset, autoMask;
    float intensity, tone, structure, skin, motionX, motionY;
    uint32_t comparisonSplit;
    float modelScale = .5f, whitePoint = 1, exposureEV = 0, transferStrength = 1, colorStrength = .5f, maxRatio = 2;
    uint32_t codec = 0, autoExposure = 0, inputEncoding = 0, style = 0, capture = 0, flipY = 0;
    uint32_t downFilter=3, transferMode=1, applyModel=1, holdFrame=0, compareMode=2, compareSwap=0, compareLabels=0, debugView=0;
    float comparePosition=.5f, compareZoom=1, debugGain=4;
    uint32_t useDepth=1, depthInverted=1;
};
bool ValidConfiguration(const Configuration* options) {
    if (!options || options->size != sizeof(Configuration) || options->neural > 1 || options->autoMask > 1 || options->preset > 3 || options->comparisonSplit > 1) return false;
    for (auto v : {options->intensity, options->tone, options->structure})
        if (!std::isfinite(v) || v < 0 || v > 2) return false;
    if (!std::isfinite(options->skin) || options->skin < -1 || options->skin > 2) return false;
    if (!std::isfinite(options->modelScale) || options->modelScale < .25f || options->modelScale > 2 ||
        !std::isfinite(options->whitePoint) || options->whitePoint < .01f || options->whitePoint > 4096 ||
        !std::isfinite(options->exposureEV) || std::abs(options->exposureEV) > 4 ||
        !std::isfinite(options->transferStrength) || options->transferStrength < 0 || options->transferStrength > 2 ||
        !std::isfinite(options->colorStrength) || options->colorStrength < 0 || options->colorStrength > 4 ||
        !std::isfinite(options->maxRatio) || options->maxRatio < 1 || options->maxRatio > 8 ||
        options->codec > 4 || options->autoExposure > 1 || options->inputEncoding > 1 || options->style > 3 || options->capture > 1 || options->flipY > 1) return false;
    if(options->downFilter>3||options->transferMode>1||options->applyModel>1||options->holdFrame>1||options->compareMode<1||options->compareMode>2||options->compareSwap>1||options->compareLabels>1||options->debugView>3||options->useDepth>1||options->depthInverted>1)return false;
    if(!std::isfinite(options->comparePosition)||options->comparePosition<0||options->comparePosition>1||!std::isfinite(options->compareZoom)||options->compareZoom<1||options->compareZoom>2||!std::isfinite(options->debugGain)||options->debugGain<1||options->debugGain>32)return false;
    return std::isfinite(options->motionX) && std::isfinite(options->motionY) && std::abs(options->motionX) <= 4 && std::abs(options->motionY) <= 4;
}
void Check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    char message[256]; sprintf_s(message, "%s: HRESULT 0x%08X", operation, static_cast<unsigned>(result));
    throw std::runtime_error(message);
}
struct WinHandle { HANDLE value{}; ~WinHandle() { if (value) CloseHandle(value); } };
struct SharedTexture {
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
struct Job { uint64_t session; ComPtr<ID3D11Texture2D> color, motion; int frame; int64_t camera; ComPtr<ID3D11Texture2D> depth; };
DXGI_FORMAT ViewFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    default: return format;
    }
}
bool ColorFormat(DXGI_FORMAT format) {
    format = ViewFormat(format);
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R11G11B10_FLOAT ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM;
}
bool MotionFormat(DXGI_FORMAT format) {
    format = ViewFormat(format);
    return format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R32G32_FLOAT || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}
ComPtr<ID3DBlob> Compile(const char* source, const char* profile, const char* entry = "main") {
    ComPtr<ID3DBlob> code, error;
    auto result = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (FAILED(result) && error) throw std::runtime_error(static_cast<const char*>(error->GetBufferPointer()));
    Check(result, "Compile conversion shader"); return code;
}
void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; list->ResourceBarrier(1, &barrier);
}
struct Session {
    Configuration config;
    std::wstring runtimeDirectory, storage;
    std::string status = "Waiting for a matched scene frame (NR not active).";
    bool fault{}, gpuPending{};
    unsigned width{}, height{}, processed{}, workWidth{}, workHeight{};
    int lastFrame = -1;
    int64_t lastCamera{};
    uint64_t value{};
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> immediate;
    ComPtr<ID3D11DeviceContext> deferred;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D12Fence> fence12;
    WinHandle completion;
    SharedTexture input, motion, output;
    SharedTexture original, proxyFull, resolved;
    SharedTexture superInput, superOutput, superAdjusted, resizeScratch, depth;
    ComPtr<ID3D11Texture2D> heldColor, heldDepth;
    ComPtr<ID3D11ComputeShader> resizeShader, residualShader, depthShader;
    ComPtr<ID3D11PixelShader> displayShader;
    ComPtr<ID3D11Buffer> resizeConstants, displayConstants;
    bool depthActive{};
    std::string depthStatus="unavailable";
    ComPtr<ID3D11ComputeShader> pipelineShader, prepareShader;
    ComPtr<ID3D11Buffer> pipelineConstants;
    ComPtr<ID3D11SamplerState> linearSampler;
    ComPtr<ID3D11PixelShader> encodedPixelShader, encodedSplitShader;
    ComPtr<ID3D11Texture2D> meterTexture, meterReadback;
    ComPtr<ID3D11UnorderedAccessView> meterUav;
    float measuredWhitePoint = 1;
    bool exposureReady{}, captured{};
    unsigned visibleCaptureFrames{};
    std::string captureStatus;
    ComPtr<ID3D11ComputeShader> colorShader, motionShader;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader, splitShader;
    ComPtr<ID3D11ComputeShader> differenceShader;
    ComPtr<ID3D11Texture2D> differenceTexture, differenceReadback;
    ComPtr<ID3D11UnorderedAccessView> differenceUav;
    double differenceMean{}, differenceMax{}, differenceAboveStep{}, sampledInputMax{};
    std::string differenceStatus;
    ComPtr<ID3D11BlendState> rgbOnly;
    ComPtr<ID3D11DepthStencilState> noDepth;
    ComPtr<ID3D11RasterizerState> raster;
    std::unique_ptr<ngx::Runtime> runtime;
    Session(const Configuration& options, const wchar_t* directory, const wchar_t* data)
        : config(options), runtimeDirectory(directory ? directory : L""), storage(data ? data : L"") {}
    void Wait(uint64_t target) {
        const auto completed = fence12->GetCompletedValue();
        if (completed == UINT64_MAX) throw std::runtime_error("D3D12 device removed");
        if (completed < target) {
            Check(fence12->SetEventOnCompletion(target, completion.value), "Set completion event");
            if (WaitForSingleObject(completion.value, 5000) != WAIT_OBJECT_0)
                throw std::runtime_error("GPU completion timed out; processing stopped");
        }
        Check(device12->GetDeviceRemovedReason(), "D3D12 device status"); Check(device11->GetDeviceRemovedReason(), "D3D11 device status");
    }
    void Initialize(ID3D11Texture2D* source) {
        ComPtr<ID3D11Device> gameDevice; source->GetDevice(&gameDevice);
        Check(gameDevice.As(&device11), "D3D11 device5 interface");
        ComPtr<ID3D11DeviceContext> originalContext; gameDevice->GetImmediateContext(&originalContext);
        Check(originalContext.As(&immediate), "D3D11 context4 interface");
        Check(gameDevice->CreateDeferredContext(0, &deferred), "Create isolated conversion context");
        ComPtr<IDXGIDevice> dxgi; Check(gameDevice.As(&dxgi), "DXGI device");
        ComPtr<IDXGIAdapter> adapter; Check(dxgi->GetAdapter(&adapter), "Get game adapter");
        if (!device12) Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)), "Create private D3D12 device on same adapter");
        D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Create NR queue");
        Check(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create allocator");
        Check(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Create command list");
        Check(list->Close(), "Close initial command list");
        Check(device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)), "Create shared fence");
        WinHandle sharedFence;
        Check(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedFence.value), "Share fence");
        Check(device12->OpenSharedHandle(sharedFence.value, IID_PPV_ARGS(&fence12)), "Open shared fence");
        completion.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completion.value) throw std::runtime_error("Cannot create completion event");
        const char* colorCode = "Texture2D<float4> src:register(t0);RWTexture2D<float4> dst:register(u0);[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h;dst.GetDimensions(w,h);if(p.x<w&&p.y<h)dst[p.xy]=src.Load(int3(p.xy,0));}";
        const std::string motionTransform = config.neural && config.flipY ? "uv.y=1-uv.y;" : "";
        const std::string motionSign = config.neural && config.flipY ? "v.y=-v.y;" : "";
        const std::string motionCode = "Texture2D<float2> src:register(t0);SamplerState samp:register(s0);RWTexture2D<float2> dst:register(u0);[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h;dst.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;float2 uv=(p.xy+.5)/float2(w,h);" + motionTransform + "float2 v=src.SampleLevel(samp,uv,0);" + motionSign + "dst[p.xy]=v;}";
        const char* vertexCode = "float4 main(uint i:SV_VertexID):SV_POSITION{return float4((i==2?3:-1),(i==1?3:-1),0,1);}";
        const std::string outputCoordinates = "uint2 q=uint2(p.xy);" + std::string(config.neural && config.flipY ? "q.y=h-1-q.y;" : "");
        const std::string pixelCode = "Texture2D<float4> src:register(t0);float4 main(float4 p:SV_POSITION):SV_TARGET{uint w,h;src.GetDimensions(w,h);" + outputCoordinates + "return src.Load(int3(q,0));}";
        const std::string splitCode = "Texture2D<float4> enhanced:register(t0);Texture2D<float4> original:register(t1);float4 main(float4 p:SV_POSITION):SV_TARGET{uint w,h;original.GetDimensions(w,h);if(abs(p.x-w*.5)<max(2,w*.003))return float4(1,1,0,1);" + outputCoordinates + "return p.x<w*.5?original.Load(int3(q,0)):enhanced.Load(int3(q,0));}";
        auto c = Compile(colorCode, "cs_5_0"); Check(gameDevice->CreateComputeShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &colorShader), "Create color shader");
        c = Compile(motionCode.c_str(), "cs_5_0"); Check(gameDevice->CreateComputeShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &motionShader), "Create motion shader");
        c = Compile(vertexCode, "vs_5_0"); Check(gameDevice->CreateVertexShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &vertexShader), "Create vertex shader");
        c = Compile(pixelCode.c_str(), "ps_5_0"); Check(gameDevice->CreatePixelShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &pixelShader), "Create output shader");
        c = Compile(splitCode.c_str(), "ps_5_0"); Check(gameDevice->CreatePixelShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &splitShader), "Create comparison shader");
        InitializeColorPipeline();
        InitializeExtendedPipeline();
        D3D11_BLEND_DESC blend{}; blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        Check(gameDevice->CreateBlendState(&blend, &rgbOnly), "Create RGB blend state");
        D3D11_DEPTH_STENCIL_DESC depthState{}; depthState.DepthEnable = FALSE; depthState.DepthFunc = D3D11_COMPARISON_ALWAYS;
        Check(gameDevice->CreateDepthStencilState(&depthState, &noDepth), "Create depth state");
        D3D11_RASTERIZER_DESC rs{}; rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
        Check(gameDevice->CreateRasterizerState(&rs, &raster), "Create raster state");
        if (config.neural) {
            if (!std::filesystem::is_regular_file(std::filesystem::path(runtimeDirectory) / L"nvngx_dlssnr.dll"))
                throw std::runtime_error("nvngx_dlssnr.dll not found in configured directory");
            runtime = std::make_unique<ngx::Runtime>(); runtime->Initialize(device12.Get(), runtimeDirectory, storage);
        }
    }
    SharedTexture MakeShared(DXGI_FORMAT format, unsigned textureWidth = 0, unsigned textureHeight = 0) {
        // This driver rejects opening D3D12-created UAV textures in D3D11.
        // Create on the game's API and import the NT handle into the model API.
        SharedTexture result;
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = textureWidth ? textureWidth : width; desc.Height = textureHeight ? textureHeight : height; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        Check(device11->CreateTexture2D(&desc, nullptr, &result.texture), "Create DX11 shared texture");
        ComPtr<IDXGIResource1> dxgi; Check(result.texture.As(&dxgi), "DXGI shared texture interface");
        WinHandle shared;
        Check(dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared.value), "Share DX11 texture");
        Check(device12->OpenSharedHandle(shared.value, IID_PPV_ARGS(&result.resource)), "Import shared texture into DX12");
        Check(device11->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv), "Create shared SRV");
        Check(device11->CreateUnorderedAccessView(result.texture.Get(), nullptr, &result.uav), "Create shared UAV"); return result;
    }
    ComPtr<ID3D11ShaderResourceView> SourceView(ID3D11Texture2D* texture) {
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = ViewFormat(desc.Format);
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> result; Check(device11->CreateShaderResourceView(texture, &view, &result), "Create game texture SRV"); return result;
    }
    void ExecuteDeferred() {
        ComPtr<ID3D11CommandList> commands; Check(deferred->FinishCommandList(FALSE, &commands), "Finish conversion commands");
        gpuPending = true; immediate->ExecuteCommandList(commands.Get(), TRUE);
    }
    void Convert(ID3D11ShaderResourceView* source, SharedTexture& target, ID3D11ComputeShader* shader) {
        deferred->CSSetShader(shader, nullptr, 0); deferred->CSSetShaderResources(0, 1, &source);
        auto sampler = linearSampler.Get(); deferred->CSSetSamplers(0, 1, &sampler);
        auto destination = target.uav.Get(); deferred->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
        D3D11_TEXTURE2D_DESC size{}; target.texture->GetDesc(&size);
        deferred->Dispatch((size.Width + 7) / 8, (size.Height + 7) / 8, 1);
        ID3D11ShaderResourceView* noSrv{}; ID3D11UnorderedAccessView* noUav{};
        deferred->CSSetShaderResources(0, 1, &noSrv); deferred->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    }
    void Composite(ID3D11Texture2D* destination) {
        D3D11_TEXTURE2D_DESC desc{}; destination->GetDesc(&desc);
        D3D11_RENDER_TARGET_VIEW_DESC view{}; view.Format = ViewFormat(desc.Format); view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> target; Check(device11->CreateRenderTargetView(destination, &view, &target), "Create output RTV");
        deferred->ClearState(); auto rt = target.Get(); deferred->OMSetRenderTargets(1, &rt, nullptr);
        deferred->OMSetBlendState(rgbOnly.Get(), nullptr, UINT_MAX); deferred->OMSetDepthStencilState(noDepth.Get(), 0);
        deferred->RSSetState(raster.Get()); D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        deferred->RSSetViewports(1, &viewport); deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        deferred->VSSetShader(vertexShader.Get(), nullptr, 0);
        auto pixel = runtime ? displayShader.Get() : pixelShader.Get();
        deferred->PSSetShader(pixel, nullptr, 0);
        if(runtime) {
            struct DisplayConstants { float split,zoom,gain,white; uint32_t mode,swap,debug,apply,flip,encoded,labels,pad; };
            const DisplayConstants c{config.comparePosition,config.compareZoom,config.debugGain,WhitePoint(),config.comparisonSplit?config.compareMode:0,config.compareSwap,config.debugView,config.applyModel,config.flipY,config.inputEncoding,config.compareLabels,0};
            deferred->UpdateSubresource(displayConstants.Get(),0,nullptr,&c,0,0);auto cb=displayConstants.Get();deferred->PSSetConstantBuffers(0,1,&cb);
            ID3D11ShaderResourceView* views[]{original.srv.Get(),resolved.srv.Get(),input.srv.Get(),output.srv.Get()};deferred->PSSetShaderResources(0,4,views);
            auto sampler=linearSampler.Get();deferred->PSSetSamplers(0,1,&sampler);
        } else {auto sourceView=output.srv.Get();deferred->PSSetShaderResources(0,1,&sourceView);}
        deferred->Draw(3, 0); ExecuteDeferred();
    }
    void MeasureDifference() {
        if (!differenceShader) {
            const char* code = "Texture2D<float4> a:register(t0);Texture2D<float4> b:register(t1);RWTexture2D<float4> dst:register(u0);[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h;a.GetDimensions(w,h);uint2 q=min(uint2((p.xy+.5)*float2(w,h)/64),uint2(w-1,h-1));float3 original=a.Load(int3(q,0)).rgb;float3 output=b.Load(int3(q,0)).rgb;dst[p.xy]=float4(abs(original-output),max(original.r,max(original.g,original.b)));}";
            auto shader = Compile(code, "cs_5_0");
            Check(device11->CreateComputeShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr, &differenceShader), "Create difference sampler");
            D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            Check(device11->CreateTexture2D(&desc, nullptr, &differenceTexture), "Create difference samples");
            Check(device11->CreateUnorderedAccessView(differenceTexture.Get(), nullptr, &differenceUav), "Create difference UAV");
            desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Check(device11->CreateTexture2D(&desc, nullptr, &differenceReadback), "Create difference readback");
        }
        deferred->ClearState(); deferred->CSSetShader(differenceShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* sources[]{runtime ? original.srv.Get() : input.srv.Get(), runtime ? resolved.srv.Get() : output.srv.Get()};
        deferred->CSSetShaderResources(0, 2, sources); auto uav = differenceUav.Get();
        deferred->CSSetUnorderedAccessViews(0, 1, &uav, nullptr); deferred->Dispatch(8, 8, 1);
        deferred->ClearState(); deferred->CopyResource(differenceReadback.Get(), differenceTexture.Get()); ExecuteDeferred();
        const auto done = ++value; Check(immediate->Signal(fence11.Get(), done), "Signal difference samples");
        immediate->Flush(); Wait(done); gpuPending = false;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(immediate->Map(differenceReadback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map difference samples");
        double sum{}, maximum{}, above{}, inputMaximum{}; bool finite = true;
        for (unsigned y = 0; y < 64; ++y) {
            const auto row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
            for (unsigned x = 0; x < 64; ++x) {
                for (unsigned c = 0; c < 3; ++c) {
                    const double delta = row[x*4+c]; finite = finite && std::isfinite(delta);
                    sum += delta; maximum = (std::max)(maximum, delta); if (delta > 1. / 255.) ++above;
                }
                inputMaximum = (std::max)(inputMaximum, static_cast<double>(row[x*4+3]));
            }
        }
        immediate->Unmap(differenceReadback.Get(), 0);
        differenceMean = sum / (64*64*3); differenceMax = maximum; differenceAboveStep = 100. * above / (64*64*3); sampledInputMax = inputMaximum;
        differenceStatus = finite ? ", delta_mean=" + std::to_string(differenceMean) + ", delta_max=" + std::to_string(differenceMax) +
            ", delta_gt1_255_pct=" + std::to_string(differenceAboveStep) + ", input_sample_max=" + std::to_string(sampledInputMax) : ", delta=NON_FINITE";
        differenceStatus += ", delta_frame=" + std::to_string(processed);
    }
#include "ColorPipeline.inl"
#include "ExtendedPipeline.inl"
    void Process(const Job& job) {
        const auto started = std::chrono::steady_clock::now();
        D3D11_TEXTURE2D_DESC colorDesc{}, motionDesc{}; job.color->GetDesc(&colorDesc); job.motion->GetDesc(&motionDesc);
        if (!ColorFormat(colorDesc.Format) || !MotionFormat(motionDesc.Format)) throw std::runtime_error("Unsupported color or motion format; frame unchanged");
        if (colorDesc.Width != motionDesc.Width || colorDesc.Height != motionDesc.Height) throw std::runtime_error("Color/motion resolution mismatch; frame unchanged");
        if (colorDesc.SampleDesc.Count != 1 || motionDesc.SampleDesc.Count != 1 || colorDesc.ArraySize != 1 || motionDesc.ArraySize != 1)
            throw std::runtime_error("MSAA/array texture unsupported; frame unchanged");
        if (!(colorDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || !(motionDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
            (config.neural && !(colorDesc.BindFlags & D3D11_BIND_RENDER_TARGET))) throw std::runtime_error("Texture bindings unavailable; frame unchanged");
        ComPtr<ID3D11Device> colorDevice, motionDevice; job.color->GetDevice(&colorDevice); job.motion->GetDevice(&motionDevice);
        if (colorDevice.Get() != motionDevice.Get()) throw std::runtime_error("Textures belong to different devices");
        if (!device11) Initialize(job.color.Get());
        ComPtr<ID3D11Device> sessionDevice; Check(device11.As(&sessionDevice), "Session device identity");
        if (sessionDevice.Get() != colorDevice.Get()) throw std::runtime_error("Graphics device changed; restart required");
        bool reset = lastFrame < 0 || lastCamera != job.camera || job.frame != lastFrame + 1;
        if (width != colorDesc.Width || height != colorDesc.Height) {
            if (runtime) runtime->ResetFeature(); width = colorDesc.Width; height = colorDesc.Height;
            workWidth = runtime ? (std::max)(1u, static_cast<unsigned>(width * config.modelScale + .5f)) : width;
            workHeight = runtime ? (std::max)(1u, static_cast<unsigned>(height * config.modelScale + .5f)) : height;
            if(workWidth>16384||workHeight>16384)throw std::runtime_error("Model resolution exceeds texture limits");
            input = MakeShared(DXGI_FORMAT_R16G16B16A16_FLOAT, workWidth, workHeight); motion = MakeShared(DXGI_FORMAT_R16G16_FLOAT, workWidth, workHeight);
            output = MakeShared(DXGI_FORMAT_R16G16B16A16_FLOAT, workWidth, workHeight);
            if (runtime) { original = MakeShared(DXGI_FORMAT_R16G16B16A16_FLOAT); proxyFull = MakeShared(DXGI_FORMAT_R16G16B16A16_FLOAT); resolved = MakeShared(DXGI_FORMAT_R16G16B16A16_FLOAT); }
            depth={};superInput={};superOutput={};superAdjusted={};resizeScratch={};
            if(runtime&&config.modelScale>1){superInput=MakeShared(DXGI_FORMAT_R16G16B16A16_FLOAT);superOutput=MakeShared(DXGI_FORMAT_R16G16B16A16_FLOAT);superAdjusted=MakeShared(DXGI_FORMAT_R16G16B16A16_FLOAT);resizeScratch=MakeShared(DXGI_FORMAT_R16G16B16A16_FLOAT,width,workHeight);}
            reset = true;
        }
        deferred->ClearState();
        if(runtime)PrepareHeldFrame(job);
        auto colorSrv = SourceView(runtime&&config.holdFrame?heldColor.Get():job.color.Get()); auto motionSrv = SourceView(job.motion.Get());
        if (runtime) PrepareModelInput(colorSrv.Get(), reset);
        else Convert(colorSrv.Get(), input, colorShader.Get());
        if(runtime&&config.holdFrame){const float zero[4]{};deferred->ClearUnorderedAccessViewFloat(motion.uav.Get(),zero);}
        else Convert(motionSrv.Get(), motion, motionShader.Get());
        const bool previousDepth=depthActive;
        if(runtime)PrepareDepth(config.holdFrame?heldDepth.Get():job.depth.Get());
        if(previousDepth!=depthActive)reset=true;
        ExecuteDeferred();
        const auto from11 = ++value; Check(immediate->Signal(fence11.Get(), from11), "Signal input ready"); immediate->Flush();
        Check(queue->Wait(fence12.Get(), from11), "Wait for game inputs");
        Check(allocator->Reset(), "Reset allocator"); Check(list->Reset(allocator.Get(), nullptr), "Reset NR list");
        if (runtime) {
            Barrier(list.Get(), input.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(list.Get(), motion.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            auto p = runtime->parameters;
            p->Set("DLSSNR.Color", input.resource.Get()); p->Set("DLSSNR.MVec", motion.resource.Get()); p->Set("DLSSNR.Output", output.resource.Get());
            p->Set("DLSSNR.Depth", depthActive?depth.resource.Get():static_cast<ID3D12Resource*>(nullptr));
            p->Set("DLSSNR.DepthInverted",config.depthInverted);
            p->Set("DLSSNR.DepthSubrectBaseX",0u);p->Set("DLSSNR.DepthSubrectBaseY",0u);
            p->Set("DLSSNR.DepthSubrectWidth",depthActive?workWidth:0u);p->Set("DLSSNR.DepthSubrectHeight",depthActive?workHeight:0u);
            if(depthActive)Barrier(list.Get(),depth.resource.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            for (const auto name : {"Color", "MVec", "Output"}) {
                const auto prefix = std::string("DLSSNR.") + name + "Subrect";
                p->Set((prefix + "BaseX").c_str(), 0u); p->Set((prefix + "BaseY").c_str(), 0u);
                p->Set((prefix + "Width").c_str(), workWidth); p->Set((prefix + "Height").c_str(), workHeight);
            }
            p->Set("DLSSNR.MVecScaleX", config.motionX * static_cast<float>(workWidth)); p->Set("DLSSNR.MVecScaleY", config.motionY * static_cast<float>(workHeight));
            p->Set("DLSSNR.Reset", reset ? 1u : 0u); p->Set("DLSSNR.Enabled", 1u);
            p->Set("DLSSNR.Intensity", config.intensity); p->Set("DLSSNR.LocalToneStrength", config.tone);
            p->Set("DLSSNR.LocalStructureStrength", config.structure); p->Set("DLSSNR.SkinStructureStrength", config.skin); p->Set("DLSSNR.UseAutoMask", config.autoMask);
            p->Set("DLSSNR.Style", config.style); p->Set("DLSSNR.UICorrection", 0u);
            // Runtime tuning is latched at creation; populate it before CreateFeature.
            if (!runtime->handle) runtime->CreateFeature(list.Get(), workWidth, workHeight, config.preset);
            runtime->EvaluateFeature(list.Get());
            if(depthActive)Barrier(list.Get(),depth.resource.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
            Barrier(list.Get(), input.resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(list.Get(), motion.resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        } else {
            Barrier(list.Get(), input.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(output.resource.Get(), input.resource.Get());
            Barrier(list.Get(), input.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        }
        Check(list->Close(), "Close NR list"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
        const auto from12 = ++value; Check(queue->Signal(fence12.Get(), from12), "Signal output ready");
        Check(immediate->Wait(fence11.Get(), from12), "Wait for NR output");
        if (runtime) {
            ResolveModelOutput();
            // A zero transfer is an exact bypass, not an approximate round trip.
            if (config.transferStrength > 0 || config.comparisonSplit || config.holdFrame || config.debugView) Composite(job.color.Get());
        }
        const auto finished = ++value; Check(immediate->Signal(fence11.Get(), finished), "Signal frame completion"); immediate->Flush();
        Wait(finished); gpuPending = false; ++processed; lastFrame = job.frame; lastCamera = job.camera;
        // Sample original/resolved colors, excluding the comparison stripe.
        // Any UI already merged into the scene can still be present in the samples.
        // Only 64x64 points on the first/every 300th frame while split mode is on.
        if (runtime && config.comparisonSplit && (processed == 1 || processed % 300 == 0)) MeasureDifference();
        if (runtime && config.capture && !captured && processed % 30 == 0) CaptureFrame(job.frame);
        status = std::string(runtime ? "NR evaluated (visual result unverified): " : "DIAGNOSTIC ONLY, picture unchanged: ") +
            std::to_string(width) + "x" + std::to_string(height) + ", GPU frames=" + std::to_string(processed) +
            ", bridge_ms=" + std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()) +
            (runtime ? ", model=" + std::to_string(workWidth) + "x" + std::to_string(workHeight) + ", white_point=" + std::to_string(WhitePoint()) + ", codec=" + std::to_string(config.codec) + ", flip_y=" + std::to_string(config.flipY)+", depth="+depthStatus+", held="+std::to_string(config.holdFrame)+", apply="+std::to_string(config.applyModel)+", debug="+std::to_string(config.debugView) : "") +
            (runtime && config.comparisonSplit ? std::string(", compare=")+(config.compareMode==1?"side-by-side":"wipe")+", swapped="+std::to_string(config.compareSwap)+", split_position="+std::to_string(config.comparePosition)+differenceStatus : "") + captureStatus;
    }
};
std::mutex stateMutex;
std::map<uint64_t, std::unique_ptr<Session>> sessions;
std::map<uint64_t, std::unique_ptr<Session>> pendingSettings;
std::map<uint64_t, std::string> settingsErrors;
std::map<uint64_t, unsigned> settingsRevisions;
std::map<int, Job> jobs;
uint64_t nextSession = 1;
int nextTicket = 1;
struct Stranded { std::unique_ptr<Session> session; std::vector<Job> inputs; };
auto* stranded = new std::vector<Stranded>;
std::map<uint64_t, std::vector<Job>> failedInputs;
void __stdcall RenderEvent(int ticket) noexcept {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto node = jobs.extract(ticket); if (node.empty()) return;
    auto found = sessions.find(node.mapped().session); if (found == sessions.end()) return;
    auto pending = pendingSettings.extract(node.mapped().session);
    if (!pending.empty()) {
        auto candidate = std::move(pending.mapped());
        try {
            // The old session has completed its GPU work. Validate the replacement
            // on this render thread before retiring the old feature and resources.
            if(candidate->config.holdFrame&&found->second->config.holdFrame){candidate->heldColor=found->second->heldColor;candidate->heldDepth=found->second->heldDepth;}
            candidate->Process(node.mapped());
            found->second = std::move(candidate);
            settingsErrors.erase(node.mapped().session);
            ++settingsRevisions[node.mapped().session];
            return;
        } catch (const std::exception& error) {
            settingsErrors[node.mapped().session] = std::string("Settings rejected; previous settings retained: ") + error.what();
        } catch (...) {
            settingsErrors[node.mapped().session] = "Settings rejected; previous settings retained: unknown native error";
        }
        if (candidate->gpuPending) stranded->push_back({std::move(candidate), {node.mapped()}});
        // A failed candidate never replaces the old session. The next event
        // resumes the previous settings; device faults still stop normal work.
        return;
    }
    auto& session = *found->second; if (session.fault) return;
    try { session.Process(node.mapped()); }
    catch (const std::exception& error) { session.fault = true; session.status = std::string("STOPPED; no further NR writes: ") + error.what(); }
    catch (...) { session.fault = true; session.status = "STOPPED: unknown native error"; }
    if (session.gpuPending) failedInputs[node.mapped().session].push_back(std::move(node.mapped()));
}
}
API uint64_t __cdecl GkmsNrOpen(const Configuration* options, const wchar_t* directory, const wchar_t* storage) noexcept {
    try {
        if (!ValidConfiguration(options)) return 0;
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&RenderEvent), &pinned)) return 0;
        std::lock_guard<std::mutex> lock(stateMutex); if (!sessions.empty()) return 0;
        const auto id = nextSession++; sessions.emplace(id, std::make_unique<Session>(*options, directory, storage)); return id;
    } catch (...) { return 0; }
}
API int __cdecl GkmsNrUpdate(uint64_t id, const Configuration* options, const wchar_t* directory) noexcept {
    try {
        if (!ValidConfiguration(options)) return 0;
        std::lock_guard<std::mutex> lock(stateMutex);
        auto found = sessions.find(id);
        if (found == sessions.end() || found->second->fault || found->second->gpuPending) return 0;
        if (options->neural && (!directory || !std::filesystem::path(directory).is_absolute() ||
            !std::filesystem::is_regular_file(std::filesystem::path(directory) / L"nvngx_dlssnr.dll"))) return 0;
        auto candidate = std::make_unique<Session>(*options, directory, found->second->storage.c_str());
        candidate->device12 = found->second->device12; // Reuse the device across feature replacements.
        pendingSettings[id] = std::move(candidate); // Coalesce rapid saves until the next render event.
        settingsErrors.erase(id);
        return 1;
    } catch (...) { return 0; }
}
API void __cdecl GkmsNrClose(uint64_t id) noexcept {
    std::lock_guard<std::mutex> lock(stateMutex);
    pendingSettings.erase(id); settingsErrors.erase(id); settingsRevisions.erase(id);
    for (auto it = jobs.begin(); it != jobs.end();) { if (it->second.session == id) it = jobs.erase(it); else ++it; }
    auto node = sessions.extract(id); auto retained = failedInputs.extract(id);
    if (!node.empty() && node.mapped()->gpuPending) stranded->push_back({std::move(node.mapped()), retained.empty() ? std::vector<Job>{} : std::move(retained.mapped())});
}
API void* __cdecl GkmsNrRenderEvent() noexcept { return reinterpret_cast<void*>(&RenderEvent); }
API int __cdecl GkmsNrQueueEx(uint64_t id, IUnknown* color, IUnknown* motion, int frame, int64_t camera,IUnknown* depth) noexcept {
    try {
        std::lock_guard<std::mutex> lock(stateMutex); auto found = sessions.find(id);
        if (found == sessions.end() || found->second->fault || jobs.size() >= 3 || !color || !motion || nextTicket == INT_MAX) return 0;
        Job job{id, {}, {}, frame, camera,{}};
        Check(color->QueryInterface(IID_PPV_ARGS(&job.color)), "Expected DX11 color texture"); Check(motion->QueryInterface(IID_PPV_ARGS(&job.motion)), "Expected DX11 motion texture");
        if(depth)depth->QueryInterface(IID_PPV_ARGS(&job.depth)); // Optional guide; no fallback texture is invented.
        const auto ticket = nextTicket++; jobs.emplace(ticket, std::move(job)); return ticket;
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (auto found = sessions.find(id); found != sessions.end()) found->second->status = error.what(); return 0;
    } catch (...) { return 0; }
}
API int __cdecl GkmsNrQueue(uint64_t id,IUnknown* color,IUnknown* motion,int frame,int64_t camera) noexcept {return GkmsNrQueueEx(id,color,motion,frame,camera,nullptr);}
API void __cdecl GkmsNrCancel(uint64_t id, int ticket) noexcept {
    std::lock_guard<std::mutex> lock(stateMutex); auto found = jobs.find(ticket); if (found != jobs.end() && found->second.session == id) jobs.erase(found);
}
API int __cdecl GkmsNrStatus(uint64_t id, char* destination, int capacity) noexcept {
    std::lock_guard<std::mutex> lock(stateMutex); if (!destination || capacity <= 0) return 0;
    auto found = sessions.find(id);
    std::string text = found == sessions.end() ? "Closed" : found->second->status;
    const auto error = settingsErrors.find(id);
    if (error != settingsErrors.end()) text = error->second + " | " + text;
    else if (pendingSettings.count(id)) text = "Settings pending next render frame | " + text;
    else if (auto revision = settingsRevisions.find(id); revision != settingsRevisions.end())
        text = "Settings applied #" + std::to_string(revision->second) + " | " + text;
    strncpy_s(destination, static_cast<size_t>(capacity), text.c_str(), _TRUNCATE);
    return found == sessions.end() ? -1 : found->second->fault || error != settingsErrors.end() ? 1 : 0;
}
#ifdef GKMS_NR_TEST_MAIN
#include "NativeTests.inl"
#endif
