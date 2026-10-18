# ApplepieManager

<p align="center">
  <img src="assets/icons/applepie-manager.png" alt="ApplepieManager" width="160">
</p>

[English](README_EN.md) | 中文

为《明日方舟：终末地》设计的 DLL 插件管理器。Manager 内置 Core，提供插件加载、独占与 typed 共享 Hook、受管指针槽及窗口路由，以及异步逻辑启停和动画/相机独立写权；并提供 UID 隐藏功能。插件接入协议见 [SDK](sdk/README.md)。

## 主要功能

- **插件管理**：CoreReady 后扫描 `plugin\`，核对清单、ABI、重复 ID、必要能力；分别显示加载、接入和活动状态
- **通用 Hook**：动态独占与 typed 共享目标、两阶段激活、同址去重、准确原型与 SEH 薄层；SDK 位于 `sdk/`
- **共享服务**：PointerPatch 条件恢复、统一游戏窗口链、最小 Runtime/目标代次票据
- **启停/写权**：运行时按钮异步加载、启用和停用已适配插件；确认恢复后才显示 Disabled；Enable 不授予动画/相机写权，DLL 保持常驻
- **统一启用开关**：同时请求本次启停并自动保存下次启动偏好；被拒绝的请求保留原设置
- **基础设置**：插件可选注册开关、整数、小数、枚举、短文本和按键；按键显示键名并直接录入，Manager 提供编辑、校验、修改后自动保存及完成通知，字段数量动态分配，复杂配置仍由插件处理
- **UID隐藏**：隐藏游戏内 UID、延迟显示、菜单 UID/名字、名片 UID/名字
- **多语言界面**：Manager 面板中文/英文切换

## 用户协议与免责声明

<details>
<summary>在下载、安装或使用本插件之前，请您仔细阅读本协议。<b>使用本插件即代表您已完整阅读、充分理解并同意遵守以下所有条款。</b></summary>

### 1. 开源许可与最终用户权利
- 本插件基于 **AGPL-3.0** 许可证在 GitHub 平台完全开源。用户可在遵守该许可证的前提下自由使用、修改和分发本插件的源代码。
- 最终用户（End User）在不对本插件进行修改的前提下，使用和分发本插件**不受任何限制**。此权利不因用户是否违反本协议而改变。

### 2. 反欺诈声明
- 您**不得**在网络销售平台公然售卖本插件**软件本体**且未提供 GitHub 仓库地址与售后服务。
- 本插件在 GitHub 平台完全免费开源，如果您是付费购买获取的，请知悉本插件可从 GitHub 免费获取。

### 3. 内容合规与行为约束
- 本插件本身不包含任何游戏美术资产。用户知悉并同意，《明日方舟：终末地》游戏内置的官方动画、场景、模型等资产其版权完全隶属于鹰角网络，并不适用 AGPL-3.0 协议。您**不应该**利用本插件，或利用游戏内置的官方动画、场景、模型等游戏资产，制作、播放或传播任何不合适的动作/动画（包括但不限于色情、暴力、政治敏感等违反法律法规或引起社区不适的内容）。使用者应自行负责确保起拥有任何汇入资源的版权，并遵守其许可和使用条款。

### 4. 风险与免责声明
- 本项目仅供学习、技术研究和交流目的。作者不对使用者如何使用本插件负责，包括任何误用情况。本插件中使用的明日方舟游戏数据资产版权均隶属于鹰角网络。使用本工具可能违反游戏服务条款，存在账号封禁的风险。因使用本插件而直接或间接导致的任何损失（包括但不限于账号封禁、游戏数据损坏等），**本项目不承担任何法律或经济责任**。用户需自行承担所有风险，强烈建议您在测试账号上运行。

</details>

## 安装

您可以在 [Releases](https://github.com/zonemodAE/ApplepieManager/releases) 下载最新发布版或从源代码自行编译。
Release 提供Applepie Manager桌面管理器安装包（`*-setup.exe`）、免安装版（`ApplepieManager.exe`）和独立 DLL。

打开Applepie Manager桌面管理器，选择游戏文件夹后点击“安装 / 更新到游戏”。

也可以手动将三个 DLL 放到以下位置（游戏目录指 `Endfield.exe` 所在文件夹）；从源码构建时，这些文件位于 `bin/`：

```text
applepie_manager.dll   → 游戏目录/plugin/applepie_manager.dll
vulkan-1.dll           → 游戏目录/vulkan-1.dll
d3dcompiler_47.dll     → 游戏目录/d3dcompiler_47.dll
```

> `d3dcompiler_47.dll`（DX）和 `vulkan-1.dll`（Vulkan）只引导 Manager，二者放其一或全放均可。

> 如果您**没有安装过此类型的插件**，您可能需要自行创建plugin文件夹。

## 使用方法

1. 按上述方式安装后点击"连接至塔卫二"启动游戏，或者手动启动游戏。
2. 打开游戏的设置菜单，选择页签列表末尾新增的图标；选中后页面标题为 **Applepie Manager**。
3. 点击插件右侧的“配置”进入设置，在“详情”中查看状态和错误信息。Active 显示绿色，未启用显示红色；“启用插件”同时控制本次启停和下次启动偏好，修改后自动保存。
4. 基础设置修改后自动保存；按键直接按下录入，Esc 取消。
5. 在“管理器设置”中切换语言、调整 UID/延迟/菜单/名片隐藏选项。

已加载的插件 DLL 会保持常驻。更换 Manager 或已加载插件的 DLL 时，先退出游戏，替换后重新启动。

## 插件安装

您可以点击"插件下载"选项卡从 [GitHub 插件目录](https://github.com/zonemodAE/ApplepieManager/blob/master/catalog/plugins.json)下载插件。也可以在插件管理页面进行手动安装。

## 配置与升级

| 内容 | 位置 |
| --- | --- |
| Manager 语言、UID 隐藏和插件启动偏好 | 游戏目录下的 `plugin/config/applepie.manager.ini` |
| 插件通过 SDK 注册的基础设置 | 对应插件 DLL 所在目录下的 `config/applepie.<插件ID>.ini` |
| 桌面编辑器的字段描述 | 同目录的 `config/applepie.<插件ID>.schema.json`，由 Core 在注册设置时生成 |
| Manager 默认配置模板 | 仓库中的 [templates/config/applepie.manager.ini](templates/config/applepie.manager.ini) |

配置使用 UTF-8 扁平 `key=value` 格式，开关写为 `true/false`，无需 `[general]`、`[uid]` 或 `[plugins]` 分节。例如：

```ini
language=zh
hide_uid=true
hide_ping=false
# 禁用指定插件的示例，使用时取消注释并替换为实际的小写 DLL 文件名
# plugin.example.dll=false
```

新安装时 Manager 会自动创建默认配置；需要预设选项时，可将模板复制到 `plugin/config/applepie.manager.ini`。未列出的插件默认启用，游戏内“启用插件”开关会自动保存其启动偏好。复杂配置仍由各插件自行管理。

从旧版升级时，如果新文件不存在，Manager 会迁入旧 `plugin/applepie_manager_config.txt` 中的语言、UID 和插件启用设置。旧文件保持原样；新文件一旦存在，后续只读写新文件。

游戏内修改自动保存；在游戏外手动编辑配置后，下次启动时读取。保存失败会保留原文件，错误信息可在界面和日志中查看。

桌面编辑器保留配置注释和未知键，保存时检查外部修改，防止覆盖较新的文件。启用插件一次后会生成字段描述。描述缺失或与当前 DLL 不匹配时，仍可按现有键值编辑；未注册基础设置的复杂配置由插件自己的界面管理。

## 插件开发

插件可使用 [C++ 客户端封装](sdk/applepie_client.h)，需要常用终末地共享 Hook 时再包含[可选适配器](sdk/applepie_endfield_adapters.h)；底层 [C API](sdk/applepie.h) 继续开放。接入协议与可编译示例见 [SDK 说明](sdk/README.md)。运行 `sdk\examples\build.bat` 编译最小插件，加 `/verify` 可运行独立的离线验证，不会启动或部署到游戏。

开发新插件或扩展功能时，可使用 [Applepie 插件开发 skill](skills/applepie-plugin-development/SKILL.md)，按功能选择 SDK 接口、游戏接口核验和验证方式。

迁移旧插件时，可让编码代理使用 [Applepie 插件迁移 skill](skills/applepie-plugin-migration/SKILL.md)，按实际操作选择接入方式，保留特殊 Hook、slot 和业务恢复逻辑。

安装 Visual Studio 的 C++ 桌面开发工具后，在项目根目录运行 `build.bat`。正式构建生成 `bin/applepie_manager.dll`、`bin/d3dcompiler_47.dll` 和 `bin/vulkan-1.dll`。
