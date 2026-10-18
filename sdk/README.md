# Applepie SDK

本文对应 **Applepie Manager v0.2.2**。管理器发布版本与 SDK ABI 独立编号；当前基础 ABI 为 **1.1**（兼容 1.0 插件），各服务版本为 1。

SDK 由 `applepie_manager.dll` 提供，全部 C 接口统一在 [applepie.h](applepie.h)。平台为 Windows x64、MSVC C11/C++17、8 字节 pack。公开边界只传固定 POD、opaque handle、函数指针和模块期 context，不传 STL/CRT 对象、C++ 异常或分配所有权。

新插件可以直接使用 [C++ 客户端封装](applepie_client.h)；需要终末地常用共享 Hook 时，再包含 [可选适配器](applepie_endfield_adapters.h)。底层辅助头包括 [准确签名入口](applepie_native.h)、[SEH](applepie_seh.h)、[指针槽作用域](applepie_pointer_scope.h)和[终末地共享数据契约](applepie_endfield_contracts.h)。C API 继续开放，可以按需混合使用。

开发新插件或扩展功能可使用 [Applepie 插件开发 skill](../skills/applepie-plugin-development/SKILL.md)；迁移现有插件可使用 [Applepie 插件迁移 skill](../skills/applepie-plugin-migration/SKILL.md)。将对应的完整技能文件夹放入 `$CODEX_HOME/skills`（未设置时为 `~/.codex/skills`），调用 `$applepie-plugin-development` 或 `$applepie-plugin-migration` 并提供插件源码/目标目录与 SDK 目录；其他工具也可以直接读取 `SKILL.md` 执行。技能本身不包含 SDK 或游戏地址。

## 从可编译示例开始

[minimal_plugin.cpp](examples/minimal_plugin.cpp) 是完整的插件，包含只读 Manifest、Attach、异步 Enable、逻辑停用和再次启用。它只 Hook 自己导出的 `APH_ExampleTarget(int)`：启用时返回 `value + 11`，停用时恢复原调用结果 `value + 1`。不需要游戏、IL2CPP、角色、偏移量或写权，也不安装自己的 MinHook。

在 MSVC x64 环境运行：

```bat
sdk\examples\build.bat
sdk\examples\build.bat /verify
```

第一条编译基础示例 `sdk/examples/bin/applepie_example.dll`、配置示例 `applepie_config_example.dll`，以及跨插件服务示例 `applepie_service1.dll`（提供方）和 `applepie_service0.dll`（使用方）；没有配置编译环境时脚本尝试通过 vswhere 寻找 MSVC。第二条还会编译并运行[离线验证宿主](examples/verify.cpp)和[配置验证](examples/config_verify.cpp)，链接本仓库真实 Core 与 MinHook 后端，在独立进程中加载示例 DLL。它不启动游戏、不部署文件，也不向其他进程注入。单独分发 SDK 时，普通示例构建只需 SDK 头文件及示例源码；`/verify` 另外需要本仓库的 `src/`、MinHook 依赖，以及示例目录中的 `abi_verify.c`、`hook_thunks.asm`（由 MSVC 的 `ml64` 编译）。这些验证源码不参与正式 Manager DLL 构建，生成的测试日志可以删除。

旧插件迁移时，先保留地址解析、准确函数签名和业务回调，将初始化移到 Enable，再把安装流程换成下面的客户端接口。游戏状态恢复及后台任务取消由插件实现。

## 可选 C++ 客户端

只需包含 `applepie_client.h`，不需要链接客户端库，也没有静态初始化时的运行操作。接口位于 `applepie` 命名空间：

| 接口 | 负责的流程 |
|---|---|
| `Client::Attach / Service<T>` | 校验 ABI、保存 owner、按需获取服务表；失败不覆盖已有输出表 |
| `Config::Register / Notify / Status / Read / Save` | 注册基础字段与保存完成通知，读取快照、异步保存；应用配置由插件自己负责 |
| `NativeHook<Tag, Function>` | 准确签名入口与普通独占 Hook 的申请、准备、original 发布、激活和状态查询 |
| `Hook` | 插件提供手写入口时的安装流程，保留特殊 ABI 和控制逻辑 |
| `SharedHook` | 任意自定义合约的共享申请，区分 provider/member 并完成激活 |
| `Subscription` | Core 通知或共享 Hook 的订阅；显式 Close，保留 Pending 句柄 |
| `Client::RegisterLifecycle / CompleteEnable / Disable / Checkpoint` | 对接生命周期服务，不替插件决定线程、写权、恢复逻辑或当前 epoch |

普通 Hook 的实际用法见示例中的 `SampleHook`。`Request` 提交申请；反复调用**非阻塞**的 `Advance`，返回 `APH_OK` 才表示激活完成。它不会创建线程或 Sleep。示例自己创建短期初始化线程；已有插件可以使用自己的工作线程推进。受管回调中不能等待安装、释放或排空。

`NativeHook` 的业务回调通过 `Original(...)` 调用原函数，保留原调用阶段、重入与 SEH 计数；独占替换可以选择不调用 original，调用两次会报错。默认没有 Runtime、当前角色、Animation/Camera 或常驻观察者要求。需要额外准入时显式传 `NativeOptions.begin/end`；domain/access_flags 只是传给这组回调的参数，不授予权限。`automatic_ready=false` 允许先推进多项安装，再用 `PublishReady()` 发布已经验证好的业务组。原始 C++ 异常仍应在插件自身语言边界处理；默认不会吞掉 native/original 异常。

手写入口使用 `MakeHookRequest` + `Hook::Request`，将 `owner()/handle()/api()` 绑定到自己的常驻上下文，再调用 `Advance(&original)`。入口仍须按准确原型配对 `enter_call/leave_call` 并设置 SEH 边界。`MakeSharedRequest` + `SharedHook` 同样接受任意新目标、新合约和手写适配器；成员可以省略 native entry，`Advance()` 不要求 original。无需为每一种新插件修改 Core。

资源与并发规则：

- `Client` 在 Attach 中配置一次，服务表按需获取并缓存到调用方。首次 Request 完成后再把注册对象交给其他线程；控制方法的并发调用返回 `APH_E_BUSY`，由调用方稍后重试。高频 native 入口不调用这些安装方法，不增加客户端注册表扫描、服务获取或安装轮询。
- 每个 `NativeHook<Tag, Function>` 的 Tag/签名组合只能绑定一个目标。同一请求可重试；换 owner、key、目标或业务配置会拒绝。`Hook`、`SharedHook`、原入口上下文和 original 槽均须保持模块期生命，不放在短期栈上，也不重新覆盖仍被占用的 original。不要用底层安装调用绕过同一绑定的封装状态机。
- `Retire()` 是永久释放申请，不是热停用；调用后不会自动重新申请，已启用的 trampoline/context 继续常驻。常规停用走生命周期服务，再启用保留 native claim。共享 provider 即使不再参与业务，入口仍服务其他插件，不能清空它的 original。
- `Subscription::Close()` 返回 Pending 时保留句柄和回调上下文，直到返回 OK。对象析构不会隐式退订或卸载 DLL。再次启用时先关闭旧订阅、确认排空，再 Listen，获得新 epoch/generation；仅 Core 已确认过期的代次可以视为已关闭。
- 查询、申请及失败返回保留 `APH_Result` 和原始状态；失败、超时不会伪造释放成功或清空不确定资源。需要撤销已申请的 Hook 时显式 Retire，并继续查询/排空。

