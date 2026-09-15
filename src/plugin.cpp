// plugin.cpp — 主类实现 + 命令菜单 + DllMain + 导出。
#include "plugin.h"
#include "autostart.h"
#include "common.h"
#include "dialogs.h"
#include "item.h"
#include "logger.h"
#include "settings.h"
#include "worker.h"
#include <cwchar>
#include <thread>
#include <chrono>

namespace wb2 {

HMODULE g_hInst = nullptr; // dialogs.cpp extern 引用；DllMain 里赋值（对话框资源在本 DLL 内）

CPluginApp& CPluginApp::Instance()
{
    static CPluginApp inst;
    return inst;
}

IPluginItem* CPluginApp::GetItem(int index)
{
    // 只暴露 1 个显示项；越界必须返回 nullptr（TM 用 nullptr 探测结束）
    return index == 0 ? &StatusItem::Instance() : nullptr;
}

void CPluginApp::DataRequired()
{
    // 数据由 worker 线程自取；这里只兜底启动（有的 TM 版本先调 DataRequired 再 OnInitialize）
    Worker::Instance().Start();
}

const wchar_t* CPluginApp::GetInfo(PluginInfoIndex index)
{
    switch (index) {
    case TMI_NAME: return L"WorkBuddy2API";
    case TMI_DESCRIPTION: return L"workbuddy2api 服务状态监控与手动控制（loopback 本地接口）";
    case TMI_AUTHOR: return L"Arima";
    case TMI_COPYRIGHT: return L"MIT License";
    case TMI_VERSION: return L"1.0.0";
    case TMI_URL: return L"https://github.com/Arimayuki03/workbuddy2api-trafficmonitor-plugin";
    default: return L"";
    }
}

const wchar_t* CPluginApp::GetTooltipInfo()
{
    std::wstring t = Worker::Instance().TooltipText();
    if (t.empty()) t = L"WorkBuddy2API：等待首次轮询…";
    std::lock_guard<std::mutex> lk(tt_mu_);
    tt_cache_ = std::move(t);
    return tt_cache_.c_str();
}

COLORREF CPluginApp::ValueTextColor(bool dark_mode) const
{
    if (colors_set_) return value_color_;
    return dark_mode ? RGB(240, 240, 240) : RGB(20, 20, 20);
}

void CPluginApp::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    auto parse_color = [](const wchar_t* s, COLORREF& out) {
        if (!s || !*s) return;
        wchar_t* end{};
        long v = wcstol(s, &end, 10);
        if (end == s) {
            v = wcstol(s, &end, 16); // 十六进制容错
            if (end == s) return;
        }
        out = static_cast<COLORREF>(v) & 0xFFFFFF;
    };
    switch (index) {
    case EI_LABEL_TEXT_COLOR:
        parse_color(data, label_color_);
        colors_set_ = true;
        break;
    case EI_VALUE_TEXT_COLOR:
        parse_color(data, value_color_);
        break;
    case EI_CONFIG_DIR:
        if (data) SettingsStore::Instance().Init(data);
        break;
    default:
        break;
    }
}

void CPluginApp::OnInitialize(ITrafficMonitor* pApp)
{
    app_ = pApp;
    EnsureInited();
}

void CPluginApp::EnsureInited()
{
    std::call_once(init_once_, [this] {
        // EI_CONFIG_DIR 可能比 OnInitialize 晚/早到：哪边先到谁补齐。
        Settings s = SettingsStore::Instance().Get();
        if (s.config_dir.empty() && app_) {
            const wchar_t* dir = app_->GetPluginConfigDir();
            if (dir && *dir) SettingsStore::Instance().Init(dir);
        }
        s = SettingsStore::Instance().Get();
        if (!s.config_dir.empty())
            LogInit(s.config_dir + L"\\WorkBuddy2ApiPlugin.log", s.logging);
        Worker::Instance().Start();
        LogI(L"插件初始化完成");
        if (s.start_with_tm) {
            std::thread([] {
                std::this_thread::sleep_for(std::chrono::seconds(2)); // 让 TM/桌面先起
                if (Worker::Instance().State() == SvcState::Stopped) {
                    LogI(L"start_with_tm: 拉起服务");
                    Worker::Instance().RequestStartService();
                }
            }).detach();
        }
    });
}

// ============================================================================
// 命令菜单（TM「插件管理/插件命令」里出现的快捷动作与勾选）
// ============================================================================
namespace {

struct CmdDef {
    std::wstring name;      // 动态名走 NameOf
    const char* kind;       // 任务类命令的参数
    int type;               // 0 启停 1 设置开关 2 立即执行 3 启用开关 4 静态动作
};

enum CmdType { CT_SVC = 0, CT_SETTING = 1, CT_RUNTASK = 2, CT_TOGGLETASK = 3, CT_STATIC = 4 };
const char* kKinds[6] = { "checkin", "travel", "activity", "keepalive", "school", "cat" };
const wchar_t* kKindZh[6] = { L"签到", L"旅行", L"活跃", L"保活", L"开学季", L"夜猫子" };
const wchar_t* kSettingZh[3] = { L"开机自启(计划任务)", L"随TrafficMonitor启动", L"意外自动拉起" };

int SettingIndex(int idx) { return idx - 17; }  // 17..19 → 0..2

} // namespace

