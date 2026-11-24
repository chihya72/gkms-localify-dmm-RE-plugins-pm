void InitializeExtendedPipeline() {
    auto code=Compile(ResizeShader,"cs_5_0");Check(device11->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&resizeShader),"Create reconstruction filter");
    code=Compile(ResidualShader,"cs_5_0");Check(device11->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&residualShader),"Create supersample residual");
    code=Compile(DisplayShader,"ps_5_0");Check(device11->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&displayShader),"Create comparison views");
    D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Check(device11->CreateBuffer(&d,nullptr,&resizeConstants),"Create resize settings");
    d.ByteWidth=48;Check(device11->CreateBuffer(&d,nullptr,&displayConstants),"Create display settings");
    const auto depthCode=std::string("Texture2D<float> src:register(t0);RWTexture2D<float> dst:register(u0);[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h,sw,sh;dst.GetDimensions(w,h);src.GetDimensions(sw,sh);if(p.x>=w||p.y>=h)return;uint2 pos=p.xy;")+(config.flipY?"pos.y=h-1-pos.y;":"")+"uint2 q=min(((2*pos+1)*uint2(sw,sh))/(2*uint2(w,h)),uint2(sw-1,sh-1));dst[p.xy]=src.Load(int3(q,0));}";
    code=Compile(depthCode.c_str(),"cs_5_0");Check(device11->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&depthShader),"Create depth conversion");
}
void ResizePass(ID3D11ShaderResourceView* source,SharedTexture& target,unsigned axis) {
    deferred->ClearState();const uint32_t c[]{axis,config.downFilter,0,0};deferred->UpdateSubresource(resizeConstants.Get(),0,nullptr,c,0,0);
    auto cb=resizeConstants.Get();deferred->CSSetConstantBuffers(0,1,&cb);Convert(source,target,resizeShader.Get());deferred->ClearState();
}
void Downsample(SharedTexture& source,SharedTexture& target) {
    ResizePass(source.srv.Get(),resizeScratch,0);ResizePass(resizeScratch.srv.Get(),target,1);
}
void PrepareHeldFrame(const Job& job) {
    if(!config.holdFrame){heldColor.Reset();heldDepth.Reset();return;}
    if(heldColor){D3D11_TEXTURE2D_DESC d{};heldColor->GetDesc(&d);D3D11_TEXTURE2D_DESC s{};job.color->GetDesc(&s);if(d.Width!=s.Width||d.Height!=s.Height||d.Format!=s.Format){heldColor.Reset();heldDepth.Reset();}}
    if(!heldColor){D3D11_TEXTURE2D_DESC d{};job.color->GetDesc(&d);d.MiscFlags=0;Check(device11->CreateTexture2D(&d,nullptr,&heldColor),"Create held scene");deferred->CopyResource(heldColor.Get(),job.color.Get());
        if(job.depth){job.depth->GetDesc(&d);d.MiscFlags=0;if(SUCCEEDED(device11->CreateTexture2D(&d,nullptr,&heldDepth)))deferred->CopyResource(heldDepth.Get(),job.depth.Get());}}
}
void PrepareDepth(ID3D11Texture2D* texture) {
    depthActive=false;depthStatus=config.useDepth?"unavailable":"disabled";if(!config.useDepth||!texture)return;
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);ComPtr<ID3D11Device> owner;texture->GetDevice(&owner);
    ComPtr<ID3D11Device> sameDevice;Check(device11.As(&sameDevice),"Depth device identity");
    if(owner.Get()!=sameDevice.Get()||d.SampleDesc.Count!=1||d.ArraySize!=1||!(d.BindFlags&D3D11_BIND_SHADER_RESOURCE)){depthStatus="unsupported resource";return;}
    DXGI_FORMAT format=d.Format;
    if(format==DXGI_FORMAT_R32_TYPELESS||format==DXGI_FORMAT_D32_FLOAT)format=DXGI_FORMAT_R32_FLOAT;
    else if(format==DXGI_FORMAT_R24G8_TYPELESS||format==DXGI_FORMAT_D24_UNORM_S8_UINT)format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    else if(format==DXGI_FORMAT_R16_TYPELESS||format==DXGI_FORMAT_D16_UNORM)format=DXGI_FORMAT_R16_UNORM;
    if(format!=DXGI_FORMAT_R32_FLOAT&&format!=DXGI_FORMAT_R16_UNORM&&format!=DXGI_FORMAT_R24_UNORM_X8_TYPELESS){depthStatus="unsupported format";return;}
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=format;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;
    ComPtr<ID3D11ShaderResourceView> srv;if(FAILED(device11->CreateShaderResourceView(texture,&view,&srv))){depthStatus="SRV unavailable";return;}
    if(!depth.texture)depth=MakeShared(DXGI_FORMAT_R32_FLOAT,workWidth,workHeight);
    Convert(srv.Get(),depth,depthShader.Get());depthActive=true;depthStatus="connected";
}