slot 使用 `Client::Service<APH_PointerApi>` 获取一次表，再复用 `APH_WithPointerPatch`。原 wrapper 和预期值检查保留，现有 Core 的票据及写权要求仍适用。窗口、Runtime、写权等可选服务也通过 `Service<T>` 获取原表；客户端不会自动解析游戏对象或增加隐式权限。

## 可选终末地共享适配器

`applepie_endfield_adapters.h` 提供 `applepie::endfield::SetMainI32<Tag>`、`SetMainBool<Tag>`、`BipedIK<Tag>`、`SMCUpdate<Tag>`、`GameAppUpdate<Tag>`、`MorphJob<Tag>`、`SpecialMorphJob<Tag>` 和 `CreateBindingByActionId<Tag>`。它们只处理 native 调用和事件分发，不含偏移量、反射、当前角色、线程认证或写姿态业务。

例如，对**已验证准确原型**的 SMC Update 地址：

```cpp
using Smc = applepie::endfield::SMCUpdate<10>;
static applepie::Subscription smcEvents;
// client 已 Attach；OnSmc 是本插件的 APH_SharedCallback。
APH_Result r = Smc::Request(client, 10, validatedUpdate);
if (r < 0) return r;
// 首次安装登记订阅；后续由工作线程仅推进 Advance。
r = Smc::Listen(smcEvents, OnSmc, context);
if (r != APH_OK) return r;
r = Smc::Advance(); // Pending 时稍后再推进，OK 才完成。
```

首次提供适配器的插件成为 provider，其 original 在激活前保存；后来的插件只激活自己的逻辑 claim。SMC、SetMain 和 Job 保持 original 一次；BipedIK 仅允许显式登记的唯一 controller 在 BEFORE 返回 `APH_SUPPRESS`。普通 observer 没有抑制权。成员无需调用 original，也不能把借用 payload 当作游戏写权。

`GameAppUpdate` 沿用 AntiKick、EndfieldVR 的 `endfield.GameApp.Update` version 1 契约：`void(self*, MethodInfo*)`，payload 为 `APH_EfVoid`，原函数执行一次。Manager 的菜单在 AFTER 阶段处理初始化和 UI 事件；使用该入口的插件应共享同一契约。

`CreateBindingByActionId` 沿用 EIEM 已用的 Windows x64 IL2CPP 原型 `void*(self, actionId, callback, int32_t priority, MethodInfo*)`，按 `APH_EF_INPUT_BINDING` version 1 共享，原函数执行一次。`APH_EfInputBinding` 的 `result` 在 BEFORE 为 null，在 AFTER 是原返回值（也可能为 null）；订阅者不能改返回值或跳过原函数。监听捕获本身不要求 Animation/Camera 写权，后续调用捕获的游戏委托仍需自己的线程、生命期和业务权限判断。

字符串、委托及返回对象均为本次调用借用引用；SDK 不持有 GC root，不判断绑定是否仍然有效，也不补发安装前的绑定。保存引用或支持晚启用的插件须自行处理所有权、绑定代次和当前状态发现。成员按各自启用 epoch 订阅，停用后停止业务观察，provider 的 native 入口继续为其他成员分发。

迁移已有共存组合时，应一起核对相关插件的入口契约，并覆盖两种加载顺序。只把一方换成 Shared，不能让另一方的私有 MinHook 或独占请求自动成为成员；对同址可组合的观察逻辑优先统一共享契约，不放宽 Core 的重复安装检查。

不同 SetMain 的 i32/bool 版本不能混用；两个 Job 适配器保留显式返回缓冲区与依赖参数，AFTER 仅表示**调度调用返回**。插件仍需验证当前游戏构建的真实 ABI，支持范围之外的函数使用通用 `SharedHook` 和手写入口。离线参数测试不能代替游戏内签名与时序验证。

分发返回错误时，适配器沿用 native SEH 故障边界并向外传播，不静默继续业务，也不重放 original；发生在 BEFORE 时尚未调用 original，发生在 AFTER 时保留已经调用一次的事实。

## 可选基础配置

`APH_SERVICE_CONFIG` / `APH_ConfigApi` v1 提供开关、32 位整数、小数、枚举、单行短文本和按键。字段和枚举选项使用动态列表，没有固定数量上限。插件可以只把简单设置交给 Manager；复杂文件、分组、条件联动、自定义界面和配置迁移继续由插件处理。

快捷键声明为 `APH_CONFIG_KEY`，默认值和快照使用 `integer` 保存单个 Windows VK（0 表示未绑定，范围 0..255），无需声明数值范围。Manager 显示键名，点击后按键录入，Esc 取消，清除可解除绑定；录入成功后自动保存并通知插件。捕获使用面板窗口的键盘消息，失焦取消、忽略持键重复，并避免录入 Home 时触发面板开关。此类型不注册全局热键，按键响应仍由插件处理。文件继续存储十进制键值，原有整数键位可直接读取；声明此类型需要配套支持按键字段的 Manager，旧 Config 实现会拒绝未知类型。

完整可编译示例见 [config_plugin.cpp](examples/config_plugin.cpp)。C++ 使用 `applepie::Config`；C 或其他语言通过 `copy_service` 获取服务表。它是可选的新服务，未改变 ABI 1.1 的已有布局；旧 Manager 可能返回 `APH_E_UNAVAILABLE`，插件应明确处理缺少服务或使用自己的默认配置。

1. 在 `APH_Attach` 或初始化 worker 中调用一次 `register_schema`。字段声明稳定 `key`、标签、说明、类型及默认值；整数/小数声明包含端点的范围，文本声明最大 UTF-8 字节数，枚举提供稳定选项字符串。注册会复制全部内容并读取磁盘，不在游戏 Hook 或渲染回调中注册。
2. `snapshot(..., 0, NULL)` 只读取字段数、修订号和保存状态；按 `field_count` 分配值数组后再次读取完整快照。字段顺序在本次进程中不变，schema、值和枚举选项不向调用方转移内存所有权。`field()` 的 `options` 返回 NULL；用 `option()` 按索引复制选项。
3. 注册后、首次保存前，用 `set_notifier(owner, callback, context)`（C++ 为 `Config::Notify`）登记一次保存完成通知，再主动读取一次初始快照。回调签名为 `void APH_CALL(APH_Owner, void*)`；在通知中读取最新快照并更新插件自己的缓存或唤醒工作线程。业务循环直接读取本地缓存，不定时查询配置修订号。
4. `save(owner, revision, count, values)` 提交完整事务并复制输入。返回 Pending 后，成功或失败都会触发完成通知；成功写盘后才整体发布新值并递增修订号。通知只表示有保存完成，后续保存可能已经开始，因此读取的是当前快照。保存中返回 Busy，旧修订号返回 Stale；同步拒绝不会发通知。不要在游戏回调中等待保存。

通知是可选的模块期能力：需要 Manifest 声明 `APH_PLUGIN_MODULE_OBSERVER`，回调须位于本 owner 的 DLL，回调及 context 保留到进程退出。通知在保存 worker 上按 owner 串行执行，不持有配置状态或注册表锁；可重新读取快照，不能等待另一条通知、调用游戏方法或从 ABI 抛出异常。插件在正常游戏阶段应用缓存和完成旧动作恢复。通知不授予游戏访问、角色或写入权限，逻辑停用期间仍可更新纯配置缓存；不提供热卸载、通知替换或注销。通知入口有 SEH 与模块在途计数，故障 owner 不再收到后续通知。

