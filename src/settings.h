// settings.h — 插件设置（config 目录 WorkBuddy2ApiPlugin.json）读写。
// 全进程唯一入口 SettingsStore::Instance()；锁内只碰内存，落盘在锁外原子写。
#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <windows.h>

namespace wb2 {

// ShowMode：任务栏自绘内容（对应设置页④的单选）。
enum ShowMode {
    SM_STATE_ACCOUNT = 0,  // 点 + "4/4"（健康/总数）
    SM_STATE_CREDITS = 1,  // 点 + 积分（估算，有实时缓存优先实时）
    SM_STATE_ONLY = 2,     // 仅点 + 状态短词
};

// 部署方式（v1.14.0）：
//   DM_LOCAL  = 本机服务：保留原有启停/自启/自动拉起与 service_dir 全套能力；
//   DM_DOCKER = Docker/远程：只对接「服务地址 + 手动 api_key」，绝不启停本地进程，
//               也不读本机 config.json（容器内配置不在这台机器上）。
enum DeployMode {
    DM_LOCAL = 0,
    DM_DOCKER = 1,
};

// 账户表排序键的合法列上界（dialogs.cpp 的 kAccCols-1，6 列布局）。与
// acc_sort_col 的解析防呆共用单一来源：列数演进（增删列）时这里与 dialogs.cpp
// 的列定义同步改，持久化的合法排序不会被解析端误回落成未排序。
inline constexpr int kAccSortColMax = 5;

struct Settings {
    // —— 部署方式（v1.14.0）——
    // Docker 模式存在的理由：容器部署时 wb2api.exe 不在本机，插件不能也不该拉起
    // 本地进程；地址可能在本机映射端口（127.0.0.1:7863）或局域网另一台机器。
    // deploy_mode 决定地址来源与能力集：本地模式沿用 service_dir/port，Docker 模式
    // 用 server_url（完整 base URL）+ api_key（手动填写）。
    //   /admin 端点服务端只接受 loopback：跨机直连会 403。跨机想用任务/实时积分，
    //   请在宿主机把容器 127.0.0.1:7863 经 SSH 隧道映射到本机端口，再把 server_url
    //   填成本机隧道地址（如 http://127.0.0.1:17863）——对服务端而言来源仍是 loopback。
    DeployMode deploy_mode = DM_LOCAL;
    std::wstring server_url;                  // Docker/远程模式：如 https://192.168.1.10:7863（可 http，跨机建议 https）
    std::string api_key;                      // Docker/远程模式：服务端 config.json 的 api_key（UTF-8）
    std::wstring auths_dir;                   // Docker/远程模式可选：宿主机映射的 auths 目录（令牌到期）
    std::wstring service_dir;                 // 本地模式：wb2api 安装目录（空=未配置；服务页「浏览…」选择）
    int port = 7863;
    int poll_interval_sec = 30;               // /healthz+/status 轮询（零上游成本，>=10）
    int admin_poll_sec = 60;                  // /admin/tasks、/admin/credits 轮询（服务端本地缓存）
    int credits_refresh_interval_min = 0;     // 实时积分自动刷新周期（分钟）；0=仅手动，>=1
    int show_mode = SM_STATE_ACCOUNT;
    bool show_live_credits = true;            // 显示积分时优先用实时缓存
    bool show_concurrency = false;            // 积分右侧追加当前并发数（/status 各账号 in_flight 求和，纯数字）
    bool tooltip_full = true;                 // tooltip 完整展开（超 700 字符预算自动舍弃次要行）；关闭=折叠为 4 行/~150 字符
    bool tooltip_accounts = true;             // tooltip 显示"账户"区明细行（行内只显实时积分）；关闭可显著缩短 tooltip（多插件同载挤预算时用）
    // 单账户 tooltip 显隐（uid8 列表，落盘）：右键账户行切换。列表内的号不出现在
    // tooltip 账户明细里，但仍计入健康/总数等汇总行；清空列表即全部显示。
    std::vector<std::wstring> tip_hidden_uids;
    // 账户表列头排序持久化（v1.10.1）：列头点击即时落盘（与 tip_hidden_uids 同路径，
    // 不等设置窗「保存」）。acc_sort_col 与设置窗账户表列序一一对应（昵称0|域1|实时2|
    // 状态3|悬浮窗4|令牌剩5），-1=未排序；acc_sort_dir 0=升序 1=降序。解析端防呆回落。
    int acc_sort_col = -1;
    int acc_sort_dir = 0;
    // tooltip 显示"定时任务"区明细行。默认关闭（v1.7.0）：6 任务 × 20+ 字符是次要区里
    // 最长且变化最频繁的，多数时间没有观测价值，预算收紧时也该最先让位；需要盯任务
    // 下次触发时刻的再打开。默认值即"定时任务默认取消悬浮窗显示"的诉求。
    bool tooltip_tasks = false;
    bool autostart_task = false;              // Windows 计划任务随登录启动服务
    bool start_with_tm = false;               // TrafficMonitor 启动时拉起服务
    bool auto_relaunch = false;               // 意外停止自动拉起
    bool logging = false;                     // 文件日志
    bool user_stopped = false;                // 用户主动停止标志（持久化：跨 TM 重启抑制拉起）
    std::wstring config_dir;                  // 运行时由 TM 注入，不落盘
};

class SettingsStore {
public:
    static SettingsStore& Instance();

