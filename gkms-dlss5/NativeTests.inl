#include <iostream>
#include <chrono>
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
double HalfValue(uint16_t bits) {
    const int exponent = (bits >> 10) & 31;
    const double fraction = bits & 1023;
    const double value = exponent ? std::ldexp(1. + fraction / 1024., exponent - 15) : std::ldexp(fraction, -24);
    return bits & 0x8000 ? -value : value;
}
uint16_t PreviewHalf(double value) {
    // Preview replay only: BMP samples decode to finite [0,1] linear values.
    if (value < std::ldexp(1.,-14)) return static_cast<uint16_t>(std::lround(std::ldexp(value,24)));
    int exponent{}; const auto mantissa=std::frexp(value,&exponent);
    return static_cast<uint16_t>(((exponent+14)<<10)+std::lround((mantissa*2-1)*1024));
}
ComPtr<ID3D11Texture2D> TestTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, const float* clear) {
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture; Check(device->CreateTexture2D(&desc, nullptr, &texture), "Create fixture texture");
    ComPtr<ID3D11RenderTargetView> view; Check(device->CreateRenderTargetView(texture.Get(), nullptr, &view), "Create fixture target");
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context); context->ClearRenderTargetView(view.Get(), clear); return texture;
}
std::vector<uint16_t> ReadHalfTexture(ID3D11Device* device, ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc); desc.BindFlags = 0; desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const unsigned channels = desc.Format == DXGI_FORMAT_R16G16_FLOAT ? 2 : 4;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging), "Create readback");
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context); context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read GPU result");
    std::vector<uint16_t> values;
    for (UINT y = 0; y < desc.Height; ++y) {
        auto row = reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
        values.insert(values.end(), row, row + desc.Width * channels);
    }
    context->Unmap(staging.Get(), 0); return values;
}
void CheckDebug(ID3D11Device* device11, ID3D12Device* device12) {
    ComPtr<ID3D11InfoQueue> info11;
    if (SUCCEEDED(device11->QueryInterface(IID_PPV_ARGS(&info11)))) {
        for (UINT64 i = 0; i < info11->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size{}; info11->GetMessage(i, nullptr, &size); std::vector<char> bytes(size);
            auto msg = reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); Check(info11->GetMessage(i, msg, &size), "Read DX11 debug message");
            if (msg->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(msg->pDescription);
        }
    }
    ComPtr<ID3D12InfoQueue> info12;
    if (SUCCEEDED(device12->QueryInterface(IID_PPV_ARGS(&info12)))) {
        for (UINT64 i = 0; i < info12->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size{}; info12->GetMessage(i, nullptr, &size); std::vector<char> bytes(size);
            auto msg = reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); Check(info12->GetMessage(i, msg, &size), "Read DX12 debug message");
            if (msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(msg->pDescription);
        }
    }
}
}
int wmain(int argc, wchar_t** argv) {
    ComPtr<ID3D11Device> device;
    try {
        ngx::OwnedParameters parameters;
        parameters.Set("float", .5f); double doubleValue{};
        Require(parameters.Get("float", &doubleValue) == 1 && doubleValue == .5, "NGX float conversion failed");
        parameters.Set("signed", -4); int signedValue{};
        Require(parameters.Get("signed", &signedValue) == 1 && signedValue == -4, "NGX signed value failed");
        parameters.Set("pointer", static_cast<void*>(&parameters)); void* pointer{};
        Require(parameters.Get("pointer", &pointer) == 1 && pointer == &parameters, "NGX pointer roundtrip failed");
        Require(parameters.Get("pointer", &signedValue) != 1, "NGX pointer accepted as number");
        parameters.Reset(); Require(parameters.Get("float", &doubleValue) != 1, "NGX reset retained a value");
        std::cout << "PASS: owned NGX parameter types and reset\n";
        ComPtr<ID3D12Debug> debug12;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug12)))) debug12->EnableDebugLayer();
        ComPtr<ID3D11DeviceContext> context;
        auto hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        const bool debug11 = SUCCEEDED(hr);
        if (!debug11) Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "Create standalone test device");
        ComPtr<IDXGIDevice> dxgi; Check(device.As(&dxgi), "Test DXGI device"); ComPtr<IDXGIAdapter> adapter; Check(dxgi->GetAdapter(&adapter), "Test adapter");
        DXGI_ADAPTER_DESC adapterDesc{}; Check(adapter->GetDesc(&adapterDesc), "Test adapter description");
        std::wcout << L"GPU: " << adapterDesc.Description << L"; debug11=" << debug11 << L" debug12=" << !!debug12 << L"\n";
        const float colorValue[]{.25f, .5f, .75f, 1}; const float motionValue[]{.125f, -.25f, 0, 0}; const float zero[]{0, 0, 0, 0};
        if (argc == 4 || argc == 5) {
            // Replay our own uncompressed 32-bit BMP captures without launching the game.
            // This is an 8-bit SDR preview experiment, not full-HDR/temporal validation.
            std::ifstream image(std::filesystem::path(argv[3]),std::ios::binary);
            BITMAPFILEHEADER header{}; BITMAPINFOHEADER info{};
            image.read(reinterpret_cast<char*>(&header),sizeof(header)); image.read(reinterpret_cast<char*>(&info),sizeof(info));
            Require(image.good() && header.bfType==0x4D42 && info.biSize==sizeof(info) && info.biWidth>0 && info.biHeight<0 && info.biWidth<=8192 && info.biHeight>=-8192 && info.biPlanes==1 && info.biBitCount==32 && info.biCompression==BI_RGB,"Replay requires a top-down 32-bit capture BMP");
            const auto w=static_cast<unsigned>(info.biWidth), h=static_cast<unsigned>(-info.biHeight);
            std::vector<uint8_t> bytes(static_cast<size_t>(w)*h*4);
            image.seekg(header.bfOffBits); image.read(reinterpret_cast<char*>(bytes.data()),bytes.size()); Require(image.good(),"Truncated capture BMP");
            std::vector<uint16_t> fixture(bytes.size());
            for (size_t i=0; i<bytes.size(); i+=4) {
                for (unsigned c=0; c<3; ++c) {
                    const double v=bytes[i+2-c]/255.;
                    fixture[i+c]=PreviewHalf(v<=.04045 ? v/12.92 : std::pow((v+.055)/1.055,2.4));
                }
                fixture[i+3]=0x3C00;
            }
            auto source=TestTexture(device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,zero);
            auto vectors=TestTexture(device.Get(),w,h,DXGI_FORMAT_R16G16_FLOAT,zero);
            Configuration options{sizeof(Configuration),1,0,1,1,1,1,1,-1,-1};
            const auto id=GkmsNrOpen(&options,argv[1],argv[2]); Require(id!=0,"Replay open failed");
            const wchar_t* replayNames[]{L"replay-flip0",L"replay-flip1",L"replay-strong",L"replay-full",L"replay-hybrid",L"replay-unmasked"};
            const bool portPreview=argc==5;
            for (unsigned variant=0; variant<(portPreview?2u:6u); ++variant) {
                auto candidate=options; candidate.flipY=variant ? 1u : 0u;
                if (variant==2) { candidate.intensity=2; candidate.tone=2; candidate.structure=2; candidate.skin=-1; candidate.transferStrength=2; }
                if (variant==3) candidate.modelScale=1;
                if (variant==4) candidate.codec=3;
                if (variant==5) candidate.autoMask=0;
                if(portPreview){candidate.flipY=0;candidate.modelScale=variant?1.25f:1;candidate.colorStrength=1;candidate.transferStrength=2;}
                if (variant||portPreview) Require(GkmsNrUpdate(id,&candidate,argv[1])==1,"Replay settings update failed");
                for (unsigned frame=1; frame<=30; ++frame) {
                    context->UpdateSubresource(source.Get(),0,nullptr,fixture.data(),w*8,0);
                    const auto ticket=GkmsNrQueue(id,source.Get(),vectors.Get(),frame,101); Require(ticket>0,"Replay queue failed"); RenderEvent(ticket);
                    Require(!sessions.at(id)->fault,sessions.at(id)->status.c_str());
                }
                auto& replay=*sessions.at(id); const std::wstring prefix=portPreview?(variant?L"port-super125":L"port-native"):replayNames[variant];
                const auto directory=std::filesystem::path(argv[2]);
                replay.SaveBitmap(replay.original.texture.Get(),directory/(prefix+L"-original.bmp"),false);
                replay.SaveBitmap(replay.input.texture.Get(),directory/(prefix+L"-model-input.bmp"),true);
                replay.SaveBitmap(replay.output.texture.Get(),directory/(prefix+L"-model-output.bmp"),true);
                replay.SaveBitmap(replay.resolved.texture.Get(),directory/(prefix+L"-final.bmp"),false);
                if(portPreview){
                    replay.config.comparisonSplit=1;replay.config.compareMode=1;replay.config.compareLabels=1;
                    replay.Composite(source.Get());replay.SaveBitmap(source.Get(),directory/(prefix+L"-compare.bmp"),false);
                    replay.config.debugView=3;replay.Composite(source.Get());replay.SaveBitmap(source.Get(),directory/(prefix+L"-difference.bmp"),false);
                }
                replay.MeasureDifference(); CheckDebug(device.Get(),replay.device12.Get());
                std::cout << "PASS: captured-scene replay variant=" << variant << "; " << replay.status << replay.differenceStatus << "\n";
            }
            GkmsNrClose(id); return 0;
        }
        if (argc == 3) {
            auto source = TestTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, colorValue);
            auto vectors = TestTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16_FLOAT, zero);
            std::vector<uint16_t> fixture(512 * 512 * 4);
            for (unsigned y = 0; y < 512; ++y) for (unsigned x = 0; x < 512; ++x) {
                const auto i = (y * 512 + x) * 4;
                fixture[i] = static_cast<uint16_t>(0x3000 + x * 4);
                fixture[i+1] = static_cast<uint16_t>(0x3000 + y * 4);
                fixture[i+2] = ((x / 32 + y / 32) % 2) ? 0x3800 : 0x3400;
                fixture[i+3] = 0x3C00;
            }
            Configuration options{sizeof(Configuration), 1, 0, 1, .5f, .25f, .5f, 0, -1, -1};
            const auto id = GkmsNrOpen(&options, argv[1], argv[2]); Require(id != 0, "NR probe session creation failed");
            auto& session = *sessions.at(id);
            for (int frame = 1; frame <= 3; ++frame) {
                // Restore the same scene each frame; never feed NR its previous output.
                context->UpdateSubresource(source.Get(), 0, nullptr, fixture.data(), 512 * 8, 0);
                const auto event = GkmsNrQueue(id, source.Get(), vectors.Get(), frame, 101); Require(event > 0, "NR probe queue failed");
                const auto start = std::chrono::steady_clock::now(); RenderEvent(event);
                const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                std::cout << session.status << "; elapsed_ms=" << milliseconds << "\n";
                if (session.fault) { GkmsNrClose(id); return 2; }
                Require(session.processed == static_cast<unsigned>(frame), "NR frame did not complete");
                CheckDebug(device.Get(), session.device12.Get());
                const auto result = ReadHalfTexture(device.Get(), source.Get()); size_t changed{};
                for (size_t i = 0; i < result.size(); ++i) {
                    Require((result[i] & 0x7C00) != 0x7C00, "NR produced non-finite pixels");
                    if (i % 4 == 3) Require(result[i] == fixture[i], "NR changed scene alpha");
                    else if (result[i] != fixture[i]) ++changed;
                }
                Require(changed != 0, "NR output was identical to input");
                double absoluteSum{}, maximum{}; size_t aboveOneStep{};
                for (size_t i = 0; i < result.size(); ++i) if (i % 4 != 3) {
                    const auto delta = std::abs(HalfValue(result[i]) - HalfValue(fixture[i]));
                    absoluteSum += delta; maximum = (std::max)(maximum, delta);
                    if (delta > 1. / 255.) ++aboveOneStep;
                }
                const auto channels = result.size() / 4 * 3;
                std::cout << "NR delta: mean=" << absoluteSum / channels << " max=" << maximum << " above_1_255=" << 100. * aboveOneStep / channels << "%\n";
                if (frame == 1) {
                    const auto reads = static_cast<ngx::OwnedParameters*>(session.runtime->parameters)->readCounts;
                    for (const auto& entry : reads) std::cout << "NR parameter read: " << entry.first << " count=" << entry.second << "\n";
                }
                std::cout << "PASS: finite RGB output; changed_channels=" << changed << "; alpha preserved\n";
            }
            for (int frame = 4; frame <= 9; ++frame) {
                auto changedOptions = options;
                changedOptions.neural = frame == 6 ? 0u : 1u;
                changedOptions.intensity = frame % 2 ? 1.f : .1f;
                changedOptions.tone = .75f; changedOptions.structure = .8f;
                changedOptions.skin = .1f; changedOptions.motionX = -.5f;
                changedOptions.preset = frame == 9 ? 1u : 0u;
                changedOptions.autoExposure = frame == 5 ? 1u : 0u;
                changedOptions.modelScale = frame == 5 ? 1.f : frame == 7 ? .75f : .5f;
                changedOptions.codec = frame == 7 ? 1u : frame == 8 ? 3u : 0u;
                Require(GkmsNrUpdate(id, &changedOptions, argv[1]) == 1, "NR settings save rejected");
                context->UpdateSubresource(source.Get(), 0, nullptr, fixture.data(), 512 * 8, 0);
                const auto event = GkmsNrQueue(id, source.Get(), vectors.Get(), frame, 101);
                Require(event > 0, "Hot reload frame queue failed"); RenderEvent(event);
                auto& updated = *sessions.at(id);
                char status[2048]{}; Require(GkmsNrStatus(id, status, 2048) == 0, status);
                Require(updated.processed == 1 && updated.config.intensity == changedOptions.intensity, "New session did not use saved settings");
                Require(!!updated.runtime == !!changedOptions.neural, "NR toggle did not apply");
                CheckDebug(device.Get(), updated.device12.Get());
                const auto pixels = ReadHalfTexture(device.Get(), source.Get());
                if (!changedOptions.neural) Require(pixels == fixture, "Disable NR changed scene");
                for (auto pixel : pixels) Require((pixel & 0x7C00) != 0x7C00, "Hot reload generated non-finite output");
                std::cout << "PASS: live NR settings/toggle frame=" << frame << "; " << status << "\n";
                for (int followup = 1; followup <= 3; ++followup) {
                    context->UpdateSubresource(source.Get(), 0, nullptr, fixture.data(), 512 * 8, 0);
                    const auto next = GkmsNrQueue(id, source.Get(), vectors.Get(), frame * 10 + followup, 101);
                    Require(next > 0, "Post-update frame queue failed"); RenderEvent(next);
                    Require(GkmsNrStatus(id, status, 2048) == 0, status);
                    CheckDebug(device.Get(), updated.device12.Get());
                }
                std::cout << "PASS: three subsequent frames after retiring previous NR session\n";
            }
            auto splitOptions = options; splitOptions.comparisonSplit = 1;
            Require(GkmsNrUpdate(id, &splitOptions, argv[1]) == 1, "Split mode rejected");
            context->UpdateSubresource(source.Get(), 0, nullptr, fixture.data(), 512 * 8, 0);
            auto splitEvent = GkmsNrQueue(id, source.Get(), vectors.Get(), 100, 101); RenderEvent(splitEvent);
            char splitStatus[2048]{}; Require(GkmsNrStatus(id, splitStatus, 2048) == 0, splitStatus);
            const auto splitPixels = ReadHalfTexture(device.Get(), source.Get());
            const auto modelPixels = ReadHalfTexture(device.Get(), sessions.at(id)->resolved.texture.Get());
            double expectedMean{};
            for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
                const auto i = ((y*8+4)*512 + x*8+4)*4;
                for (unsigned c = 0; c < 3; ++c) expectedMean += std::abs(HalfValue(fixture[i+c]) - HalfValue(modelPixels[i+c]));
            }
            expectedMean /= 64*64*3;
            Require(std::abs(expectedMean - sessions.at(id)->differenceMean) < 1e-7, "GPU difference sampling disagreed with CPU readback");
            std::cout << "PASS: GPU difference samples match independent CPU readback; " << splitStatus << "\n";
            for (unsigned y = 0; y < 512; ++y) for (unsigned x = 0; x < 512; ++x) {
                const auto i = (y * 512 + x) * 4;
                for (unsigned c = 0; c < 3; ++c) {
                    const auto expected = x >= 254 && x <= 257 ? (c == 2 ? 0 : 0x3C00)
                        : (x < 256 ? fixture[i+c] : modelPixels[i+c]);
                    Require(splitPixels[i+c] == expected, "Split comparison did not match original/model pixels");
                }
                Require(splitPixels[i+3] == fixture[i+3], "Split comparison changed alpha");
            }
            CheckDebug(device.Get(), sessions.at(id)->device12.Get());
            std::cout << "PASS: split left-original/right-NR pixels and yellow separator\n";
            for (int captureCheck=0; captureCheck<4; ++captureCheck) sessions.at(id)->CaptureFrame(100);
            Require(sessions.at(id)->captureStatus.find("capture=") != std::string::npos, "Matched capture quartet failed");
            std::cout << sessions.at(id)->captureStatus << "\n";
            auto& pipeline = *sessions.at(id);
            const auto encoded = ReadHalfTexture(device.Get(), pipeline.input.texture.Get());
            for (size_t i=0; i<encoded.size(); i+=4) {
                for (size_t c=0; c<3; ++c) Require(HalfValue(encoded[i+c]) >= 0 && HalfValue(encoded[i+c]) <= 1, "Encoded proxy outside model range");
                Require(encoded[i+3] == 0x3C00, "Model input alpha is not opaque");
            }
            // Identity-model control: reconstruct the original when NR returns its input.
            // This catches color transforms manufacturing an enhancement on their own.
            pipeline.immediate->CopyResource(pipeline.output.texture.Get(), pipeline.input.texture.Get());
            pipeline.ResolveModelOutput();
            auto identityFence = ++pipeline.value; Check(pipeline.immediate->Signal(pipeline.fence11.Get(), identityFence), "Identity fence");
            pipeline.immediate->Flush(); pipeline.Wait(identityFence); pipeline.gpuPending = false;
            const auto identity = ReadHalfTexture(device.Get(), pipeline.resolved.texture.Get());
            double identityError{};
            for (size_t i=0; i<identity.size(); ++i) if(i%4 != 3) identityError = (std::max)(identityError, std::abs(HalfValue(identity[i])-HalfValue(fixture[i])));
            Require(identityError < .002, "Identity model introduced color/lighting changes");
            CheckDebug(device.Get(), pipeline.device12.Get());
            std::cout << "PASS: bounded opaque proxy and identity-model color reconstruction; max_error=" << identityError << "\n";
            auto bypass = options; bypass.transferStrength = 0;
            Require(GkmsNrUpdate(id, &bypass, argv[1]) == 1, "Zero-strength save failed");
            context->UpdateSubresource(source.Get(), 0, nullptr, fixture.data(), 512 * 8, 0);
            RenderEvent(GkmsNrQueue(id, source.Get(), vectors.Get(), 101, 101));
            Require(ReadHalfTexture(device.Get(), source.Get()) == fixture, "Zero transfer changed the original scene");
            CheckDebug(device.Get(), sessions.at(id)->device12.Get());
            std::cout << "PASS: zero transfer is a pixel-exact bypass\n";
            // A vertically asymmetric scene and spatially varying signed vectors expose
            // double flips, missing writeback flips, and motion sign/location errors.
            std::vector<uint16_t> motionFixture(512*512*2);
            for (unsigned y=0; y<512; ++y) for (unsigned x=0; x<512; ++x) {
                const auto i=(y*512+x)*2;
                motionFixture[i]=static_cast<uint16_t>(0x2800+x);
                motionFixture[i+1]=static_cast<uint16_t>(0xAC00+y);
                fixture[(y*512+x)*4+3]=static_cast<uint16_t>(0x3400+x);
            }
            context->UpdateSubresource(vectors.Get(), 0, nullptr, motionFixture.data(), 512*4, 0);
            for (unsigned encoding=0; encoding<2; ++encoding) for (unsigned split=0; split<2; ++split) {
                auto orientedOptions=options; orientedOptions.flipY=1; orientedOptions.modelScale=1;
                orientedOptions.inputEncoding=encoding; orientedOptions.comparisonSplit=split;
                Require(GkmsNrUpdate(id, &orientedOptions, argv[1]) == 1, "Orientation settings rejected");
                context->UpdateSubresource(source.Get(), 0, nullptr, fixture.data(), 512*8, 0);
                RenderEvent(GkmsNrQueue(id, source.Get(), vectors.Get(), 200+encoding*2+split, 101));
                auto& oriented=*sessions.at(id); Require(!oriented.fault, oriented.status.c_str());
                const auto upright=ReadHalfTexture(device.Get(), oriented.original.texture.Get());
                const auto moved=ReadHalfTexture(device.Get(), oriented.motion.texture.Get());
                const auto resolvedPixels=ReadHalfTexture(device.Get(), oriented.resolved.texture.Get());
                const auto written=ReadHalfTexture(device.Get(), source.Get());
                auto encode=[](double v) { return v<=.0031308 ? v*12.92 : 1.055*std::pow(v,1./2.4)-.055; };
                for (unsigned y=0; y<512; ++y) for (unsigned x=0; x<512; ++x) {
                    const auto i=(y*512+x)*4, flipped=((511-y)*512+x)*4;
                    Require(moved[i/2]==motionFixture[flipped/2] && moved[i/2+1]==(motionFixture[flipped/2+1]^0x8000), "Motion flip lost spatial alignment or Y sign");
                    for (unsigned c=0; c<3; ++c) {
                        const double raw=HalfValue(fixture[flipped+c]);
                        const double linear=encoding ? (raw<=.04045 ? raw/12.92 : std::pow((raw+.055)/1.055,2.4)) : raw;
                        Require(std::abs(HalfValue(upright[i+c])-linear)<.001, "Model scene is not upright");
                        double expected=HalfValue(split && x<256 ? upright[flipped+c] : resolvedPixels[flipped+c]);
                        if (encoding) expected=encode(expected);
                        if (split && x>=254 && x<=257) expected=c==2 ? 0 : 1;
                        Require(std::abs(HalfValue(written[i+c])-expected)<.002, "Output orientation or split restoration failed");
                    }
                    Require(written[i+3]==fixture[i+3], "Orientation changed destination alpha");
                }
                // Return the model's input unchanged and exercise the actual writeback too.
                oriented.immediate->CopyResource(oriented.output.texture.Get(), oriented.input.texture.Get());
                oriented.ResolveModelOutput(); oriented.Composite(source.Get());
                const auto orientationFence=++oriented.value;
                Check(oriented.immediate->Signal(oriented.fence11.Get(), orientationFence), "Orientation roundtrip fence");
                oriented.immediate->Flush(); oriented.Wait(orientationFence); oriented.gpuPending=false;
                const auto roundTrip=ReadHalfTexture(device.Get(), source.Get());
                for (unsigned y=0; y<512; ++y) for (unsigned x=0; x<512; ++x) {
                    if (split && x>=254 && x<=257) continue;
                    const auto i=(y*512+x)*4;
                    for (unsigned c=0; c<3; ++c) Require(std::abs(HalfValue(roundTrip[i+c])-HalfValue(fixture[i+c]))<.002, "Oriented identity roundtrip changed the scene");
                }
                CheckDebug(device.Get(), oriented.device12.Get());
                std::cout << "PASS: upright model input, flipped signed motion, restored game orientation, alpha and identity roundtrip; encoding=" << encoding << " split=" << split << "\n";
            }