`set_notifier` 是 v1 表尾的可选扩展；原 48 字节前缀继续兼容。新客户端连接旧配置服务时，该成员为 NULL，`Config::Notify` 返回 Unavailable。依赖通知的插件应明确检查此能力，不能退回定时查询。

Manager 位于游戏设置菜单的 **Applepie Manager** 页签。插件条目中的按键、开关、枚举和恢复默认值操作完成后自动保存；文字和数值在原生输入弹窗确认后保存。保存中暂停该插件的配置编辑；失败显示具体错误和仍然生效的旧值，重新编辑可再次提交，不定时重试。保存使用线程池任务，磁盘 I/O 不在 GUI、游戏回调或 Core 控制线程上执行。逻辑停用后仍可编辑已注册配置，再次启用读取最新快照；磁盘保存使用宿主复制的数据，完成后才调用可选通知，与插件业务 epoch 无关。

原生界面复用 `GameSettingCtrl` 的页签、开关、下拉框、按钮及通用输入弹窗。Manager 校验 `LuaManager.LoadLua(System.String)`、`Tick(System.Single)` 和 `DoString(System.String)` 的运行时原型后，通过 Core 安装内部入口；窗口路由录入按键，异步状态/保存完成只设置通知标记，游戏线程按事件更新界面。空闲帧不读取配置快照或执行界面 Lua。脚本嵌入 DLL，无独立 ImGui 窗口；这是 Manager 内部界面实现，插件 SDK/配置 ABI 保持不变。

持久文件位于**插件 DLL 所在目录的 `config/applepie.<插件ID>.ini`**，以 UTF-8 `key=value` 保存，文本首尾空格和等号会保留。缺少值使用默认值；类型/范围不合法的已有值按字段回退，并通过 `defaulted_fields` 返回数量。结构损坏、重复 key、非法 UTF-8 或读取失败会拒绝注册并保留原文件。保存先写同目录临时文件再替换；失败保留旧文件、旧值和修订号，返回 `APH_E_IO` 与 Win32 错误，详情可在 Diagnostics 中查看。未知 key 会保留，注释不作为持久状态保留。运行时不监听文件变化，菜单的“刷新状态”读取当前内存快照；外部文件修改在下次启动时读取。

注册成功后，Core 会在同目录生成 `applepie.<插件ID>.schema.json`，供桌面管理器在游戏外显示字段和按键控件。描述包含字段默认值、范围、选项以及 DLL 文件大小与修改时间；插件更新后，旧描述失效，下一次注册会刷新。生成描述失败仅记日志，不影响配置注册，也不修改 INI。插件无需增加接口或轮询。桌面编辑保留注释和未知键，并在保存前检查外部修改；游戏内保存仍使用上述 SDK 持久化规则。

Manager 自身使用同一配置读写实现，文件为 `plugin/config/applepie.manager.ini`，模板位于 [`templates/config/applepie.manager.ini`](../templates/config/applepie.manager.ini)。`manager` 是此文件保留的配置 ID，业务插件不能用该 ID 注册配置。语言和 UID 选项为扁平字段，插件启动偏好为 `plugin.<小写 DLL 文件名>=true/false`。旧的分节 TXT 配置仅在新文件缺失时导入一次，新文件始终优先；旧文件保持原样，之后不再写入。启动时读取一次，游戏内修改通过异步保存写回，无文件轮询。

键为 1..47 个 ASCII 字母、数字、点、下划线或连字符；标签/说明分别最多 95/159 个 UTF-8 字节，枚举值最多 63 字节，短文本最多 255 字节。字符串必须 NUL 结尾，不接受换行、控制字符或非法 UTF-8；数值不接受 NaN/Infinity。枚举按字符串持久化，调整选项顺序不会改变已保存的含义。字段及选项总数按实际需要分配，资源不足时返回错误。

配置 C ABI 布局：`APH_ConfigValue=272`、`APH_ConfigOption=64`、`APH_ConfigField=616`、`APH_ConfigSchema=16`、`APH_ConfigSnapshot=32`、`APH_ConfigApi=56` 字节（原 v1 前缀 48 字节）；仍为 Windows x64、pack 8。

## 服务入口与 ABI

| 服务 ID | 表 | 用途 |
|---|---|---|
| `APH_SERVICE_HOOKS` | `APH_HookApi` | 独占 Hook、两阶段激活和 native 入口计数 |
| `APH_SERVICE_SHARED` | `APH_SharedApi` | 同址单一 provider、typed 回调和显式 controller |
| `APH_SERVICE_POINTERS` | `APH_PointerApi` | PE 指针槽的预期值交换与条件恢复 |
| `APH_SERVICE_RUNTIME` | `APH_RuntimeApi` | 运行时、当前目标和线程票据 |
| `APH_SERVICE_WINDOWS` | `APH_WindowApi` | 公共 subclass、owner 路由和标量命令 |
| `APH_SERVICE_LIFECYCLE` | `APH_LifecycleApi` | 异步启停、恢复及目标切换 |
| `APH_SERVICE_WRITES` | `APH_WriteApi` | Animation / Camera 写权 |
| `APH_SERVICE_PLUGINS` | `APH_PluginServiceApi` | 版本化跨插件同步服务 |
| `APH_SERVICE_DIAGNOSTICS` | `APH_DiagnosticsApi` | 错误详情查询与上报 |
| `APH_SERVICE_CONFIG` | `APH_ConfigApi` | 可选基础配置、内存快照与异步持久化 |

关键 x64 布局：`APH_Api=48`、`APH_HookApi=72`、`APH_HookRequest=240`、`APH_LifecycleApi=56`、`APH_WriteApi=56`、`APH_LifecycleDesc=32`、`APH_Cleanup=48`、`APH_WriteRequest=40`、`APH_WriteStatus=56`、`APH_WriteCall=32` 字节。

ABI 1.1 的新增布局：`APH_PluginInfoData=728`、`APH_Dependency=88`、`APH_PluginServiceApi=40`、`APH_ServiceDesc=112`、`APH_ServiceQuery=144`、`APH_DiagnosticsApi=24`、`APH_ErrorDetail=392` 字节。原 `APH_Manifest=160` 和 `APH_Api=48` 保持不变。

Runtime v1 的可选尾部 `probe_open(owner)` 用于高频检查模块/运行时状态，避免调用含资源统计的 `owner_status`。原 40 字节前缀兼容，新表为 48 字节；先零初始化，再按返回 `size` 和函数指针检测支持。probe 不授予业务、线程、目标或写权限，不能缓存其成功结果。

## 插件依赖与版本化服务

旧的 160 字节 Manifest 不变。需要声明插件版本或依赖时，另导出只读 DATA `APH_PluginInfo`，类型为 `APH_PluginInfoData`，`format=1`，Manifest 的 `required_minor=1`，旧 Manager 会明确拒绝。没有该导出的旧插件仍可加载，版本按 0 处理。

