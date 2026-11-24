# gkms-localify-dmm-swap（RE 内置换图版）

学园偶像大师 DMM/PC 版的 RE API 2 插件。照片扫描、文件选择、图像适配、前后预览、替换和备份还原全部位于游戏内插件配置页，不需要 Python、Pillow 或单独打开 gkms-photo-swap。

插件 ID：`pm.photoswap`。作者：`pm`。入口程序集：`Gkms.PhotoSwap.dll`。SDK：`Gkms.Sdk 2.0.0`。

## 安装与使用

安装包：`build\pm.photoswap-1.1.2.zip`。

1. 在已部署 RE API 2 框架的插件管理面板中，通过 ZIP 安装入口安装本包，然后启用「照片替换支持」。
2. 拍照后停在「保存到本地 / 保存到相册」界面。
3. 按 Ctrl+U 打开插件配置，点击「重新扫描」，选择刚拍的照片。
4. 点击「选择我的图片」，或直接填写图片路径。支持 JPG、PNG、BMP、GIF、TIFF；GIF 使用静态帧。WebP 暂不支持。
5. 选择「裁切填满」「完整放入」或「拉伸」；完整放入可调整留边颜色。修改适配方式或颜色后，点击「生成预览」。
6. 检查当前照片和替换后预览，点击「自动备份并替换这张照片」。看到完成提示后，关闭管理面板并回游戏保存。

插件启用后自动完成内部初始化，无需额外配置或确认步骤。在游戏自身的保存界面选择保存到相册或本地。

1.1.2 使用新 ID `pm.photoswap`。旧 ID `chinosk.photoswap` 是不同的安装条目，安装本包前请先停用旧包，不要同时启用；旧缓存目录配置可复制到新 ID。专用备份目录保持不变。不要把本 DLL 当作游戏根目录的 `version.dll`。

## 图片处理与文件保护

- 保留每个缓存文件原有的 JPEG/PNG 格式和像素尺寸，原图与缩略图一起处理。
- 用游戏 `_t` 命名规则精确配对缩略图，支持无扩展名的缓存图片；不会仅凭相似前缀把另一张原图当缩略图。
- 支持 EXIF 方向校正。预览来自最终编码后的图片，实际写入保持原尺寸。
- 写入前重新比较缓存原始字节。如果游戏或其他工具已修改缓存，则拒绝写入并提示重新扫描。
- 原图和缩略图同时独占打开，全部检查成功并备份完成后才开始写入。失败时尝试恢复已写入的文件，保留备份；如回滚也失败，会报告备份位置。
- 缓存消失或被游戏占用时，不重新创建缓存或强制覆盖。仅处理 `OutGame`、`Produce`、`MemoryReview` 中的照片。
- 图像处理和文件读写在后台执行，界面显示进度状态。关闭设置页时取消尚未提交的工作；已开始提交的写入会完成或回滚。

图片文件上限 128 MiB、解码尺寸上限 4800 万像素，以控制游戏进程中的内存占用。本插件不修改已经保存到游戏相册的照片。

## 备份和还原

备份存放在当前用户的专用目录：

`%LOCALAPPDATA%\Gkms.PhotoSwap\backups`

每次替换生成独立的时间戳 ZIP，包含原图、缩略图和原缓存目录记录。备份不放在插件安装目录中，因此升级或卸载插件不会删除备份。

在配置页展开「备份与还原」，点击「刷新备份列表」，选择备份并点击「还原选中的备份」。还原要求仍使用原缓存目录、原待保存文件仍存在；还原前也会备份当前图片，方便撤销还原。

旧外部工具 `gkms-photo-swap\backup` 下的目录备份不会自动导入本列表；如需使用旧备份，请通过旧工具还原。本插件不会修改外部工具及其备份。

默认缓存目录：

`%USERPROFILE%\AppData\LocalLow\BANDAI NAMCO Entertainment Inc_\gakumas\07578AE1495C51B9C5AA9A74E48AD00A`

可以在界面中选择其他缓存根目录，点击「保存目录」保存。配置由 RE 管理器保存到 `profile.json` 的 `settings["pm.photoswap"]`；旧版 `config.json` 的参数不会自动导入。

## 构建

需要 Windows x64、PowerShell 7、.NET SDK 8 或更新版本。插件目标框架为 `net6.0`，运行时由 RE 提供。

在本插件文件夹中用 PowerShell 7 运行，无需编译 DLSS5 插件：

```powershell
& .\Build.ps1
```

默认从 PM 总仓库同级的 RE 仓库 `out\sdk` 还原 `Gkms.Sdk.2.0.4.nupkg`。使用其他 SDK 包目录：

```powershell
& .\Build.ps1 -SdkFeed 'D:\PluginWork\sdk-feed'
```

构建脚本还原依赖、编译、运行离线测试、生成 ZIP，并从实际 ZIP 隔离加载设置入口和图像处理依赖。中间产物和合成测试图片位于 `build\re`。脚本覆盖同名本地构建包；对外发布新版前，同时递增项目和清单中的版本。

安装包包含插件 DLL、清单、依赖描述、System.Drawing.Common、Microsoft.Win32.SystemEvents（包括 Windows 运行时资产）及许可证。不包含 SDK、ImGui.NET、cimgui、Host、Manager 或游戏文件。

脚本不安装插件、不修改游戏目录、不启动游戏。旧 `src`、`deps`、`premake5.lua` 等 C++ 资料保留作迁移参考，新构建仅使用根目录的 C# 项目。

## 实现与验证

- `PhotoSwap.cs`：启用插件时通过 SDK 自动注册 `PhotoUtility.GetB` 的两个三参数重载；宿主负责停用和启动失败时清理。
- `PhotoImages.cs`：方向校正、三种适配方式、JPEG/PNG 编码和预览像素。
- `PhotoFiles.cs`：缓存扫描、精确配对、写入前字节检查、双文件备份与恢复。
- `PhotoSettingsUi.cs` / `PhotoFilePicker.cs`：插件内配置、选图和完整换图流程。
- `PreviewTexture.cs`：使用 RE 现有 DX11 设备上传自有预览纹理；不创建游戏图形设备、不 Hook Present、不持有交换链，设置页释放时清理纹理。使用 Windows 的 [GetDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicechild-getdevice) 和 [CreateTexture2D](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createtexture2d) 接口。

验证包括 15 项 Hook/包检查、13 项编辑器检查，以及实际 ZIP 的隔离加载检查；编辑器测试使用合成缓存，覆盖配对、尺寸、三种适配、备份还原、过期缓存、占用、取消、路径约束、真实 DX11 纹理上传及宽窄 ImGui 布局。界面截图由实际 ImGui draw data 离线渲染，位于测试产物目录。

本机 DMM metadata 的相关方法与返回字段已离线核对。**尚未通过实际游戏内验收**：需自行启动游戏验证启用插件后拍照、插件内换图、保存到相册、取消照片、还原及重复启停。离线测试不能替代当前游戏版本的原生调用验证。