#include "ExtendedTests.inl"
            GkmsNrClose(id);
            Require(stranded->empty(), "NR retained in-flight resources after completion"); return 0;
        }
        Require(argc == 1, "Usage: NativeTests.exe [runtime-directory existing-log-directory [capture.bmp]]");
        auto color = TestTexture(device.Get(), 37, 23, DXGI_FORMAT_R16G16B16A16_FLOAT, colorValue);
        auto motion = TestTexture(device.Get(), 37, 23, DXGI_FORMAT_R16G16_FLOAT, motionValue);
        Configuration config{sizeof(Configuration), 0, 0, 1, .5f, .25f, .5f, 0, -1, -1};
        auto sessionId = GkmsNrOpen(&config, L"", L""); Require(sessionId != 0, "Session open failed");
        auto ticket = GkmsNrQueue(sessionId, color.Get(), motion.Get(), 1, 101); Require(ticket > 0, "Queue failed");
        GkmsNrCancel(sessionId, ticket); RenderEvent(ticket); Require(sessions.at(sessionId)->processed == 0, "Cancelled event executed");
        D3D11_VIEWPORT viewport{1, 2, 17, 19, 0, 1}; context->RSSetViewports(1, &viewport);
        auto before = ReadHalfTexture(device.Get(), color.Get()); auto motionBefore = ReadHalfTexture(device.Get(), motion.Get());
        for (int frame = 1; frame <= 3; ++frame) {
            ticket = GkmsNrQueue(sessionId, color.Get(), motion.Get(), frame, 101); Require(ticket > 0, "Frame queue failed"); RenderEvent(ticket);
            auto& session = *sessions.at(sessionId); Require(!session.fault, session.status.c_str());
            Require(session.processed == static_cast<unsigned>(frame), "GPU frame did not execute");
            Require(before == ReadHalfTexture(device.Get(), session.output.texture.Get()), "Cross-API color mismatch");
            Require(motionBefore == ReadHalfTexture(device.Get(), session.motion.texture.Get()), "Motion conversion mismatch");
            Require(before == ReadHalfTexture(device.Get(), color.Get()), "Diagnostic mode changed game color");
            CheckDebug(device.Get(), session.device12.Get());
        }
        D3D11_VIEWPORT after{}; UINT count = 1; context->RSGetViewports(&count, &after);
        Require(count == 1 && after.TopLeftX == viewport.TopLeftX && after.Width == viewport.Width, "Immediate state was not restored");
        std::cout << "PASS: pixel-exact cross-API copies, signed motion, repeated frames, context state\n";
        auto& session = *sessions.at(sessionId);
        auto destination = TestTexture(device.Get(), 37, 23, DXGI_FORMAT_R16G16B16A16_FLOAT, zero); session.Composite(destination.Get());
        auto finished = ++session.value; Check(session.immediate->Signal(session.fence11.Get(), finished), "Composite test fence");
        session.immediate->Flush(); session.Wait(finished); session.gpuPending = false;
        auto composite = ReadHalfTexture(device.Get(), destination.Get());
        for (size_t i = 0; i < composite.size(); i += 4) {
            Require(composite[i] == before[i] && composite[i+1] == before[i+1] && composite[i+2] == before[i+2], "Composite RGB mismatch");
            Require(composite[i+3] == 0, "Composite overwrote alpha");
        }
        CheckDebug(device.Get(), session.device12.Get()); std::cout << "PASS: output composite and alpha preservation\n";
        auto changedConfig = config; changedConfig.intensity = .8f;
        auto invalidConfig = config; invalidConfig.intensity = 2.01f;
        Require(GkmsNrUpdate(sessionId, &invalidConfig, L"") == 0, "Invalid update accepted");
        Require(pendingSettings.empty() && session.config.intensity == .5f, "Rejected update mutated session");
        Require(GkmsNrUpdate(sessionId, &changedConfig, L"") == 1, "Valid update rejected");
        changedConfig.intensity = .9f;
        Require(GkmsNrUpdate(sessionId, &changedConfig, L"") == 1 && pendingSettings.size() == 1, "Rapid saves not coalesced");
        ticket = GkmsNrQueue(sessionId, color.Get(), motion.Get(), 4, 101); GkmsNrCancel(sessionId, ticket); RenderEvent(ticket);
        Require(pendingSettings.size() == 1, "Cancelled frame applied configuration");
        ticket = GkmsNrQueue(sessionId, color.Get(), motion.Get(), 5, 101); RenderEvent(ticket);
        Require(sessions.at(sessionId)->config.intensity == .9f && pendingSettings.empty(), "Latest save was not applied");
        char updateStatus[2048]{};
        Require(GkmsNrStatus(sessionId, updateStatus, 2048) == 0 && std::string(updateStatus).find("Settings applied #1") == 0, "Successful update status incorrect");
        // Simulate the runtime becoming unavailable between acceptance and rendering.
        Require(GkmsNrUpdate(sessionId, &config, L"") == 1, "Rollback fixture save rejected");
        pendingSettings.at(sessionId)->config.neural = 1;
        pendingSettings.at(sessionId)->runtimeDirectory = L"Z:\\gkms-test-missing-runtime";
        auto* previous = sessions.at(sessionId).get();
        ticket = GkmsNrQueue(sessionId, color.Get(), motion.Get(), 6, 101); RenderEvent(ticket);
        Require(sessions.at(sessionId).get() == previous && previous->config.intensity == .9f, "Failed candidate replaced prior settings");
        Require(GkmsNrStatus(sessionId, updateStatus, 2048) == 1, "Failed update was not reported");
        ticket = GkmsNrQueue(sessionId, color.Get(), motion.Get(), 7, 101); RenderEvent(ticket);
        Require(!previous->fault && previous->processed == 2, "Previous configuration failed to resume");
        Require(GkmsNrUpdate(sessionId, &config, L"") == 1, "Retry after failed update rejected");
        ticket = GkmsNrQueue(sessionId, color.Get(), motion.Get(), 8, 101); RenderEvent(ticket);
        Require(GkmsNrStatus(sessionId, updateStatus, 2048) == 0, "Successful retry did not clear update failure");
        std::cout << "PASS: invalid saves, coalescing, cancelled event, rollback and successful retry\n";
        color = TestTexture(device.Get(), 16, 11, DXGI_FORMAT_R16G16B16A16_FLOAT, colorValue);
        motion = TestTexture(device.Get(), 16, 11, DXGI_FORMAT_R16G16_FLOAT, motionValue);
        ticket = GkmsNrQueue(sessionId, color.Get(), motion.Get(), 4, 202); RenderEvent(ticket);
        auto& resized = *sessions.at(sessionId);
        Require(!resized.fault && resized.width == 16 && resized.height == 11, resized.status.c_str()); CheckDebug(device.Get(), resized.device12.Get());
        Require(GkmsNrUpdate(sessionId, &changedConfig, L"") == 1, "Pending-close fixture failed");
        ticket = GkmsNrQueue(sessionId, color.Get(), motion.Get(), 5, 202); GkmsNrClose(sessionId); RenderEvent(ticket);
        Require(sessions.empty() && jobs.empty() && pendingSettings.empty() && settingsErrors.empty() && settingsRevisions.empty(), "Close retained jobs or settings"); std::cout << "PASS: resize, cancellation, close with queued event\n";
        sessionId = GkmsNrOpen(&config, L"", L""); auto wrongMotion = TestTexture(device.Get(), 7, 5, DXGI_FORMAT_R16G16_FLOAT, motionValue);
        ticket = GkmsNrQueue(sessionId, color.Get(), wrongMotion.Get(), 6, 202); RenderEvent(ticket);
        Require(sessions.at(sessionId)->fault && sessions.at(sessionId)->processed == 0, "Mismatch was accepted");
        Require(GkmsNrQueue(sessionId, color.Get(), motion.Get(), 7, 202) == 0, "Faulted session accepted work"); GkmsNrClose(sessionId);
        Require(stranded->empty(), "Healthy tests retained GPU resources"); std::cout << "PASS: invalid input leaves scene untouched\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << "\n"; return 1; }
}