    // config_dir：TM 提供的插件配置目录；加载已有 json 或写默认。
    void Init(const std::wstring& config_dir);
    Settings Get() const;
    // 保存：锁内替换 + 锁外原子落盘（tmp+rename）。
    void Update(const Settings& s);
    // 原子读-改-写：fn 在锁内直接改当前值，锁外原子落盘，返回改后副本。
    // 用于"只动一两个字段"的并发场景（动作线程/菜单线程 vs 设置窗保存），
    // 替代 Get→改→Update 的竞态窗口。fn 内不得调用 SettingsStore 的任何方法（死锁）。
    Settings Modify(const std::function<void(Settings&)>& fn);

    // api_key：Docker 模式直接返回手动配置值；本地模式读 service_dir\config.json
    // 的 api_key（mtime 缓存，静默失败返回空）。可能从 worker/action/UI 线程调用，
    // 内部自锁。返回 UTF-8。
    std::string CurrentApiKey();
    // 用调用方已取得的 Settings 快照取 key：保证 base URL 与鉴权来自同一份
    // 配置。否则轮询途中用户切换部署模式，会出现"容器的 key 发往本机"或
    // "本机 key 发往远程"的错配。
    std::string CurrentApiKey(const Settings& s);

private:
    std::string ReadApiKeyLocked(const std::wstring& file); // 调用方须持 mu_

    SettingsStore() = default;
    mutable std::mutex mu_;
    Settings cur_;
    std::wstring path_;
    // config.json api_key 缓存
    std::wstring key_file_;
    FILETIME key_mtime_{};
    std::string key_cached_;
};

// 服务 base URL（不带结尾斜杠）：本地模式恒为 http://127.0.0.1:<port>，
// Docker 模式为设置里的 server_url。Docker 模式未填地址时返回空串——**绝不
// 回落到本机**，调用点据此提示"未配置服务地址"并短路请求。
std::wstring ServiceBaseUrl(const Settings& s);
// 本地 auths 目录：本地模式=service_dir\auths；Docker 模式=设置里的映射目录
// （留空=不扫描，token 到期列显示 "-"；相对路径按插件配置目录解析）。
std::wstring AuthsDirectory(const Settings& s);

// 目录/URL/密钥归一（ParseSettings 与设置对话框保存共用同一份实现）：
//   NormalizeLocalDir  ：去首尾空白/配对的引号/尾分隔符；
//   NormalizeServiceUrl：补 http://、去前导斜杠与尾斜杠、校验 scheme+host；
//                        非法或空返回空串；
//   SanitizeApiKey     ：去首尾空白并剔除 CR/LF 等控制字符（防 Authorization 头注入）。
std::wstring NormalizeLocalDir(std::wstring v);
std::wstring NormalizeServiceUrl(std::wstring v);
std::string SanitizeApiKey(const std::string& key);

} // namespace wb2