// 布局：0启动 1停止 2重启 3设置 4积分 | 5..10 立即执行 | 11..16 启用勾选 | 17..19 开关勾选
int CPluginApp::GetCommandCount() { return 20; }

const wchar_t* CPluginApp::GetCommandName(int command_index)
{
    static thread_local std::wstring buf;
    const wchar_t* names[5] = { L"启动服务", L"停止服务", L"重启服务", L"打开设置…", L"查询实时积分" };
    if (command_index >= 0 && command_index < 5) return names[command_index];
    if (command_index >= 5 && command_index < 11) {
        buf = std::wstring(L"立即执行: ") + kKindZh[command_index - 5];
        return buf.c_str();
    }
    if (command_index >= 11 && command_index < 17) {
        buf = std::wstring(L"启用: ") + kKindZh[command_index - 11];
        return buf.c_str();
    }
    if (command_index >= 17 && command_index < 20) return kSettingZh[SettingIndex(command_index)];
    return L"";
}

int CPluginApp::IsCommandChecked(int command_index)
{
    if (command_index >= 11 && command_index < 17) {
        Snapshot sn = Worker::Instance().Copy();
        if (!sn.admin_available) return 0;
        std::string want = kKinds[command_index - 11];
        for (auto& t : sn.tasks)
            if (t.kind == want) return t.enabled ? 1 : 0;
        return 1; // 快照还没回来：按默认启用
    }
    if (command_index >= 17 && command_index < 20) {
        Settings s = SettingsStore::Instance().Get();
        switch (SettingIndex(command_index)) {
        case 0: return s.autostart_task ? 1 : 0;
        case 1: return s.start_with_tm ? 1 : 0;
        case 2: return s.auto_relaunch ? 1 : 0;
        }
    }
    return 0;
}

void CPluginApp::OnPluginCommand(int command_index, void* hWnd, void*)
{
    HWND parent = static_cast<HWND>(hWnd);
    auto& wk = Worker::Instance();
    if (command_index == 0) wk.RequestStartService();
    else if (command_index == 1) wk.RequestStopService();
    else if (command_index == 2) wk.RequestRestartService();
    else if (command_index == 3) OpenSettings(parent);
    else if (command_index == 4) wk.RequestRefreshCredits();
    else if (command_index >= 5 && command_index < 11) wk.RequestRunTask(kKinds[command_index - 5]);
    else if (command_index >= 11 && command_index < 17) {
        // 勾选切换：取反当前状态（服务端为准，动作失败会在快照刷新里回正）
        std::string want = kKinds[command_index - 11];
        Snapshot sn = wk.Copy();
        bool cur = true;
        for (auto& t : sn.tasks)
            if (t.kind == want) cur = t.enabled;
        wk.RequestToggleTask(want, !cur);
    }
    else if (command_index >= 17 && command_index < 20) {
        int si = SettingIndex(command_index);
        Settings s = SettingsStore::Instance().Get();
        if (si == 0) {
            s.autostart_task = !s.autostart_task;
            // 安装/卸载要走 schtasks（~百毫秒），放后台线程别卡菜单关闭
            bool want = s.autostart_task;
            std::thread([want] {
                Settings cur = SettingsStore::Instance().Get();
                if (want) {
                    std::wstring err;
                    bool ok = autostart::Install(err);
                    cur.autostart_task = ok;
                    if (!ok) LogW(L"autostart install: " + err);
                } else {
                    std::wstring err;
                    autostart::Uninstall(err); // 删除失败也取消勾选（下次重开任务页可见真实状态）
                    cur.autostart_task = false;
                }
                SettingsStore::Instance().Update(cur);
            }).detach();
            return;
        }
        if (si == 1) s.start_with_tm = !s.start_with_tm;
        if (si == 2) s.auto_relaunch = !s.auto_relaunch;
        SettingsStore::Instance().Update(s);
        if (si == 1 && s.start_with_tm) wk.RequestStartService();
    }
}

ITMPlugin::OptionReturn CPluginApp::ShowOptionsDialog(void* hParent)
{
    return dlg::ShowSettingsDialog(static_cast<HWND>(hParent));
}

void CPluginApp::OpenSettings(HWND parent)
{
    if (!parent) parent = app_ ? static_cast<HWND>(app_->GetMainWindowHwnd()) : nullptr;
    dlg::ShowSettingsDialog(parent);
}

} // namespace wb2

// ============================================================================
// 导出与 DLL 生命周期
// ============================================================================
extern "C" {

__declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &wb2::CPluginApp::Instance();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        wb2::g_hInst = hModule;
        DisableThreadLibraryCalls(hModule);
    } else if (reason == DLL_PROCESS_DETACH) {
        wb2::Worker::Instance().Stop(); // TM 卸载/退出前停线程（join 有超时上限，不会卡死）
    }
    return TRUE;
}

} // extern "C"
