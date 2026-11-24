# pm 个人插件仓库

本仓库是 RE 兼容的 pm 个人插件订阅源，管理订阅索引、发布脚本和版本化 ZIP。两个插件的源码分别位于独立仓库，均使用 `main`：

- [DLSS5 源码](https://github.com/chihya72/gkms-dlss5)，本地目录 `gkms-dlss5`。
- [照片替换支持源码](https://github.com/chihya72/gkms-localify-dmm-swap)，本地目录 `gkms-localify-dmm-swap`。

开发时将两个源码仓库放在本仓库对应的子目录；外层 `.gitignore` 排除子仓库源码，提交与推送需分别进行。仅订阅安装插件无需下载源码。

插件源码仓库包含 RE 插件源码、构建与离线测试及许可证，不包含旧 DMM 注入器、缓存、截图、模型或第三方 NR 运行库。

## 构建与订阅

在 RE 的「订阅源」添加：

```text
https://raw.githubusercontent.com/chihya72/gkms-localify-dmm-RE-plugins-pm/main/pluginmaster.json
```

同目录的 `index.json` 也可使用。安装后插件默认停用，按需在「已安装」启用。

在 PowerShell 7 中执行 `Build-Repository.ps1 -Test`。SDK 默认从同级 RE 仓库的 `out\sdk` 读取，也可通过 `-SdkFeed` 指定。它独立构建两个插件、运行测试，再更新外层 `packages`、`index.json` 和 `pluginmaster.json`。不启动游戏，不安装插件，不执行 Git 提交或推送。

运行 `Serve-Repository.ps1` 后，在 RE 插件库添加订阅 `http://localhost:18765/index.json`，或兼容格式的 `http://localhost:18765/pluginmaster.json`。服务只监听本机，关闭脚本后订阅地址不可访问。框架不接受本地文件路径作为在线订阅地址。

在线索引指向根目录；下载路径是相对的 `packages/<程序集>-<版本>.zip`。发布时分别提交并推送插件源码，再提交并推送外层的索引与对应安装包。外层提交不包含子仓库源码，也不修改 RE 框架 Release。

## 修改插件资料

修改各子仓库 `plugin.json` 的 `name`、`punchline`（列表简介）、`description`（详细说明）、`author`、`version`、`category`、`tags`、`names`（多语言名称）和可选的 `repositoryUrl`，然后重新构建。`id` 是安装和配置的标识，不宜作为显示名称修改。保持项目文件的程序集版本与 manifest 的插件版本一致。

当前 DLSS5 的 ID 为 `pm.dlss5`、版本为 `0.3.0-alpha.2`；照片替换支持的 ID 为 `pm.photoswap`、版本为 `1.1.2`。两者的作者字段均为 `pm`。完整当前展示资料在 `index.json`，各子仓库 manifest 另含入口、参数和热更新能力。

旧 ID `gkms.dlss5`、`chinosk.photoswap` 与新 ID 属于不同的安装条目。使用新包前先停用旧包，不要同时启用。旧配置仍保存在旧 ID 下；需要沿用时，在游戏关闭后将 `profile.json` 的 `settings` 对象中对应旧 ID 的值复制到新 ID（已有新配置时优先保留）。许可证及第三方声明继续保留。

DLSS5 仍为实验版，需要用户自备适配的 NR 运行库；其原有硬件、运行库及实机验证限制见子仓库 README。原有许可证及第三方声明随各插件保留。
