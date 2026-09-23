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

// 账户表排序键的合法列上界（dialogs.cpp 的 kAccCols-1，6 列布局）。与
// acc_sort_col 的解析防呆共用单一来源：列数演进（增删列）时这里与 dialogs.cpp
// 的列定义同步改，持久化的合法排序不会被解析端误回落成未排序。
inline constexpr int kAccSortColMax = 5;

struct Settings {
    std::wstring service_dir;                 // wb2api 安装目录（空=未配置；服务页「浏览…」选择）
    int port = 7863;
    int poll_interval_sec = 30;               // /healthz+/status 轮询（零上游成本，>=10）
    int admin_poll_sec = 60;                  // /admin/tasks、/admin/credits 轮询（服务端本地缓存）
    int credits_refresh_interval_min = 0;     // 实时积分自动刷新周期（分钟）；0=仅手动，>=1
    int show_mode = SM_STATE_ACCOUNT;
    bool show_live_credits = true;            // 显示积分时优先用实时缓存
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

    // api_key：读 service_dir\config.json 的 api_key（mtime 缓存，静默失败返回空）。
    // 可能从 worker/action/UI 线程调用，内部自锁。返回 UTF-8。
    std::string CurrentApiKey();

private:
    SettingsStore() = default;
    mutable std::mutex mu_;
    Settings cur_;
    std::wstring path_;
    // config.json api_key 缓存
    std::wstring key_file_;
    FILETIME key_mtime_{};
    std::string key_cached_;
};

} // namespace wb2