- 版本使用 `APH_VERSION(major,minor,patch)`，每段 0–65535。最多 8 个依赖，以稳定 ID 匹配；最小版本包含、最大版本不包含，最大版本为 0 表示无上限。
- 必需依赖必须处于 Active，才执行使用方的 DllMain、Attach 和 Enable。缺失、被禁用或版本不符显示 WaitingDependencies 及原因；不会擅自启用配置中关闭的提供方。提供方手动启用成功后，等待者继续。
- `APH_DEP_OPTIONAL` 缺失或版本不符均不阻塞，不承诺可选加载顺序；使用方需要处理查找失败，并可稍后重新查找。必需依赖环在执行 DLL 前拒绝。
- 面板要求先停用仍在 Active/Enabling 的必需依赖使用方，再停用提供方。提供方自身停用或故障时，服务调用立即失效，Loader 随后请求停用使用方；不自动重启使用方。生命周期不支持停用的使用方会标记 NeedsRestart。
- 目录启动时扫描一次。新增、替换 DLL 或修改版本声明后重启；常驻 DLL 不执行热替换。

`Client::Service<APH_PluginServiceApi>` 取得服务表。提供方在本次 Enable 中 `publish` 名称、major/minor、当前 epoch、线程策略、回调及 context；只有 Active 后可被查到。使用方 `find` 指定提供方 ID、服务名、major、最低 minor，再用 `invoke` 传自己的当前 epoch。

调用同步执行，Core 锁外调用提供方，参数和结果各最多 4096 字节、16 字节对齐的 POD 副本。消息内不传裸指针、STL/CRT 对象或跨调用借用引用。服务自行约定 operation、结构布局和结果语义。只有回调返回 OK 才复制结果；无异步排队，最多 16 层嵌套。服务记录按需扩展，撤销或旧启用期结束且在途调用排空后复用。

Core 同时计入使用方和提供方的在途调用，停用关闭新调用并等待已进入的调用返回后才恢复插件。`withdraw` 有在途调用时返回 Pending，返回 OK 才能释放 context；不要在回调内等待自己的排空。再发布取得新句柄，旧句柄返回 Stale。发生提供方停用、失败或重启时，使用方要处理失败并重新 `find`，不能持有提供方原始函数指针绕过调用入口。

`APH_SERVICE_ANY_THREAD` 只表示该接口允许在调用线程执行；`APH_SERVICE_GAME_THREAD` 额外检查 Runtime 的 Live 状态及已验证线程，不负责调度，也不授予目标/动画/相机写权。SEH 规则与其他 SDK 回调相同：`recovery=NONE` 为默认，只有严格只读回调才可声明 READ_ONLY；不能把写坏内存承诺为可恢复。

可编译用法见 [service_plugin.cpp](examples/service_plugin.cpp)。提供方实现纯整数服务，使用方声明依赖并导出 `APH_ExampleCompute` 演示业务期调用。

## 错误详情与日志

面板显示结果名称、失败阶段、依赖原因及 Win32 错误；点击插件状态行可查看 Hook/资源、目标地址和后端状态。SDK 的 `APH_DiagnosticsApi::last_error` 读取最近错误，`report` 供插件补充自己的初始化/业务阶段。日志与详情只用于诊断，不代替 Hook 状态和 owner 状态的判断。异常码、地址、阶段和线程继续从 `APH_OwnerStatus` 获取；SEH 过滤器不做文件 I/O。

`plugin/applepie_core.log` 和 `plugin/applepie_manager_log.txt` 各保留当前文件及 `.1`、`.2`、`.3`，新写入文件每份上限 1 MiB（升级前已有的大文件保留原样，随轮换淘汰）。启动时保留上次非空日志，写满时继续轮换，记录带本地时间。文件占用/权限导致轮换失败时停止该实例的文件写入并发出 OutputDebugString 提示，保留当前日志，不截断旧证据；历史编号可能出现空缺。

## Manager 的 IL2CPP 边界

内置 UID 与原生设置共用受保护的 VM 启动边界：先安装共享 shutdown 保护，等 corlib 与既有 VM 线程出现后才 domain_get/attach；只在元数据工作期间附加自己创建的线程，随后 detach。可选 [Il2CppBootstrap](applepie_il2cpp.h) 提供这一流程的基础函数，采用它的插件仍需核对目标游戏构建的无分配探针及初始化顺序，并提供退出互斥。

面板仅提交标量设置；UID 查找、SetActive、字符串和 GC handle 操作在实际 TMP 回调中执行，并核对 Unity 窗口所属线程。退出共享回调关闭准入并等待元数据/业务区间退出；CoreReady、窗口存在和导出地址本身均不作为 GC 已就绪的证明。独立测试只能检查这些边界与调用顺序，真实游戏的 ABI、时序和隐藏效果仍须实测。

运行 `sdk\examples\verify_foundation.bat` 可在独立宿主验证依赖图、版本化服务、并发排空、UID 线程边界和日志轮换；构建的测试 DLL 只放在该宿主的隔离目录。

## 启动与接入

1. DX/Vulkan 代理保持原系统导出转发，只在线程入口加载 **EXE 所在目录**下的 `plugin/applepie_manager.dll`，调用 `APH_Bootstrap(1)`。Manager 的 DllMain 不启动业务。Bootstrap 按本进程该 Manager 实例只执行一次，返回 `Pending`、`OK` 或失败；`OK` 表示 CoreReady，不表示扫描已经完成。轮询 `APH_GetBootStatus` 区分 Starting、CoreReady、Scanned、BootFailed。
2. Core 注册表、控制线程、MinHook 与 API 表先准备好，再扫描 DLL。`[plugins] filename.dll=0` 或 `false` 不加载；未列出的默认开启。配置使用 UTF-8（可带 BOM），文件名不区分大小写，路径不依赖当前工作目录。路径使用 `MAX_PATH` 有界路径，过长路径拒绝。
3. 插件必须导出只读 **DATA** `APH_PluginManifest`。结构是固定长度 POD，没有指针。Manager 使用 `SEC_IMAGE_NO_EXECUTE` 映射检查清单，不执行 DllMain；没有该清单的文件视为普通依赖，不主动 LoadLibrary。ABI/结构长度/架构/ID 不符合要求的文件不加载。文件名排序决定重复稳定 ID 的接入顺序，已有 owner 的 ID 不再 Attach。
4. `LoadLibraryExW` 成功后，Manager 再核对加载后的清单、取得 `APH_Attach` 和 `APH_Enable`。每个有效模块 Attach 一次。业务模块与目标模块在进程期保留，不执行 FreeLibrary 热卸载。
5. Attach 接收 owner token 和进程期 API 表。Attach 可准备 Hook 和订阅，但不激活 Hook。必要能力满足后调用 `APH_Enable(owner, activation_epoch)`；只有它返回 `APH_OK` 且 owner 未故障，才记为 Active。返回 Pending 不被当成成功；异步启用必须先登记 Lifecycle，再用 `complete_enable(owner, epoch, result)` 完成异步启用；迟到结果拒绝。Enable 可在独立初始化线程轮询准备结果，不能让工作永远等待自己。

当前只发布 `CORE | HOOKS | NOTICES`。GAME、GUI 是保留位；Manager 自用 IL2CPP/UID 初始化和 Runtime 的 Live 状态均不会把它们标成已验证。需要这些位的插件可完成 Attach，但保持 WaitingCapabilities，不调用 Enable。可选能力缺失不阻止纯工具插件启用。最小目标/线程票据通过可选服务取得，不等于完整 GAME/GUI 能力。

