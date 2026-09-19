// settings.h — 插件设置（config 目录 WorkBuddy2ApiPlugin.json）读写。
// 全进程唯一入口 SettingsStore::Instance()；锁内只碰内存，落盘在锁外原子写。
#pragma once
#include <string>
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

struct Settings {
    std::wstring service_dir = L"D:\\Code\\workbuddy2api"; // wb2api 安装目录（工作目录+exe+config.json）
    int port = 7863;
    int poll_interval_sec = 30;               // /healthz+/status 轮询（零上游成本，>=10）
    int admin_poll_sec = 60;                  // /admin/tasks、/admin/credits 轮询（服务端本地缓存）
    int credits_refresh_interval_min = 0;     // 实时积分自动刷新周期（分钟）；0=仅手动，>=1
    int show_mode = SM_STATE_ACCOUNT;
    bool show_live_credits = true;            // 显示积分时优先用实时缓存
    bool tooltip_full = true;                 // tooltip 完整展开；关闭=折叠为 4 行/~150 字符（多插件同载防宿主 1024 超限弹框）
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
