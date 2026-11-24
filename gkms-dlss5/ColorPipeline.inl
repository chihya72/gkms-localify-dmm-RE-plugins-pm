// Host integration for the OptiScaler/RenoDX shader in ColorPipeline.h.
struct alignas(16) ColorConstants {
    uint32_t mode; float white; uint32_t width, height;
    float transferStrength, colorStrength; uint32_t debug; float maxRatio;
    uint32_t passthrough; float motionX, motionY; uint32_t guideWidth;
    uint32_t guideHeight, comparison; float split, zoom;
    uint32_t swap, transfer; float debugScale; uint32_t codec;
    uint32_t apply, gameExposure; float exposurePreMul, padding;
};
static_assert(sizeof(ColorConstants) == 96);
void InitializeColorPipeline() {
    auto compiled = Compile(ColorPipelineShader.c_str(), "cs_5_0", "CSMain");
    Check(device11->CreateComputeShader(compiled->GetBufferPointer(), compiled->GetBufferSize(), nullptr, &pipelineShader), "Create reference color pipeline");
    D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = sizeof(ColorConstants); buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Check(device11->CreateBuffer(&buffer, nullptr, &pipelineConstants), "Create color constants");
    D3D11_SAMPLER_DESC sampler{}; sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampler.MaxLOD = D3D11_FLOAT32_MAX;
    Check(device11->CreateSamplerState(&sampler, &linearSampler), "Create color sampler");
    const std::string coordinates = "uint2 q=uint2(p.xy);" + std::string(config.flipY ? "q.y=h-1-q.y;" : "");
    const std::string decode = "Texture2D<float4> src:register(t0);RWTexture2D<float4> dst:register(u0);[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h;dst.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;" + coordinates + "float4 c=src.Load(int3(q,0));" +
        (config.inputEncoding ? "float3 v=max(c.rgb,0);c.rgb=lerp(v/12.92,pow((v+.055)/1.055,2.4),step(.04045,v));" : "") + "dst[p.xy]=c;}";
    compiled = Compile(decode.c_str(), "cs_5_0");
    Check(device11->CreateComputeShader(compiled->GetBufferPointer(), compiled->GetBufferSize(), nullptr, &prepareShader), "Create oriented model input preparation");
    const std::string encoding = "float3 enc(float3 v){v=max(v,0);return lerp(v*12.92,1.055*pow(max(v,1e-8),1./2.4)-.055,step(.0031308,v));}";
    auto encode = encoding + "Texture2D<float4> src:register(t0);float4 main(float4 p:SV_POSITION):SV_TARGET{uint w,h;src.GetDimensions(w,h);" + coordinates + "float4 c=src.Load(int3(q,0));return float4(enc(c.rgb),c.a);}";
    compiled = Compile(encode.c_str(), "ps_5_0");
    Check(device11->CreatePixelShader(compiled->GetBufferPointer(), compiled->GetBufferSize(), nullptr, &encodedPixelShader), "Create sRGB output encoder");
    encode = encoding + "Texture2D<float4> a:register(t0);Texture2D<float4> b:register(t1);float4 main(float4 p:SV_POSITION):SV_TARGET{uint w,h;b.GetDimensions(w,h);if(abs(p.x-w*.5)<max(2,w*.003))return float4(1,1,0,1);" + coordinates + "float4 c=p.x<w*.5?b.Load(int3(q,0)):a.Load(int3(q,0));return float4(enc(c.rgb),c.a);}";
    compiled = Compile(encode.c_str(), "ps_5_0");
    Check(device11->CreatePixelShader(compiled->GetBufferPointer(), compiled->GetBufferSize(), nullptr, &encodedSplitShader), "Create sRGB comparison encoder");
}
float WhitePoint() const { return (config.autoExposure && exposureReady ? measuredWhitePoint : config.whitePoint) * std::exp2(-config.exposureEV); }
void ColorPass(unsigned mode, unsigned w, unsigned h, ID3D11ShaderResourceView* source,
    ID3D11ShaderResourceView* model, ID3D11ShaderResourceView* scene, ID3D11UnorderedAccessView* target, ID3D11UnorderedAccessView* keep = nullptr) {
    ColorConstants constants{mode, WhitePoint(), w, h, config.transferStrength, config.colorStrength, 0, config.maxRatio,
        0, config.motionX, config.motionY, workWidth, workHeight, 0, .5f, 1, 0, config.transferMode, 1, config.codec,
        config.transferStrength > 0 ? 1u : 0u, 0, 1, 0};
    deferred->ClearState(); deferred->UpdateSubresource(pipelineConstants.Get(), 0, nullptr, &constants, 0, 0);
    auto cb = pipelineConstants.Get(); auto sampler = linearSampler.Get();
    deferred->CSSetConstantBuffers(0, 1, &cb); deferred->CSSetSamplers(0, 1, &sampler);
    ID3D11ShaderResourceView* sources[]{source, model, scene};
    ID3D11UnorderedAccessView* targets[]{target, keep};
    deferred->CSSetShaderResources(0, 3, sources); deferred->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
    deferred->CSSetShader(pipelineShader.Get(), nullptr, 0); deferred->Dispatch((w+7)/8, (h+7)/8, 1);
    deferred->ClearState();
}
void UpdateExposure(bool reset) {
    if (!meterTexture) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = d.Height = 64; d.MipLevels = d.ArraySize = 1; d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        Check(device11->CreateTexture2D(&d, nullptr, &meterTexture), "Create exposure tiles");
        Check(device11->CreateUnorderedAccessView(meterTexture.Get(), nullptr, &meterUav), "Create exposure UAV");
        d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device11->CreateTexture2D(&d, nullptr, &meterReadback), "Create exposure readback");
    }
    ColorPass(4, 64, 64, original.srv.Get(), nullptr, nullptr, meterUav.Get());
    deferred->CopyResource(meterReadback.Get(), meterTexture.Get()); ExecuteDeferred();
    const auto done = ++value; Check(immediate->Signal(fence11.Get(), done), "Signal exposure meter");
    immediate->Flush(); Wait(done); gpuPending = false;
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(immediate->Map(meterReadback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read exposure meter");
    std::vector<float> peaks; peaks.reserve(4096);
    for (unsigned y=0; y<64; ++y) {
        const auto row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + y*mapped.RowPitch);
        for (unsigned x=0; x<64; ++x) if (std::isfinite(row[x*4]) && row[x*4] > .0001f) peaks.push_back(row[x*4]);
    }
    immediate->Unmap(meterReadback.Get(), 0);
    // Do not chase loading screens or isolated highlights in an otherwise dark scene.
    if (peaks.size() < 512) return;
    std::sort(peaks.begin(), peaks.end());
    const float target = std::clamp(peaks[peaks.size()*9/10], .25f, 4096.f);
    measuredWhitePoint = !exposureReady || reset ? target : measuredWhitePoint*.75f + target*.25f;
    exposureReady = true;
}
void PrepareModelInput(ID3D11ShaderResourceView* source, bool reset) {
    // All model-space color resources are upright. Composite restores game orientation.
    Convert(source, original, prepareShader.Get());
    if (config.autoExposure && (reset || processed % 30 == 0)) UpdateExposure(reset);
    // gKeep is a temporary untouched copy; original stays separate and immutable.
    ColorPass(0, width, height, original.srv.Get(), nullptr, nullptr, proxyFull.uav.Get(), resolved.uav.Get());
    if(config.modelScale>1)ResizePass(proxyFull.srv.Get(),input,0);
    else ColorPass(2, workWidth, workHeight, proxyFull.srv.Get(), nullptr, nullptr, input.uav.Get());
}
void ResolveModelOutput() {
    if(config.modelScale>1) {
        Downsample(input,superInput);Downsample(output,superOutput);
        deferred->ClearState();ID3D11ShaderResourceView* views[]{superOutput.srv.Get(),superInput.srv.Get(),proxyFull.srv.Get()};
        deferred->CSSetShaderResources(0,3,views);auto dst=superAdjusted.uav.Get();deferred->CSSetUnorderedAccessViews(0,1,&dst,nullptr);
        deferred->CSSetShader(residualShader.Get(),nullptr,0);deferred->Dispatch((width+7)/8,(height+7)/8,1);deferred->ClearState();
        ColorPass(1,width,height,proxyFull.srv.Get(),superAdjusted.srv.Get(),original.srv.Get(),resolved.uav.Get());
    } else ColorPass(1, width, height, input.srv.Get(), output.srv.Get(), original.srv.Get(), resolved.uav.Get());
    ExecuteDeferred();
}
static float DecodeHalf(uint16_t v) {
    const int e = (v >> 10) & 31;
    const float value = e ? std::ldexp(1.f + (v & 1023) / 1024.f, e-15) : std::ldexp(static_cast<float>(v & 1023), -24);
    return v & 0x8000 ? -value : value;
}
void SaveBitmap(ID3D11Texture2D* source, const std::filesystem::path& path, bool srgbEncoded) {
    D3D11_TEXTURE2D_DESC d{}; source->GetDesc(&d); d.BindFlags = d.MiscFlags = 0;
    d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device11->CreateTexture2D(&d, nullptr, &staging), "Create capture readback");
    immediate->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map capture");
    std::vector<uint8_t> pixels(static_cast<size_t>(d.Width)*d.Height*4);
    for (unsigned y=0; y<d.Height; ++y) {
        const auto row = reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch);
        for (unsigned x=0; x<d.Width; ++x) {
            const auto i = (static_cast<size_t>(y)*d.Width+x)*4;
            for (unsigned c=0; c<3; ++c) {
                float v = (std::max)(0.f, DecodeHalf(row[x*4+c]));
                if (!srgbEncoded) v = v <= .0031308f ? v*12.92f : 1.055f*std::pow(v, 1.f/2.4f)-.055f;
                pixels[i+2-c] = static_cast<uint8_t>(std::clamp(v, 0.f, 1.f)*255.f+.5f);
            }
            pixels[i+3] = 255;
        }
    }
    immediate->Unmap(staging.Get(), 0);
    BITMAPFILEHEADER file{}; file.bfType = 0x4D42; file.bfOffBits = sizeof(file)+sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = static_cast<LONG>(d.Width); info.biHeight = -static_cast<LONG>(d.Height); info.biPlanes = 1; info.biBitCount = 32;
    std::ofstream stream(path, std::ios::binary); if (!stream) throw std::runtime_error("Cannot create comparison capture");
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file)); stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    if (!stream) throw std::runtime_error("Comparison capture write failed");
}
void CaptureFrame(int frame) {
    // Capture a matched quartet BEFORE drawing the comparison divider, without overlays added later.
    MeasureDifference();
    if (sampledInputMax < .02) { visibleCaptureFrames = 0; return; }
    // Four non-black checks, 30 processed frames apart, avoid the first loading reveal.
    if (++visibleCaptureFrames < 4) return;
    captured = true; // One attempt per settings application, never an endless disk-write loop.
    try {
        const auto prefix = "nr-" + std::to_string(GetTickCount64()) + "-frame" + std::to_string(frame);
        const auto directory = std::filesystem::path(storage);
        SaveBitmap(original.texture.Get(), directory/(prefix+"-original.bmp"), false);
        SaveBitmap(input.texture.Get(), directory/(prefix+"-model-input.bmp"), true);
        SaveBitmap(output.texture.Get(), directory/(prefix+"-model-output.bmp"), true);
        SaveBitmap(resolved.texture.Get(), directory/(prefix+"-final.bmp"), false);
        captureStatus = ", capture=" + prefix + " (4 BMP files in plugin storage)";
    } catch (const std::exception& e) { captureStatus = std::string(", capture_failed=") + e.what(); }
}
