# 接入资料与依赖

- Gkms.Sdk 2.0.4：本地 NuGet 源还原，运行时由宿主提供；SDK 和 ImGui.NET 不随包复制。许可证见 SDK 包内 LICENSE。
- NVIDIA NGX：NgxApi.h 的 ABI 声明对应 https://github.com/NVIDIA/DLSS/tree/main/include 中公开的 NGX 1.5 定义。当前直接调用用户配置目录中的 `nvngx_dlssnr.dll`，不经系统 NGX core 创建 feature。
- NR feature 18、`DLSSNR.*` 参数与历史重置的调研参考：https://github.com/Kuan-Mi/UnityDLSSNR 、https://github.com/ysamtme/dlss5-nr-media-poc 。这些是社区实验性接入资料，并非 NVIDIA 公开保证的 NR 合约。
- DX11/DX12 共享纹理调研参考：https://github.com/NIGos/dlss5-bridge 。本项目独立实现纹理转换和同步，不捆绑该插件、ReShade 或 RenoDX。

本包不含 NVIDIA 模型、`nvngx_dlssnr.dll`、修改版运行库或驱动。不会修改驱动、注册表、游戏 API 或宿主渲染 Hook。第三方运行库需要另行提供并验证兼容性。

## 社区接入与本地测试依赖

- [RenoDX-DLSS5-Generic](https://github.com/Yukikaze20170315/RenoDX-DLSS5-Generic)：MIT；NgxApi.h 的本地参数对象参考其 `bridge_params.hpp` OwnedParameters 设计，参数键参考 `dlssnr.hpp`。许可证全文保留于下方。
- [OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)：GPL-3.0。0.2.0 起，ColorPipeline.h 直接包含其 `OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl`（dlss-neural-rendering 分支，2026-11-23 获取），包括曝光测量、sRGB/高光代理、区域降采样、匹配残差、双分支亮度合成、OkLab 色相修正和色域保护。适配改动：C++ 字符串分段嵌入、模型输入 alpha 固定为 1；宿主调度、资源与 Unity 接入在本项目实现。本项目衍生集成按随附 GPL-3.0 LICENSE 提供，安装包内 source.zip 包含对应源码与构建脚本；NVIDIA 专有运行库不在其中。NrCaller.cpp 为独立的函数转发实现。
- [dlss5-nr-media-poc](https://github.com/ysamtme/dlss5-nr-media-poc)：另一个公开的调用者检查及直接接入参考。
- 已测试模型下载来源：[社区兼容版 310.9.16](https://github.com/RankFTW/rhi-repo/releases/tag/dlssnr-310.9.16)。解压后的 `out\nvngx_dlssnr.dll` 未签名。只确认本机独立测试通过；不为第三方发布者提供来源或安全背书。
- 对照用的 [NVIDIA 签名版 310.9.16](https://github.com/RankFTW/rhi-repo/releases/tag/dlssnr-310.9.16-nvidia) 已验证 Authenticode 签名有效，签名者 NVIDIA Corporation；保留为 `out\nvngx_dlssnr_310.9.16-nvidia.dll`。两个下载均来自第三方发布页，插件 ZIP 不再分发模型。

## RenoDX MIT notice

MIT License

Copyright (c) 2025 Carlos Lopez Jr.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
