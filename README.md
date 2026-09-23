<div align="center">

<img src="https://raw.githubusercontent.com/DGZSbot/ai-icon/refs/heads/main/WorkBuddy.png" alt="WorkBuddy2API TrafficMonitor Plugin" width="120">

# WorkBuddy2API TrafficMonitor 插件

**把 WorkBuddy2API 服务装进 TrafficMonitor 的任务栏 —— 状态、积分、任务、成本，一眼尽览**

[![Release](https://img.shields.io/github/v/release/Arimayuki03/workbuddy2api-trafficmonitor-plugin?style=flat-square&logo=github)](https://github.com/Arimayuki03/workbuddy2api-trafficmonitor-plugin/releases)
[![License](https://img.shields.io/github/license/Arimayuki03/workbuddy2api-trafficmonitor-plugin?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows-blue?style=flat-square&logo=windows&logoColor=white)](https://github.com/zhongyang219/TrafficMonitor)
[![Host](https://img.shields.io/badge/Host-TrafficMonitor-4C8CBF?style=flat-square)](https://github.com/zhongyang219/TrafficMonitor)
[![API](https://img.shields.io/badge/Plugin%20API-v8-8A2BE2?style=flat-square)](https://github.com/zhongyang219/TrafficMonitorPlugins)
[![Language](https://img.shields.io/badge/C%2B%2B-Win32%20%2F%20MSVC-00599C?style=flat-square&logo=cplusplus&logoColor=white)](https://learn.microsoft.com/cpp/windows/desktop-applications/)
[![Upstream](https://img.shields.io/badge/Upstream-workbuddy--cockpit-6E56CF?style=flat-square)](https://github.com/Arimayuki03/workbuddy-cockpit)

</div>

---

## 这是什么

为 [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 制作的 [WorkBuddy Cockpit](https://github.com/Arimayuki03/workbuddy-cockpit)（workbuddy2api）服务监控插件：

- **任务栏栏位** — 彩点实时反映服务健康（绿/橙/灰/红），文本可配为 `4/4`（健康/总数）、积分合计或纯状态
- **悬浮提示** — 账户实时积分、定时任务、按模型用量、成本汇总，可分区开关控制长度
- **设置窗 5 页** — 服务启停/自启、账户与积分表（含成本台账）、7 类定时任务排程、显示、高级
- **插件命令菜单** — 启动/停止/重启、查询实时积分、立即执行任务、一键开关，右键即达

纯 Win32 实现（不依赖 MFC），静态 CRT，产物单文件 DLL；随带官方插件接口头 API v8（1.8.6 实测）与 nlohmann/json v3.11.3（均 vendored）。

> **必须配合 [Arimayuki03/workbuddy-cockpit](https://github.com/Arimayuki03/workbuddy-cockpit) 使用** —— `/admin` 管理接口（任务开关/立即执行/实时积分/积分冷却热改/账号停用恢复）是该网关独有的；官方上游 [Sliverkiss/workbuddy2api](https://github.com/Sliverkiss/workbuddy2api) 没有这些接口，插件的"定时任务/实时积分"页会提示不可用，只能看服务状态与账户估算积分。

## 目录

- [功能特性](#功能特性)
- [界面预览](#界面预览)
- [快速开始](#快速开始)
  - [前置条件](#前置条件)
  - [从 Release 安装](#从-release-安装)
  - [从源码编译](#从源码编译)
  - [部署到 TrafficMonitor](#部署到-trafficmonitor)
- [使用指南](#使用指南)
- [服务端版本兼容](#服务端版本兼容)
- [状态点含义](#状态点含义)
- [风控防护（三层）](#风控防护三层)
- [已知边界](#已知边界)
- [故障排查](#故障排查)
- [许可](#许可)

## 功能特性

### 🚦 服务状态与栏位

- 彩点 = 服务状态（详见[状态点含义](#状态点含义)）；栏位文本三种模式：`4/4`（健康/总数）、积分合计、纯状态
- 主窗口与任务栏窗口的显示项独立勾选，插件命令菜单随栏位右键出现

### 👥 账户与积分

- **账户表**：估算积分 / 实时积分（剩余（已用/总量））/ 状态（冷却 · 熔断 · 降权(连败) · 模型限额）/ 令牌剩；双击行弹出该号**每模型实测成本台账**（每1k=实测千 token 均价，≤0 即免费，口径同 wb2api 的 `status-report.ps1`）
- **实时积分**：经服务端 `POST /admin/credits` 查询，贴服务端冷却节奏；自动刷新周期保存时热写回服务端
- **账号停用/恢复**：右键账户行——「停用」把该号手动摘出选号池（签到/保活/排程照常，纯对话流量摘除）；「恢复」解除手动停用；「复活」解除系统自动禁用。状态列区分**手动停用 / 自动禁用**双位，停用状态服务端落盘、重启保留
- **强制清除冷却**（v1.10.0）：右键账户行——「清除冷却」把该号冷却/熔断/连败降权/6004 模型级限流表一键归零（不碰手动停用/自动禁用两位），上游若真仍限流会在下一次请求重新学习；需 wb2api ≥ v1.10.0，旧版提示不可用

### ⏰ 定时任务

- 7 类任务独立排程（v1.8.0 起含**任务队列**行，对齐网页端任务中心的"执行队列定时排程"）；勾选=热生效，触发时间可直接编辑并「应用」——写回走服务端 `/admin/tasks` 接口，**免重启热生效**（服务端调度器即时重排定时器）并最小 diff 落盘 config.json（留 .bak）；旧版服务端无该接口时回退直写文件，需重启服务生效
- 「立即执行」= 服务进程内触发，不等排程；任务队列默认关（对全账号执行真实任务动作链、消耗上游配额，同网页端 opt-in 口径）

### 📊 用量可观测

- **在途/限流模型指名道姓**：账户有在途请求时状态列与 tooltip 显示"请求中 glm-4.6×2"；模型级限流直接列模型名（"模型限额 glm-4.6、glm-4.5(至 14:00)"）
- **按模型用量统计**：tooltip"模型用量"区列出每个模型本次服务运行累计消耗的积分与请求数，每行缀上游积分倍率（如"（x0.06）"，目录未下发时整体省略）
- **悬浮窗账户只显实时积分**（v1.9.0）：估算随轮询单调扣减、长期偏差大，小字号悬浮窗宁可少也不误导；实时缓存缺失时该行不显示积分数，需要精确值去设置窗账户表

### 🖥️ 服务管理

- 启动/停止/重启/自启/意外拉起兜底；"停止服务"只动**路径与设置一致**的 `wb2api.exe`，端口被别的程序占用时明确报错而不误杀

## 界面预览

<details open>
<summary><b>任务栏栏位</b> —— 彩点=服务状态，文本可配为 <code>4/4</code>/积分/纯状态（下图为"状态+积分"模式）</summary>

![任务栏栏位](docs/img/taskbar.png)

</details>

<details open>
<summary><b>悬浮提示</b>（完整展开模式）—— 状态、账户（实时积分）、定时任务、积分汇总</summary>

![悬浮提示](docs/img/tooltip.png)

</details>

<details open>
<summary><b>插件命令菜单</b>（右键栏位/托盘菜单内）</summary>

![命令菜单](docs/img/menu.png)

</details>

<details>
<summary><b>设置窗 · ①服务</b> —— 目录/端口/启停/自启/意外拉起</summary>

![服务页](docs/img/settings_service.png)

</details>

<details>
<summary><b>设置窗 · ②账户与积分</b> —— 估算/实时（已用/总量）/状态（冷却·熔断·降权·模型限额）/令牌剩；双击行看成本台账</summary>

![账户与积分页](docs/img/settings_accounts.png)

</details>

<details>
<summary><b>设置窗 · ③定时任务</b> —— 勾选=热生效；触发时间可编辑并「应用」写回；立即执行/全部执行</summary>

![定时任务页](docs/img/settings_tasks.png)

</details>

<details>
<summary><b>设置窗 · ④显示</b> —— 栏位文本模式与悬浮提示开关</summary>

![显示页](docs/img/settings_display.png)

</details>

<details>
<summary><b>设置窗 · ⑤高级</b> —— 管理轮询周期、/admin 可用性、四个快捷打开</summary>

![高级页](docs/img/settings_advanced.png)

</details>

> 实拍于 v1.2.0（账户昵称与积分明细已打码），v1.6.0–v1.9.0 新增的"悬浮窗"列、任务队列行、倍率显示等在对应版本 Release 说明中有截图。

## 快速开始

### 前置条件

| 依赖 | 要求 |
| --- | --- |
| 宿主 | [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)（API v8 头编译，旧版宿主可加载但部分接口静默缺失） |
| 服务端 | **[Arimayuki03/workbuddy-cockpit](https://github.com/Arimayuki03/workbuddy-cockpit)**（v1.2.0 起由 workbuddy2api fork 更名独立演进，网关侧接口契约完全兼容） |
| 服务端开关 | `config.json` 增加 `"admin": { "enabled": true }` 后重启一次服务 |

`/admin` 管理接口（任务开关/立即执行/实时积分/积分冷却热改/账号停用恢复）是 workbuddy-cockpit 独有的；官方上游没有这些接口，插件的"定时任务/实时积分"页会提示不可用。**各插件版本所需的最低服务端版本见[服务端版本兼容](#服务端版本兼容)。**

### 从 Release 安装

1. 到 [Releases](https://github.com/Arimayuki03/workbuddy2api-trafficmonitor-plugin/releases) 下载最新版附件 `WorkBuddy2ApiPlugin.dll`
2. 放入 TrafficMonitor 目录下的 `plugins\` 子目录（没有就新建）
3. 重启 TrafficMonitor，「插件管理」列表出现 **WorkBuddy2API** 即成功
4. 在「显示项目」里勾选 `WB2API 服务状态`（主窗口与任务栏窗口各有独立勾选）

### 从源码编译

需 VS2022（含 C++ 桌面工作负载或 BuildTools）：

```powershell
git clone https://github.com/Arimayuki03/workbuddy2api-trafficmonitor-plugin.git
cd workbuddy2api-trafficmonitor-plugin
powershell -ExecutionPolicy Bypass -File build\build.ps1
```

输出 `build\out\Release\WorkBuddy2ApiPlugin.dll`。

### 部署到 TrafficMonitor

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -TMDir 'E:\软件\TrafficMonitor'
```

部署脚本做"改名式替换"（旧 DLL 先改名挪走再拷入新文件），TrafficMonitor 运行中也能落位新 DLL。**但更新插件后必须重启 TrafficMonitor**——「插件管理→重新加载」不会真正加载新代码：worker 轮询线程持有模块引用把已加载的 DLL 钉在进程里，重载后跑的仍是旧实例（不报错不崩溃），只有重启宿主才生效。

## 使用指南

- **任务栏/主窗口**：彩点=服务状态，文本可配置为 `4/4`（健康/总数）、积分或纯状态；单击栏位打开设置。
- **设置（5 页）**：服务（目录/端口/启停/自启）、账户与积分（实时列含"剩余（已用/总量）"；状态列区分 冷却/熔断/降权(连败)/模型限额；**双击账户行**弹出该号每模型实测成本台账；**"悬浮窗"列**实时反映该号是否出现在悬浮提示明细里，与右键菜单的"悬浮提示显示此账户"同一份开关）、定时任务（勾选=热生效；触发时间可直接编辑并「应用」——经服务端 `/admin/tasks` 接口**热生效**并写回 config.json，旧版服务端回退直写文件、重启服务后生效；立即执行=服务进程内触发；**任务队列**行见下文）、显示、高级。
- **插件命令菜单**：启动/停止/重启、查询实时积分、立即执行/启用各类任务（含任务队列）、三个开关。
- **任务队列**（需服务端 ≥ 2026-09-21 合并版，`admin.enabled`）：对齐网页端任务中心的"执行队列"——扫描全部账号的待办（成长任务 + 开学季闭环）并按账号排队执行。**定时排程默认关**，勾选即热启用并写回 `schedule.queue_enabled`；触发时间默认 10 点（`queue_hours`），可编辑并「应用」写回；「立即执行」不等排程、随时可手动跑一次（撞车 409 提示，与网页端互斥语义一致）。
- **成本台账**：设置窗页②**双击账户行**，弹出该号每模型实测成本（模型｜每1k均价｜样本｜末次观测）。每1k=实测千 token 均价（EMA，≤0 即实测免费），6 小时无观测服务端自动回收；服务端选号按便宜优先，台账解释"为什么总选它"。
- **账号停用/恢复/复活**：页②**右键账户行**——详见上文[功能特性](#功能特性)。停用是独立状态位，与签到解冻、冷却到期等自动复活路径互不干扰。
- **tooltip 控制**：完整展开默认开，硬预算 700 字符（超预算从尾部按区整行舍弃，骨架行必留）；"显示"页可关"定时任务明细"（默认关）/“账户明细”；单账户显隐在页②右键账户行控制。详细预算规则见[已知边界](#已知边界)。

## 服务端版本兼容

插件按需使用服务端新能力，旧版服务端只是对应功能降级或提示不可用，不影响其他功能：

| 插件版本 | 功能 | 最低服务端版本 | 缺失时表现 |
| --- | --- | --- | --- |
| ≥ v1.4.0 | 账号停用/恢复/复活 | **2026-09-20 合并版**（上游 `a20d06f` 临时停用端点 + `/status` 的 `manual_disabled` 双位状态） | 提示不可用 |
| ≥ v1.5.0 | 在途模型 / 模型用量 | **2026-09-20 fork 版**（`in_flight_by_model` 台账 + `GET /v1/stats` 按模型用量聚合） | 降级为纯数量/不可用展示 |
| ≥ v1.7.0 | 模型用量倍率 | **2026-09-19 上游版**（`/v1/stats` 每模型透出积分倍率 `credits`，上游 5009a1f） | 不显示倍率，其余照常 |
| ≥ v1.8.0 | 任务队列定时排程 | **2026-09-21 合并版**（调度器第 7 类 `queue` 任务） | 该行不出现 |
| ≥ v1.9.3 | 触发时间热改 | **2026-09-23 fork 版**（`PATCH /admin/tasks` 支持 `{kind, hours}` + 面板保存配置热应用 `schedule.*_hours`，调度器 `SetHours` 即时重排） | 回退直写 config.json，需重启服务生效 |
| ≥ v1.10.0 | 强制清除冷却 | **2026-09-24 fork 版**（panel 域 `POST /api/accounts/{uid}/clear-cooldown`，冷却/熔断/连败降权/6004 模型级限流表一键归零） | 菜单置灰项点了提示不可用 |

> 仓库 [Arimayuki03/workbuddy2api](https://github.com/Arimayuki03/workbuddy2api) 已更名为 **[Arimayuki03/workbuddy-cockpit](https://github.com/Arimayuki03/workbuddy-cockpit)**，旧地址由 GitHub 自动重定向，本地已克隆的不需要动 remote。

## 状态点含义

| 颜色 | 含义 |
| --- | --- |
| 🟢 绿 | 运行中且至少一个账号可服务（/healthz 200） |
| 🟠 橙 | 服务在跑，但暂无可用账号（/healthz 503，如全部冷却/禁用/在途满载） |
| ⚪ 灰 | 已停止（端口无监听） |
| 🔴 红 | 异常：端口被其他程序占用 / 服务无响应 |

## 风控防护（三层）

1. 常规轮询只打 `/healthz`、`/status`、`/admin/tasks`、`/admin/credits`——全部是**本地服务内存数据，零上游调用**；
2. 实时积分只能经服务端 `POST /admin/credits`，服务端默认 **10 分钟冷却 + 单飞 + 账号间 200ms 限速**；冷却可热改：插件保存"自动刷新周期"时 `PATCH /admin/credits-interval`（60 秒–24 小时，免重启、写回服务端 config.json 留 .bak；设 0=关闭自动刷新时插件不改服务端）；
3. 插件 UI 在冷却期内禁用按钮，积分自动刷新默认关闭；开着时插件**贴服务端冷却节奏**查询（周期再密也只是到点即查，429 由服务端挡，插件不硬顶）。

## 已知边界

- 只监控/控制 **本机回环**上的 wb2api（`/admin` 拒绝非 loopback 来源）。
- 账号"停用/恢复/复活"走服务端内存操作（幂等，重复点击不报错）；停用是独立状态位，对旧版服务端会提示不可用。
- "模型用量"数据来自服务端 `GET /v1/stats` 的**进程内存聚合**：wb2api 重启即清零，插件只展示"本次运行累计"口径，无法回看历史；旧版服务端 tooltip 静默 30 分钟探测一次，其余时间跳过请求。"在途模型"台账同理是运行态（归零即删行、不落盘）。
- 插件更新后「插件管理→重新加载」**不会**加载新代码（worker 线程钉住 DLL 映像，重载静默失效），改配置/换 DLL 后请**重启 TM**；部署脚本已做改名式替换，见"部署到 TrafficMonitor"一节。
- 定时任务勾选的写回会先备份 `config.json.bak`，并只改动目标一行（未知字段/键序保留）。
- 触发时间（`schedule.*_hours`）修改经服务端 `PATCH /admin/tasks` **热生效**（2026-09 合并版起，免重启）；旧版服务端回退插件直写 config.json，需重启服务生效。`*_enabled` 开关与积分冷却（`/admin/credits-interval`）热生效不受版本影响。
- 小程序成长任务（minichat）刻意**不在**"定时任务"页出现：它不进 wb2api 的 taskKind 枚举、不经 `/admin/tasks` 透出，只能在 wb2api 的启动菜单里手动触发。任务队列（queue）则相反：它**是**调度器第 7 类任务（2026-09-21 合并版起），故 v1.8.0 起在"定时任务"页有独立一行，排程开关缺省 false 与服务端默认一致。
- TM 只拒载 `GetAPIVersion() <= 0` 的插件，**没有**"声明版本与宿主版本比较"的检查。本插件按 v8 头编译，宿主版本更低的 TM 会照常加载，但其不支持的接口（如 v7 以下不调用 `OnInitialize`）静默缺失，表现为部分功能降级；遇到异常请升级 TM。
- TM 弹"遇到不适当的参数。"错误框是 MFC `CInvalidArgException`：TM 会把**所有插件**的 tooltip 文本拼成一条喂给 `CToolTipCtrl::UpdateTipText`，MFC 对超过 1024 字符的文本抛此异常——多个信息型插件（本插件 + MijiaPower 等）同载时总和越界才弹（栈回溯实测：mfc140u 内 throw，TrafficMonitor.exe 调用链）。
- v1.6.0 起本插件对自家 tooltip 上**硬预算 700 字符**（`worker.cpp` 的 `kTipBudget`）：完整展开（默认）最多舍弃最末的"模型用量"区，其余显示与未加预算时一致；超预算时从尾部按区整行舍弃（模型用量 → 积分汇总 → 定时任务 → 账户明细），骨架行（状态/地址/健康概要/提示/操作/尾注）必留并补一行"…"。`GetTooltipInfo` 出口另有 1000 字符硬截兜底。悬浮提示的额外控制（设置④"显示"页）：
  - **"悬浮提示显示定时任务明细"** 开关（`tooltip_tasks`，v1.7.0 起默认关闭）：需要盯任务下次触发时刻的再打开；关闭后健康概要等汇总行不受影响；
  - **"悬浮提示显示账户明细"** 开关（`tooltip_accounts`）：账户区是 tooltip 最大的长度来源，多插件同载挤占 1024 总额时优先关它；
  - **单账户显隐**：设置②"账户与积分"页**右键账户行**勾/去"悬浮提示显示此账户"（按 uid8 落盘到 `tip_hidden_uids`），隐藏的号不占 tooltip 行但仍计入健康/总数；v1.7.0 起账户表新增"悬浮窗"列（是/否）直观反映该开关的当前状态；
  - 关闭"完整展开"切到最省的折叠配额：每行 64 字符 / 4 行 / 总长 150 字符兜底。
  其他插件的 tooltip 长度不受本插件控制——同载超限弹框时优先检查同类信息型插件。

## 故障排查

- **栏位不显示**：先查主窗口与任务栏窗口各自的「显示项目」是否勾选 `WB2API 服务状态`——插件加载正常 ≠ 已勾选显示（落盘在 TM 目录 config.ini 的 `plugin_display_item` 列表）。
- **"定时任务/实时积分"页提示不可用**：服务端未开 `"admin": { "enabled": true }`，或用的是官方上游（无 `/admin` 接口）。
- **更新插件后行为没变**：「插件管理→重新加载」不生效，必须**重启 TrafficMonitor**（原因见上）。
- **TM 弹"遇到不适当的参数。"**：多个信息型插件 tooltip 总长超 1024 字符，先关本插件的"账户明细"/其他插件的 tooltip 再试；详见[已知边界](#已知边界)。
- **想看接口调用时序 / 排查宿主弹框**：设环境变量 `WB2API_TRACE=1` 后启动 TM，插件会记录接口调用时序，并把 first-chance 异常（C++ 异常含调用栈）写入追踪日志：默认 `<WB2API_TRACE_DIR>\wb2api_trace.log`（未设该变量则为 TM 当前目录），也可用 `WB2API_TRACE_PATH` 直接指定完整文件路径。普通 Release 产物即支持（运行期开关，无需专用构建），未开启时零开销。

## 许可

本项目基于 [MIT License](LICENSE) 开源。