最小导出声明：

```cpp
extern "C" __declspec(dllexport) const APH_Manifest APH_PluginManifest = {
    sizeof(APH_Manifest), 1, 0, APH_ARCH_X64, 0,
    APH_CAP_CORE | APH_CAP_HOOKS, 0, "example.my_plugin", "My plugin"
};
extern "C" __declspec(dllexport)
APH_Result APH_CALL APH_Attach(const APH_AttachArgs* args);
extern "C" __declspec(dllexport)
APH_Result APH_CALL APH_Enable(APH_Owner owner, uint64_t activation_epoch);
```

DllMain 和静态初始化必须被动。不要自行再扫描、加载业务插件、安装受管 Hook，或在找不到 Manager 时退回私有 Hook。Windows 的 [DllMain 限制](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices) 同样适用；代理只创建工作线程并立即返回，不在 Loader Lock 内等待它。

## 生命周期

Manifest 的 `APH_PLUGIN_LIFECYCLE` 表示支持本文的启停恢复契约；`APH_PLUGIN_LATE_LOAD` 声明支持运行中首次加载；`APH_PLUGIN_MODULE_OBSERVER` 允许登记常驻观察者。后两项必须与 `APH_PLUGIN_LIFECYCLE` 一起声明。标志不代替游戏 ABI 或时序验证。

首次启用：Manager 管理线程先用不可执行映射预检 Manifest，再执行 `LoadLibraryExW → Attach 一次 → 等待必要能力 → Enable(epoch)`。加载和准备不在 DllMain 或 UI 回调中等待。已加载模块保留至进程结束；停用不调用 FreeLibrary。

Attach 中复制服务、登记 `APH_LifecycleDesc` 和模块资源。`timeout_ms` 范围为 100–60000；`restore` 必须有效，context 保留至进程结束。Enable 可以同步返回 OK，或返回 Pending 后由初始化线程调用 `complete_enable`。结果只能完成当次 Enabling epoch；旧 epoch、已禁用/故障/超时后的结果返回 Stale。基础 GAME/GUI 能力位仍未发布，Runtime.Live 不等于这两个大能力已就绪。

正常停用经历 `Active → Disabling → Disabled`；取消尚未完成的启用经历 `Enabling → Disabling → Disabled`。两者的 `disable()` 都返回 Pending，重复停用在 Disabling 时返回 Pending、在 Disabled 时返回 OK。

取消不会强制终止线程。`disabling` 可能与 `APH_Enable` 或初始化 worker 并发：插件需要唤醒自有等待，并持续检查取消状态及当次 epoch；`restore` 必须确认 worker 真正退出后才能返回 OK。Core 等待已准入的 `APH_Enable` 返回，取消先于准入时不再调用它。初始化的 OK、Pending 或失败结果都不能覆盖 Disabling、Disabled 或新 epoch；启用截止时间从进入 Enabling 开始计算，迟到结果不重置停用截止时间。

停用与取消共用下列排空流程：

1. Core 先原子关闭 owner 业务准入，关闭当期通知、普通共享订阅、窗口命令与写入口，清除尚未执行的旧命令。
2. `disabling` 在控制线程通知客户端，仅取消插件自有任务、改标量与 UI 状态，不调用 Unity。它不代表恢复完成。
3. 已准入的业务、写作用域、临时 PointerPatch 和清理访问必须真实返回。客户端在已验证的游戏线程、符合其业务恢复条件的常驻 native/window 阶段调用 `checkpoint`；该函数不等待。
4. Core 在注册表锁外调用受限 `restore`。有待释放 domain 时，Runtime 必须 Live、TargetReady，且 runtime epoch、target generation 和游戏线程仍匹配旧 lease。仅当前 restore 回调能使用清理准入；它不重新打开普通业务。
5. `restore == OK` 且相关计数/资源确实排空后，才清除 lease 并发布 Disabled。Pending / wrong-thread 留待安全阶段；失败、目标失效或超时转 NeedsRestart，保留不确定记录与占用。

恢复确认只适用于该次 checkpoint 捕获的 domain、lease ID、activation epoch、runtime epoch 和 target generation。回调在锁外执行时，另一个 domain 可能因 release 或并发 Disable 才进入 Releasing；该 domain 必须保留，等待下一次自己的 restore。若 RELEASE 回调执行期间开始 Disable，它不能直接完成停用；须等待 epoch 关闭、`disabling` 通知真实返回及后续 DISABLE checkpoint，正常待办返回 Pending，不误隔离为 NeedsRestart。

无游戏资源的纯工具可以由控制线程完成 domains=0 的恢复，无须 Actor。若插件无写 lease 但仍持有只读 Unity root，restore 必须自行返回 Pending，等验证线程释放。超时不得伪装成已排空。禁止把旧 AP_PluginDisable 的布尔返回值当作恢复完成的证据。

再启用仅从已确认 Disabled 开始，生成新 activation epoch，重新登记业务订阅/路由并刷新上下文。native Hook 与 provider 不重装；首次 Attach 不重复。启用本身没有 Animation 或 Camera 权限。状态新增 Disabling=8、Disabled=9，所以故障判断必须用 `APH_OwnerHasFault`，不能使用 `state >= Failed`。

## 独占 Hook

通过 `api->copy_service(owner, APH_SERVICE_HOOKS, 1, sizeof(table), &table)` 取得 Hook 表。检查返回值和 table 的 size/版本，不能假定取得 API 就意味着某个游戏绑定可用。

- RequestHook 提供唯一 `request_key`、target、准确签名的 native_entry、PE timestamp/SizeOfImage、native 签名字符串和恢复契约。`APH_ModuleBuild` 只帮助读取模块身份，**不证明目标语义/签名正确**。解析地址、更新构建支持和准确 ABI（包括隐藏参数、返回 ABI）由插件负责。
- target 和 entry 必须是已加载 PE 模块中的可执行地址，entry 必须属于 owner 模块。模块被 pin。Core 不支持匿名/JIT 代码目标。timestamp/SizeOfImage 是构建匹配检查，不是加密证明。
- 原子地查询并预留地址；一致的 owner/key 重试返回原 claim；同址独占、签名、模式冲突分别返回明确错误。相同 key 改参数返回 `E_KEY_REUSED`。诊断名不参与身份。基础 Hook 表仍对共享模式返回 `E_MODE`；共享申请必须使用 Shared 服务。
- 新请求返回 Pending 与 claim。控制线程执行 MH_CreateHook，query 返回 Prepared 与 original 后，插件才保存该准确类型的 original 和 entry 上下文。一个 entry/context 只绑定一个仍占用的目标，不能为第二个目标覆盖同一个 original 槽；Core 拒绝复用这样的 entry。
- `activate_hook(owner, claim, saved_original)` 核对 original。控制线程通过注册表锁获得发布顺序，然后启用；查询到 Active 才表示 patch 已启用。owner 的业务准入只有 Enable 成功后才打开，准备中入口直通 original。
- 后端变更都走同一控制路径，注册表锁外调用 MinHook。回调中只提交/查询，不能等待退出自己。每个 claim 合并为一个有界待办槽；Release 优先于该 claim 尚未执行的 Activate/Create。
- 入口 JMP/thunk 本身不视为外部补丁，交由 MinHook 判断可否重定位；Core 按请求地址登记，不自动追跳合并地址。激活前核对申请时保存的入口字节，MinHook 已创建冲突明确报告；期间入口变化或后端启停/移除状态不明时冻结后续后端变更。申请前已有的第三方改写无法仅凭跳转指令区分，这些检查不保证与未接入 Core 的补丁链兼容。

