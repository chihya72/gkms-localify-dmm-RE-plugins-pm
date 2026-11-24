# pm 个人插件仓库

本仓库是 RE 兼容的 pm 个人插件订阅源，在一个 GitHub 仓库中管理两个插件的源码、订阅索引和 GitHub Actions 工作流。安装包发布到本仓库的 [Releases](https://github.com/chihya72/gkms-localify-dmm-RE-plugins-pm/releases)：

- [DLSS5 源码](gkms-dlss5)，目录 `gkms-dlss5`。
- [照片替换支持源码](gkms-localify-dmm-swap)，目录 `gkms-localify-dmm-swap`。

克隆本仓库即可取得两个插件的完整源码，无需另外克隆源码库或初始化子模块。两个子目录是普通文件夹，各自包含项目文件、构建脚本和许可证，可以独立编译；整个项目只有外层一个 Git 仓库。仅订阅安装插件无需下载源码。

插件源码仓库包含 RE 插件源码、构建与离线测试及许可证，不包含旧 DMM 注入器、缓存、截图、模型或第三方 NR 运行库。

## 构建与订阅

在 RE 的「订阅源」添加：

```text
https://raw.githubusercontent.com/chihya72/gkms-localify-dmm-RE-plugins-pm/main/pluginmaster.json
```

同目录的 `index.json` 也可使用。安装后插件默认停用，按需在「已安装」启用。

向 `main` 推送任一插件文件夹或发布工作流的修改后，GitHub Actions 自动下载已发布的 RE SDK 2.0.4，编译两个插件并执行离线测试，然后创建包含两个 ZIP 的 Release。Release 发布成功后，机器人提交更新根目录的 `index.json` 和 `pluginmaster.json`，下载链接指向对应 Release 附件。现有订阅入口无需更换。

工作流为 [Build, test and publish PM plugins](https://github.com/chihya72/gkms-localify-dmm-RE-plugins-pm/actions/workflows/publish.yml)，也可通过 **Run workflow** 手动发布。Pull Request 只构建和测试，不发布。每次发布使用独立的 `pm-build-<运行编号>-<重试次数>` 标签；插件自身版本仍取自各自的 `plugin.json`。

CI 执行 DLSS5 原生与托管编译、托管测试，以及照片替换的离线功能、软件 DX11 预览和安装包隔离加载测试。DLSS5 的本机 GPU 测试可用其 `Build.ps1 -Test` 运行；真实 NVIDIA 模型和游戏实机验证不属于托管 CI 的覆盖范围。

也可以在克隆后的仓库根目录分别编译，两个插件互不依赖：

```powershell
& .\gkms-dlss5\Build.ps1 -SdkFeed 'D:\PluginWork\sdk-feed'
& .\gkms-localify-dmm-swap\Build.ps1 -SdkFeed 'D:\PluginWork\sdk-feed'
```

将示例 `SdkFeed` 替换为存放 RE 的 `Gkms.Sdk.2.0.4.nupkg` 的目录。需要 Windows x64、PowerShell 7、.NET SDK 8+；DLSS5 还需要 Visual Studio C++ x64 工具和 Windows SDK。编译无需游戏、模型文件或另一个插件的构建产物。

仓库不再提交编译后的 ZIP，也不需要根目录的构建、导出或本地服务脚本。各插件文件夹内的 `Build.ps1` 保留，支持独立编译。此工作流只更新 PM 仓库的 Release 和订阅索引。

## 修改插件资料

修改各插件文件夹中 `plugin.json` 的 `name`、`punchline`（列表简介）、`description`（详细说明）、`author`、`version`、`category`、`tags`、`names`（多语言名称）和可选的 `repositoryUrl`，提交推送后由 Actions 重新构建发布并更新订阅索引，无需手工编辑索引。`id` 是安装和配置的标识，不宜作为显示名称修改。发布插件更新时提高插件版本，并保持项目文件的程序集版本与 manifest 的插件版本一致。

当前 DLSS5 的 ID 为 `pm.dlss5`、版本为 `0.3.0-alpha.2`；照片替换支持的 ID 为 `pm.photoswap`、版本为 `1.1.2`。两者的作者字段均为 `pm`。完整当前展示资料在 `index.json`，各插件 manifest 另含入口、参数和热更新能力。

旧 ID `gkms.dlss5`、`chinosk.photoswap` 与新 ID 属于不同的安装条目。使用新包前先停用旧包，不要同时启用。旧配置仍保存在旧 ID 下；需要沿用时，在游戏关闭后将 `profile.json` 的 `settings` 对象中对应旧 ID 的值复制到新 ID（已有新配置时优先保留）。许可证及第三方声明继续保留。

DLSS5 仍为实验版，需要用户自备适配的 NR 运行库；其原有硬件、运行库及实机验证限制见子仓库 README。原有许可证及第三方声明随各插件保留。
