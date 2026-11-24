#pragma once
// DX11 host adaptation: separable reconstruction filters and final comparison views.
inline constexpr const char* ResizeShader = R"HLSL(
Texture2D<float4> src:register(t0); SamplerState linearClamp:register(s0);
RWTexture2D<float4> dst:register(u0);
cbuffer Settings:register(b0){uint axis;uint filterMode;uint pad0;uint pad1;}
float sinc(float x){x=abs(x);return x<1e-5?1:sin(3.14159265*x)/(3.14159265*x);}
float weight(float x){x=abs(x);if(filterMode==1){if(x<1)return 1.5*x*x*x-2.5*x*x+1;if(x<2)return -.5*x*x*x+2.5*x*x-4*x+2;return 0;}
float a=filterMode==2?2:3;return x<a?sinc(x)*sinc(x/a):0;}
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h,sw,sh;dst.GetDimensions(w,h);src.GetDimensions(sw,sh);if(p.x>=w||p.y>=h)return;
if(sw<=w&&sh<=h){dst[p.xy]=src.SampleLevel(linearClamp,(p.xy+.5)/float2(w,h),0);return;}
float scale=axis==0?float(sw)/w:float(sh)/h;float center=((axis==0?p.x:p.y)+.5)*scale-.5;
float radius=filterMode==0?scale*.5:(filterMode==1||filterMode==2?2:3)*max(scale,1);
float4 sum=0;float total=0;
for(int i=int(floor(center-radius));i<=int(ceil(center+radius));++i){float a=filterMode==0?max(0,min(i+.5,center+scale*.5)-max(i-.5,center-scale*.5)):weight((i-center)/max(scale,1));
int2 q=int2(p.xy);if(axis==0)q.x=clamp(i,0,int(sw)-1);else q.y=clamp(i,0,int(sh)-1);sum+=src.Load(int3(q,0))*a;total+=a;}
dst[p.xy]=sum/max(total,1e-6);}
)HLSL";
inline constexpr const char* ResidualShader = R"HLSL(
Texture2D<float4> model:register(t0);Texture2D<float4> proxy:register(t1);Texture2D<float4> originalProxy:register(t2);RWTexture2D<float4> dst:register(u0);
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h;dst.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;
dst[p.xy]=float4(max(0,originalProxy.Load(int3(p.xy,0)).rgb+model.Load(int3(p.xy,0)).rgb-proxy.Load(int3(p.xy,0)).rgb),1);}
)HLSL";
inline constexpr const char* DisplayShader = R"HLSL(
Texture2D<float4> original:register(t0);Texture2D<float4> enhanced:register(t1);Texture2D<float4> proxy:register(t2);Texture2D<float4> model:register(t3);SamplerState linearClamp:register(s0);
cbuffer Settings:register(b0){float split;float zoom;float debugGain;float whitePoint;uint mode;uint swapSides;uint debugView;uint applyModel;uint flipY;uint encoded;uint labels;uint pad;}
float3 enc(float3 v){v=max(v,0);return lerp(v*12.92,1.055*pow(max(v,1e-8),1./2.4)-.055,step(.0031308,v));}
float3 dec(float3 v){v=max(v,0);return lerp(v/12.92,pow((v+.055)/1.055,2.4),step(.04045,v));}
bool letter(uint ch,uint2 p){if(p.x>=5||p.y>=7)return false;
// O R I G N, five-bit rows, used only for optional ORIG / NR labels.
uint bits[35]={14,17,17,17,17,17,14,30,17,17,30,20,18,17,31,4,4,4,4,4,31,14,17,16,23,17,17,14,17,25,25,21,19,19,17};return ((bits[ch*7+p.y]>>(4-p.x))&1)!=0;}
float4 main(float4 p:SV_POSITION):SV_TARGET{uint w,h;original.GetDimensions(w,h);uint2 q=uint2(p.xy);if(flipY!=0)q.y=h-1-q.y;
float2 uv=(q+.5)/float2(w,h),sampleUv=uv;bool showOriginal=false;bool outside=false;float divider=mode==1?.5:split;
if(mode==1){showOriginal=(uv.x<.5)!=(swapSides!=0);float2 halfUv=float2(frac(uv.x*2),uv.y)-.5;sampleUv=float2(.5+halfUv.x/zoom,.5+halfUv.y*2/zoom);outside=any(sampleUv<0)||any(sampleUv>1);}
else if(mode==2)showOriginal=(uv.x<split)!=(swapSides!=0);
float4 a=mode==1?original.SampleLevel(linearClamp,sampleUv,0):original.Load(int3(q,0));
float4 b=mode==1?enhanced.SampleLevel(linearClamp,sampleUv,0):enhanced.Load(int3(q,0));float3 result=applyModel!=0?b.rgb:a.rgb;
if(debugView==1)result=dec(proxy.SampleLevel(linearClamp,sampleUv,0).rgb);
if(debugView==2)result=dec(model.SampleLevel(linearClamp,sampleUv,0).rgb);
if(debugView==3)result=abs(b.rgb-a.rgb)*debugGain/max(whitePoint,1e-6);
if(showOriginal)result=a.rgb;if(outside)result=0;
if(mode!=0&&abs(p.x-w*divider)<max(2,w*.003))return float4(1,1,0,a.a);
if(mode!=0&&labels!=0){float px=max(1,min(w/640.,h/360.));float origin=uv.x<divider?8:w*divider+8;int2 t=int2(floor((float2(q)-float2(origin,8))/px));
bool tagOriginal=(uv.x<divider)!=(swapSides!=0);uint count=tagOriginal?4:2;
if(all(t>=0)&&t.y<9&&t.x<int(count*6+2)){uint col=uint(max(t.x-1,0))/6;uint ch=tagOriginal?(col==0?0:col==1?1:col==2?2:3):(col==0?4:1);
bool ink=t.x>=1&&t.y>=1&&col<count&&letter(ch,uint2((t.x-1)%6,t.y-1));return float4(ink?float3(1,1,1):float3(.015,.015,.015),a.a);}}
return float4(encoded!=0?enc(result):result,a.a);}
)HLSL";