`release_hook` 立即关闭该 claim 的业务准入，异步处理后端。未启用的 Prepared claim 成功移除后为 Released，可让出地址；**曾启用的 claim 只禁用、保留 trampoline 和目标占用，为 RetiredPinned**。原因是补丁入口到 SDK enter_call 之间仍存在窗口，计数为零不能证明所有线程已离开机器指令。Core 不转授这种地址、不释放 provider 代码和 original，query 在在途计数非零时仍给出 Pending。RetiredPinned 的 OK 只确认 patch 禁用及已计数访问排空，不授予释放 native entry 上下文的权利。

claim 属于模块期；epoch 属于本次启用。基础生命周期只启用一次；登记生命周期服务后，每次再启用使用新 epoch，常规禁用保留 native claim，不能通过 `release_hook` 模拟可再启用。失败的 Attach/Enable 关闭 owner 准入，异步清理该 owner 的已登记准备项；不操作其他 owner。清理失败保留记录与资源，显示 NeedsRestart。

## 共享 Hook

`APH_SharedRequest.native.mode` 必须为 `APH_SHARED_TYPED`。同址请求经同一个 Core 地址注册表和真实 MinHook 控制线程，不会各插件分别 patch。除了 PE timestamp/SizeOfImage 和 native 签名，还必须一致匹配 contract 名、版本、payload 大小及 original policy；冲突分别返回 `E_BUILD`、`E_SIGNATURE`、`E_CONTRACT` 或 `E_MODE`。相同 owner/key 改参数返回 `E_KEY_REUSED`。签名字符串用于协作核对，不能证明机器码的真实原型。

首次请求必须带 owner 模块中的准确 native entry，成为 provider；后续成员可不提供 entry。首次选择只决定实现位置，不授予业务优先权。provider 模块、original 和 entry 上下文保持常驻，不随业务 owner 的释放而转交给其他插件。

接入顺序：

1. `request` 返回 claim；`listen` 登记本 owner 的 BEFORE/AFTER 回调、角色与恢复契约。
2. provider 轮询到 Prepared，保存准确类型的 original 和固定上下文，再 `activate(..., saved_original)`；成员传 `nullptr` 激活自己的逻辑 claim。等待 Active 不能发生在受管回调里。
3. provider 的 POD 入口执行 `enter → publish(BEFORE) → original → publish(AFTER) → __finally leave`。原调用必须使用该目标的真实参数/返回类型；Core 不做万能函数指针调用。
4. native 激活确认前不分发业务。相同 native claim 的递归只直通 original，不重复业务阶段。entry 的调用计数与 listener 的业务计数分别维护。

`APH_ORIGINAL_ONCE` 下 original 恰好一次。只有 `APH_CONTROLLED_VOID` 合约允许跳过 void original，而且只能由唯一 `APH_CONTROLLER` 的 BEFORE 回调返回 `APH_SUPPRESS`；第二个 controller 明确冲突。observer 不能抑制 original 或修改返回值。订阅顺序不构成控制权；controller 抑制后，AFTER 的 `original_suppressed` 为真，适配器的 original 阶段仍是 NotEntered。

每次 `publish` 将至多 256 字节 payload 复制到 16 字节对齐的栈快照；回调只在调用期间借用它。payload 内的指针仍是借用标识，并非深拷贝、GC root 或游戏写入授权。需要写游戏的业务另行验证 runtime/target/thread；跨阶段需要自己保存并重新验证票据。Core 在注册表锁和共享服务锁外逐个调用插件，各回调独立检查 owner epoch/准入并执行 SEH 边界。

`unlisten` 和 `release` 立即关本方准入，不等待正在执行的自己；在途回调未返回时为 Pending。回调上下文必须保留到排空。逻辑 claim 的 Released 只说明本方订阅释放情况，不是机器码已无人执行或模块可卸载的证明。其他成员存在时，即使 provider 的业务 owner 发生可隔离故障/释放，其纯 native 适配器仍服务其他成员。最后成员离开后沿 native Hook 路径退役；曾启用的 trampoline/目标占用保留到进程结束。

适配器/original 异常通过 filter-safe `native_fault` 隔离整个 site，并继续异常传播；不会调用第二次 original。回调仅在声明且满足只读契约时允许可继续的读 AV 隔离。写/执行 AV、未知状态、C++ 异常等不伪装成只读恢复。安全故障 owner 的成员由控制线程清理；NeedsRestart 状态保留无法确认的资源。所有业务必须在自己的语言边界处理实现层 C++ 异常；原调用异常不可因该处理而被吞掉或重试。

## Runtime 与当前目标票据

状态从 Waiting 到 Live，再到 Stopping、Dead，不恢复到 Live。只有一个合作式 evidence source；其他 owner 无法抢占 source。Core 检查 Live 声明线程确为调用线程，目标绑定确在已记录线程；**原生回调是否真的属于受支持游戏 ABI，由 source 的接入代码及证据负责，Core 不靠线程号推断。**

`APH_Access` 包含 owner activation epoch、runtime epoch、target generation，flags 分别要求 Runtime、Target、GameThread。目标失效/重建改变 generation；开始停止改变 runtime epoch 并清空目标和线程；旧票据、未绑定目标、错误线程和失效 owner 被拒绝。标识值不授予对象解引用权限。`flags=0` 仅检查 owner/epoch，供纯插件工作使用。

