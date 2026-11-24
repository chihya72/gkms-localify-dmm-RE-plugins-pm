// Included inside the real-model test after orientation coverage.
auto depthFixture=TestTexture(device.Get(),512,512,DXGI_FORMAT_R32_FLOAT,colorValue);
std::vector<float> depthValues(512*512);
for(unsigned y=0;y<512;++y)for(unsigned x=0;x<512;++x)depthValues[y*512+x]=.1f+.8f*(x+y)/1024.f;
context->UpdateSubresource(depthFixture.Get(),0,nullptr,depthValues.data(),512*4,0);
for(unsigned filter=0;filter<4;++filter){
    auto supersample=options;supersample.modelScale=filter==2?2.f:filter==1?1.5f:1.25f;supersample.downFilter=filter;supersample.flipY=1;
    Require(GkmsNrUpdate(id,&supersample,argv[1])==1,"Supersample settings rejected");
    context->UpdateSubresource(source.Get(),0,nullptr,fixture.data(),512*8,0);
    RenderEvent(GkmsNrQueueEx(id,source.Get(),vectors.Get(),300+filter,101,depthFixture.Get()));
    auto& current=*sessions.at(id);Require(!current.fault,current.status.c_str());Require(current.depthActive,"Depth guide was not connected");
    D3D11_TEXTURE2D_DESC dd{};current.depth.texture->GetDesc(&dd);dd.BindFlags=dd.MiscFlags=0;dd.Usage=D3D11_USAGE_STAGING;dd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;Check(device->CreateTexture2D(&dd,nullptr,&stage),"Create depth verification");context->CopyResource(stage.Get(),current.depth.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped),"Read aligned depth");bool aligned=true;
    for(unsigned y=0;y<dd.Height;++y){auto row=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch);for(unsigned x=0;x<dd.Width;++x){
        const auto sx=((2*x+1)*512)/(2*dd.Width);
        const auto sy=((2*(dd.Height-1-y)+1)*512)/(2*dd.Height);aligned=aligned&&std::abs(row[x]-depthValues[sy*512+sx])<1e-6f;}}
    context->Unmap(stage.Get(),0);Require(aligned,"Depth scale or orientation mismatch");
    const auto pixels=ReadHalfTexture(device.Get(),source.Get());for(size_t i=0;i<pixels.size();++i){Require((pixels[i]&0x7c00)!=0x7c00,"Super-resolution produced non-finite output");if(i%4==3)Require(pixels[i]==fixture[i],"Supersample changed alpha");}
    context->CopyResource(current.output.texture.Get(),current.input.texture.Get());current.ResolveModelOutput();
    const auto roundtrip=ReadHalfTexture(device.Get(),current.resolved.texture.Get());const auto reference=ReadHalfTexture(device.Get(),current.original.texture.Get());
    double largest{};for(size_t i=0;i<roundtrip.size();++i)if(i%4!=3)largest=(std::max)(largest,std::abs(HalfValue(roundtrip[i])-HalfValue(reference[i])));
    Require(largest<.002,"Supersample identity-model control lost scene detail");
    const auto done=++current.value;Check(current.immediate->Signal(current.fence11.Get(),done),"Supersample test completion");current.immediate->Flush();current.Wait(done);current.gpuPending=false;
    CheckDebug(device.Get(),current.device12.Get());std::cout<<"PASS: supersampling filter="<<filter<<", model scale="<<supersample.modelScale<<", identity max="<<largest<<", aligned depth\n";
}
auto heldOptions=options;heldOptions.holdFrame=1;heldOptions.applyModel=0;
Require(GkmsNrUpdate(id,&heldOptions,argv[1])==1,"Hold frame rejected");
context->UpdateSubresource(source.Get(),0,nullptr,fixture.data(),512*8,0);
RenderEvent(GkmsNrQueueEx(id,source.Get(),vectors.Get(),400,101,depthFixture.Get()));
Require(!sessions.at(id)->fault,sessions.at(id)->status.c_str());
const auto heldSnapshot=ReadHalfTexture(device.Get(),sessions.at(id)->original.texture.Get());
auto changedFixture=fixture;for(size_t i=0;i<changedFixture.size();++i)if(i%4!=3)changedFixture[i]=0x3400;
context->UpdateSubresource(source.Get(),0,nullptr,changedFixture.data(),512*8,0);
RenderEvent(GkmsNrQueueEx(id,source.Get(),vectors.Get(),401,101,depthFixture.Get()));
auto heldPixels=ReadHalfTexture(device.Get(),source.Get());Require(heldPixels==fixture,"Held scene changed with incoming live colors");
for(auto v:ReadHalfTexture(device.Get(),sessions.at(id)->motion.texture.Get()))Require((v&0x7fff)==0,"Held frame used live motion vectors");
heldOptions.colorStrength=4;heldOptions.maxRatio=8;heldOptions.codec=3;
Require(GkmsNrUpdate(id,&heldOptions,argv[1])==1,"Held settings update rejected");
context->UpdateSubresource(source.Get(),0,nullptr,changedFixture.data(),512*8,0);
RenderEvent(GkmsNrQueueEx(id,source.Get(),vectors.Get(),402,101,depthFixture.Get()));
Require(!sessions.at(id)->fault,sessions.at(id)->status.c_str());
Require(ReadHalfTexture(device.Get(),sessions.at(id)->original.texture.Get())==heldSnapshot,"Settings update replaced frozen frame");
Require(ReadHalfTexture(device.Get(),source.Get())==fixture,"Apply-model off failed to show held original");
heldOptions.holdFrame=0;
Require(GkmsNrUpdate(id,&heldOptions,argv[1])==1,"Unfreeze rejected");context->UpdateSubresource(source.Get(),0,nullptr,changedFixture.data(),512*8,0);
RenderEvent(GkmsNrQueueEx(id,source.Get(),vectors.Get(),403,101,depthFixture.Get()));
Require(ReadHalfTexture(device.Get(),source.Get())==changedFixture,"Unfreezing did not resume the live scene");
std::cout<<"PASS: held frame survives live scene changes and parameter replacement; zero held motion; apply toggle; unfreeze\n";
auto& display=*sessions.at(id);display.config.applyModel=1;display.config.comparisonSplit=1;display.config.compareMode=2;display.config.comparePosition=.25f;display.config.compareSwap=1;
const auto before=ReadHalfTexture(device.Get(),display.original.texture.Get());const auto enhancedPixels=ReadHalfTexture(device.Get(),display.resolved.texture.Get());
display.Composite(source.Get());auto preview=ReadHalfTexture(device.Get(),source.Get());
for(unsigned y=0;y<512;++y)for(unsigned x=0;x<512;++x){const auto i=(y*512+x)*4;for(unsigned c=0;c<3;++c){const auto expected=x>=126&&x<=129?(c==2?0:0x3c00):(x<128?enhancedPixels[i+c]:before[i+c]);Require(preview[i+c]==expected,"Movable swapped wipe is incorrect");}}
display.config.compareMode=1;display.config.compareZoom=1;display.Composite(source.Get());preview=ReadHalfTexture(device.Get(),source.Get());
Require(preview[0]==0&&preview[1]==0&&preview[2]==0,"Side-by-side fit did not letterbox");
display.config.compareLabels=1;display.config.compareZoom=2;display.Composite(source.Get());preview=ReadHalfTexture(device.Get(),source.Get());
for(auto v:preview)Require((v&0x7c00)!=0x7c00,"Compare labels/zoom generated invalid output");
display.config.comparisonSplit=0;
for(unsigned debug=1;debug<=3;++debug){display.config.debugView=debug;display.Composite(source.Get());preview=ReadHalfTexture(device.Get(),source.Get());for(auto v:preview)Require((v&0x7c00)!=0x7c00,"Debug view generated invalid output");}
const auto displayDone=++display.value;Check(display.immediate->Signal(display.fence11.Get(),displayDone),"Display test completion");display.immediate->Flush();display.Wait(displayDone);display.gpuPending=false;
CheckDebug(device.Get(),display.device12.Get());std::cout<<"PASS: movable/swapped wipe, aspect-preserving side-by-side, zoom, labels and three debug views\n";