仅收到窗口消息不能声明游戏线程。客户端须从已核对元数据的 native 边界取得证据，确认当前线程及停止边界。当前控制器需要 GC root；entity/Animator 字段来自实际元数据，Animator 还须核对 native `m_CachedPtr` 和 `GetInstanceID`。在 SetMain 前先关闭目标访问并恢复旧写域，Pending 期间保留旧目标清理上下文，恢复成功后才使旧代次失效并重新绑定，详见[目标切换契约](#目标切换与常驻观察者)。对象消失/变化/实例重建会停止旧播放和缓存，需在新目标重新 Play。

`Application.Internal_ApplicationQuit` 前只发 Stopping；`il2cpp_shutdown` 返回后才发 Dead。selected source 的常驻 shutdown 适配器即使业务 owner 失效仍可关闭 runtime。Stopping/Dead 拒绝新增 Hook 请求和激活、游戏票据及 PointerPatch；已准入的 original 可正常退出。窗口关闭消息不替代这些证据。EIEM 和 Manager 的已解析 IL2CPP export 包装器在检查到关闭状态后拒绝新调用；这不是对尚未迁移的所有历史 native 访问的全面证明。

## 目标切换与常驻观察者

只有选中的 resident source 能推进 Runtime/Target。该 source 的业务停用不会撤销其模块身份；Live 仍须由元数据验证的 native 边界和真实游戏窗口线程共同确认。shutdown 或 ApplicationQuit 至少有一个停止边界；只有实际 shutdown original 返回才证明 Dead。Stopping/Dead 禁止新的 IL2CPP 访问。

`APH_MODULE_OBSERVER` 共享订阅和 `APH_WINDOW_MODULE_OBSERVER` 窗口路由跨业务 epoch 保留，只用于 Runtime/target 观察、取消旧工作及 checkpoint；不能冒充业务写入口。被禁用 owner 所提供的共享 native 适配器仍服务其他活动订阅者，递归按契约直通 original。

在旧角色仍有效的 SetMain BEFORE 阶段，source 调用 `prepare_target_change`，先关闭普通目标访问和两个 domain 的新写入，再尝试恢复旧拥有者。`Pending` 表示异步排空尚未完成：保留旧 root、恢复记录和 lease，从后续已验证的 resident observer/checkpoint 重试，不阻塞、取消或重放游戏原函数，也不提前绑定新目标或调用 `APH_TARGET_INVALIDATE`。新的控制器候选单独持有；只有所有旧写域恢复成功，才撤销旧 target generation，重新发现并绑定当前最新目标。先恢复的空闲 domain 在整个目标切换完成前也不可转授。

等待期间，普通 snapshot 的 `target_ready=0`，旧目标命令/票据验证失败；仅 Core 的受限 restore 回调取得旧清理上下文。其 deadline 不因重复 Pending 延长。真正恢复失败、超时、对象失效或 Runtime 停止时，仍保留不确定占用并要求重启。未走 SetMain 的对象重建也须核对实体、Animator、native cached pointer 与实例 ID；托管 GCHandle 并不保证 Unity 原生对象仍然存活。

通知/共享订阅/窗口路由的已关闭且排空记录允许同 owner 复用，但每次生成新的不透明句柄。窗口、路由、共享注册表和 PointerPatch 记录均按需扩展；Core Window 待执行命令队列仍为 128 项，满队列返回明确错误。客户端复制命令输入，Core 消息只带有界标量或槽身份，不能借用 UI/网络栈指针。

## Animation 与 Camera 写权

`APH_ANIMATION=1` 和 `APH_CAMERA=2` 分别只有一个全局拥有者。UI 的播放、冻结、相机接管等明确动作在已验证的游戏线程调用 `request`，携带 flags=7 的当期 `APH_Access`。另一个 owner 已占用时返回 Conflict；同一 owner/epoch/target 的重复请求返回原 lease。

持有 lease 还不等于当前调用可写。每个写入入口必须 `enter → 业务 → __finally leave`，跨 domain 按进入顺序逆序离开。同线程最多 32 层，call serial/owner/lease 必须匹配栈顶。`current(owner)` 仅报告当前线程被准入的 domain（包括受限恢复）。申请新 lease 不能发生在任意后台线程。已有 lease 的原生 worker 只可用 flags=3 进入有证据的 POD 工作区；不因此获得 Unity/IL2CPP 线程权限。

`release` 立即关新写入并返回 Pending；原 lease 保持 Releasing，等待上述 checkpoint、还原及排空。只有 Free 才能转授他人。Uncertain 不可重试抢占或清空。共享 Hook 订阅、角色捕获、模块观察者、只读 GUI 及 Enable 均不暗授写权。Core 约束已适配入口，不能拦截任意第三方内存写入，不提供通用 WriteMemory。

最终写权申请准入和 Disable 的关闭/扫描使用同一 lifecycle/writer 锁，锁序固定为该锁再到 Core registry。申请在锁内重新通过 owner 原子准入门与 Runtime validation：申请先赢时，新 lease 必被随后 Disable 扫描；Disable 先赢时，预检曾通过的旧申请仍被拒绝。客户端回调均在锁外运行。

## 指针槽 PointerPatch

申请必须携带三个 access flags 和当前票据。slot 必须对齐、位于已加载 PE 的可写、非执行数据页，模块构建匹配；expected/replacement 必须来自已加载 PE 的可执行页，相关模块被 pin。当前不改页保护，不提供任意 WriteMemory 或 IAT 专用后端。

Core 占用 slot 后，以原子 CAS 执行 `expected → replacement`。不匹配返回 `E_EXPECTED`，不改值；同 slot 嵌套/竞争（包括同 owner）返回 Busy。申请在真正交换前重新检查票据，并在整个 lease 期间持有 owner 准入计数。临时和持久 lease 都需显式 release；owner 故障控制线程不会在 wrapper 运行中提前恢复 slot。

临时调用使用 [applepie_pointer_scope.h](applepie_pointer_scope.h) 的 `APH_WithPointerPatch`：保留原 wrapper 调用，`__finally` 无论正常或异常都尝试 release。wrapper/original 异常记录 Running 并继续传播，绝不重试或制造成功。release 只 CAS `replacement → expected`；别人改过的值不覆盖。无法确认恢复则保留 Uncertain claim、准入计数和资源并报 NeedsRestart，不允许强制抢回 slot。恢复是对已 pin PE 内存的清理，可在目标失效或 runtime 停止后执行，不调用 Unity。

成功恢复的记录可带新 generation 复用；旧 handle 不会重新变成有效申请。EIEM 的三处包装调用全部走同一个 scope，仍用 `il2cpp_runtime_invoke(GetHumanPose, ...)`，保留 Get 槽临时替换为 Set 的原包装语义；expected 来自明确解析的 Get 地址，不接纳任意当前槽值。wrapper 返回后再次核对票据，拒绝把旧目标操作记作成功。

## 窗口路由

每个 HWND incarnation 只有一个 Core `SetWindowSubclass`。同线程可以直接绑定；跨线程请求由 Core 常驻 worker 安装目标线程的 `WH_GETMESSAGE`，在目标窗口线程完成绑定。`subscribe` 返回 Pending，`query` 的 Bound 才确认完成。worker 必须常驻，不能依赖短命申请线程维持 message hook。

普通消息按 `BEFORE → DefSubclassProc 一次 → AFTER` 流转，原返回值保持；回调没有劫持窗口返回值的通用权限。`WM_CLOSE` 可取消，不设 RuntimeDead、不清理游戏；`WM_NCDESTROY` 才退役该窗口代次、清掉排队命令。再次创建的窗口即使复用 HWND 也有新 incarnation。取消订阅只撤销本 owner 路由；Core subclass 保留到销毁，不猜测/覆盖其他模块的 WndProc 链。原窗口过程异常不重放。

`post` 复制有界标量命令，禁止借用指针。命令记录 owner epoch、window generation 和 access；入队检查除 GameThread 之外的要求，目标线程执行前再验证全部要求。旧目标、旧 owner、销毁窗口和取消订阅的命令丢弃；队列满返回 QueueFull。`flags=0` 的纯文件/音频命令仍受 owner 和窗口生命期约束。订阅回调本身没有 Unity 主线程写权限，业务必须另外验证。私有注册唤醒消息只用于 Core 队列。

## 准确签名入口与异常

Core 无法从函数指针证明 entry 有保护；接入者必须提供故障注入证据。每个 native entry 使用准确原型，调用 `enter_call`，并在 POD 薄层的 `__finally` 中调用 `leave_call`。计数包括不做业务的 native 入口；同一线程递归进入同一个 claim 时跳过业务，仍保持准确 original 调用。owner 准入关闭位和业务在途数在同一个原子 CAS 值中协调，避免“已关闭但随后才加计数”。

`APH_HookCall.flags` 必须按位检查：`APH_CALL_BUSINESS` 表示本 owner 可做业务，`APH_CALL_RECURSIVE` 表示递归，`APH_CALL_NATIVE_READY` 仅表示 native 激活已确认，不能替代业务准入。这个额外标志允许共享 provider 的常驻适配器在其业务 owner 失效后继续服务其他 owner；Prepared 阶段仍不分发业务。

`APH_EntryFilter` 只允许**声明并满足只读契约的插件前/后阶段**的可继续读 AV 恢复。这里的只读阶段不能留下游戏写入、锁、待恢复修改或未释放资源。函数签名、参数和返回值仍由准确适配器保存。original 分 NotEntered / Running / Returned：仅前置阶段失败且 original 尚未调用时，允许一次正常调用；Returned 后保留原结果；Running 中发生异常绝不重试。前置恢复后执行 original 也要有自己的 SEH 边界。

过滤器仅关闭 owner 准入并写固定大小首个故障记录（owner、claim、异常地址/码、原调用阶段、线程）；不取注册表锁、分配内存、写日志或调用插件。后续不安全故障可将已有可恢复故障升级为 NeedsRestart。控制线程再输出诊断和处理清理。

写/执行 AV、original 异常、非连续异常等继续交给外层异常链；堆/栈损坏、fail-fast、死锁不属于恢复保证。生命周期 read AV 返回 NeedsRestart，保留未确认状态，不能视为成功回滚。MinHook 的受保护操作发生 AV/页访问异常后按后端状态未知冻结，不声称已恢复。

不要用 `/EHsc` 的 C++ try/catch 代替 SEH。按 [Microsoft 的 SEH 说明](https://learn.microsoft.com/en-us/cpp/cpp/structured-exception-handling-c-cpp)，该模式下硬件异常不保证执行 C++ 局部对象析构。薄层不能依赖那些析构释放 Core 锁或计数。插件 C++ 实现应在它自己的兼容语言/CRT 边界 catch 并转成 `APH_Result`，不能穿过公开 C ABI；宿主不会把 C++ 异常码当作可恢复 AV。

`APH_TypedNative<Tag, R(*)(Args...)>` 为每个真实原型生成独立入口/original 槽，保留编译器的参数与返回 ABI、真正入口 `_ReturnAddress()`、NotEntered/Running/Returned 阶段和 finally 计数。它不是运行时签名解释器；带隐藏 MethodInfo、结构体返回、按值参数的声明仍由客户端验证。标签/entry 不可被另一个目标复用。

插件现存 native body 通过 `Original` proxy 调原函数；重复调用会报告故障，原函数异常继续传播，绝不重试。默认不恢复写入/native 异常。只读恢复沿用明确声明且满足的 只读恢复契约。C++ 实现异常在客户端兼容 CRT 边界转换；`/EHa` 下不能用 catch-all 吞掉硬件或 original 异常。恢复失败留存 root/原值/上下文，不因调用过清理函数就删除证据。

EIEM 的晚安装组先提交请求，在 resident worker 上保存 original、激活并等待 Core ACK，只有组内就绪后发布本地 ready。游戏回调不等待控制线程。失败回退仅作用于已成功申请的本 owner claim，已启用 trampoline/entry 上下文继续常驻。

## 通知订阅

基础通知服务的事件是 Core 能力快照。Subscribe 为本 owner 当前/首次 epoch 建立记录；owner Active 后，控制线程锁外、逐回调 SEH 调用。它是延后通知，不是 Unity 主线程、游戏写入权或共享 native Hook。callback/user_data 必须保留到 Unsubscribe 返回 OK；Unsubscribe 关闭准入，已有回调返回前为 Pending，可以轮询，不能在回调中等待。通知本身不给动画/相机权限。

## 状态、容量和诊断

`owner_status` 给出状态、epoch、受管业务/生命周期在途数、首个故障记录、保留资源数量；`query_hook` 给出准备/启用/释放状态、original、native 在途数、MinHook 返回码。面板分别显示 Loaded、Attached、Capabilities 和实际状态。“启用插件”统一开关异步请求本次启停，请求被接受后自动保存相同的下次启动偏好；请求被拒绝时保留原偏好并显示错误。只有 Active 显示绿色，其他状态显示红色，准备和排空期间保留实际状态名称。

插件目录、owner、原生/共享 Hook、通知、slot、窗口记录及业务服务注册表按实际数量扩展，不设固定注册数量上限；配置、生命周期与诊断记录随 owner 扩展。扩容不移动已发布记录；故障路径通过常驻索引查找，不取锁或分配内存。模块/native claim 不复用；已关闭且排空的同 owner 通知、共享订阅和窗口路由槽可复用记录，每次返回新的不透明句柄。句柄不划分固定槽位索引位数，不可自行解码；旧 ID 仍被拒绝，复用索引节点不产生新的堆分配。注册分配失败返回 QueueFull，加载器明确报告扫描失败，不静默丢弃后续 DLL。同线程受管 native 入口仍最多 32 层，超过 SDK 计数栈时不再分发业务，original 仍依赖永久保留的 trampoline。日志写到 `plugin/applepie_core.log` 和 OutputDebugString，记录操作、owner、claim、target、模式、key、结果；异常过滤器不写日志。Core 与 Manager 的文件日志均按[错误详情与日志](#错误详情与日志)中的规则轮换。

Core 只管理已接入的 Hook、通知、PointerPatch、窗口路由、游戏写权和相关生命周期，不登记文件、网络、音频、独立 UI 或纯数据线程，不提供万能 WriteMemory。当前仅有单一全局 Animation 拥有者与独立 Camera 拥有者。DLL、provider 和已启用 trampoline 均保留到进程结束。

| 资源 | 上限与复用 |
|---|---|
| 加载器 / 基础 Core | DLL 目录、owner、native claim、notice 按需扩展；owner/native claim 不复用；notice 允许同 owner 已关闭且排空后以新句柄复用 |
| Shared | site、logical claim、listener 按需扩展；允许同 owner 已排空的 listener 记录以新句柄复用；TLS 32 层，payload ≤256 字节 |
| PointerPatch | 记录按需扩展；恢复成功才以新句柄复用，不覆盖不确定的占用 |
| Windows | incarnation、route 按需扩展；允许同 owner 已排空的 route 以新句柄复用；128 排队命令（消费后复用） |
| Plugins | 服务记录按需扩展；撤销或旧启用期结束且排空后复用；请求、结果各 ≤4096 字节，嵌套最多 16 层 |
| Diagnostics | 每个 owner 保留一份最近错误详情；历史记录由有界日志保存 |

有界容量耗尽返回明确错误，不能静默覆盖旧记录。客户端自行维护游戏对象的 GC root、原值和恢复记录，Core 句柄不能替代原生对象生命期证明。

## 版本与发行

消费方可自行记录 SDK 版本和头文件 SHA256；`applepie-sdk.lock.json` 是可选记录，Manager 和本 SDK 示例构建不读取或强制校验它。消费方自己的构建或验证脚本可以检查该文件，升级 SDK 时应同步更新其记录。Manifest 的 `required_minor` 按实际需要的 ABI 填写，不使用管理器的 `0.2.2` 发布版本；使用 `APH_PluginInfo` 或要求新增服务可用的插件应声明 ABI 1.1。

升级时验证准确 native ABI、恢复时机、旧 epoch/目标命令失效，以及目标游戏内的插件共存行为。未声明并登记生命周期服务的客户端仍可使用基础加载与 Hook，但不开放运行中首次加载/启停。DLL 升级需要重启进程。
