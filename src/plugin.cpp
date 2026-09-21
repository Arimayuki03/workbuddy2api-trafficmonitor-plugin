// plugin.cpp — 主类实现 + 命令菜单 + DllMain + 导出。
#include "plugin.h"
#include "autostart.h"
#include "common.h"
#include "dialogs.h"
#include "item.h"
#include "logger.h"
#include "settings.h"
#include "trace.h"
#include "worker.h"
#include <cwchar>
#include <thread>
#include <algorithm>
#include <string_view>

namespace wb2 {

HMODULE g_hInst = nullptr; // dialogs.cpp extern 引用；DllMain 里赋值（对话框资源在本 DLL 内）

CPluginApp& CPluginApp::Instance()
{
    static CPluginApp inst;
    return inst;
}

IPluginItem* CPluginApp::GetItem(int index)
{
    WB2API_TRACE_LOG("GetItem");
    // 只暴露 1 个显示项；越界必须返回 nullptr（TM 用 nullptr 探测结束）
    return index == 0 ? &StatusItem::Instance() : nullptr;
}

void CPluginApp::DataRequired()
{
    WB2API_TRACE_LOG("DataRequired");
    // 数据由 worker 线程自取；这里只兜底启动（有的 TM 版本先调 DataRequired 再 OnInitialize）
    Worker::Instance().Start();
}

const wchar_t* CPluginApp::GetInfo(PluginInfoIndex index)
{
    WB2API_TRACE_LOG("GetInfo");
    switch (index) {
    case TMI_NAME: return L"WorkBuddy2API";
    case TMI_DESCRIPTION: return L"workbuddy2api 服务状态监控与手动控制（loopback 本地接口）";
    case TMI_AUTHOR: return L"Arima";
    case TMI_COPYRIGHT: return L"MIT License";
    case TMI_VERSION: return L"1.8.0";
    case TMI_URL: return L"https://github.com/Arimayuki03/workbuddy2api-trafficmonitor-plugin";
    default: return L"";
    }
}

const wchar_t* CPluginApp::GetTooltipInfo()
{
    std::wstring t = Worker::Instance().TooltipText();
    WB2API_TRACE_LOG("GetTooltipInfo");
    if (t.empty()) t = L"WorkBuddy2API：等待首次轮询…";
#ifdef WB2API_TRACE_BUILD
    {
        std::wstring note = WideFormat(L"GetTooltipInfo len=%zu lines=%zu",
            t.size(), static_cast<size_t>(std::count(t.begin(), t.end(), L'\n')) + 1);
        trace::Write(std::string(note.begin(), note.end()).c_str());
    }
#endif
#ifdef WB2API_TRACE_AB_EMPTY
    // A/B：把我们的 tooltip 压成一行短句，验证"三插件总长超限"假设
    static thread_local std::wstring short_tip;
    short_tip = L"WorkBuddy2API";
    return short_tip.c_str();
#endif
    // 出口硬上限：MFC UpdateTipText 对 >1024 字符抛 CInvalidArgException（"遇到不适当的
    // 参数。"），TM 把所有插件的 tooltip 拼成一条，这里无法得知拼接总额，只能保证自己
    // 永不成为压垮的那段。正常文本经 BuildDisplayLocked 的 kTipBudget(340) 预算已收敛；
    // 这道闸兜住竞态窗口（设置在两次重建间从折叠切到完整、用户手改 json 等）。1000 留
    // 出宿主自身文本与其它插件的最低生存空间；硬截在出口完成，无状态、无第二次遍历。
    constexpr size_t kTipHardCap = 1000;
    if (t.size() > kTipHardCap) {
        t.resize(kTipHardCap - 1); // 原地截断（自引用 assign 标准不保证安全）
        t += L"…";
    }
    // 乒乓双缓冲：宿主对返回指针无生命周期契约（见 GetCommandName 注释），单缓冲在
    // 下次调用变长时重分配会让宿主正读着的旧指针悬空。交替写 buf_[0]/[1]，任一次
    // 返回的指针到"再下一次调用"前不被改写，宿主有整帧时间完成拷贝。
    std::lock_guard<std::mutex> lk(tt_mu_);
    tt_cur_ ^= 1;
    tt_buf_[tt_cur_] = std::move(t);
    return tt_buf_[tt_cur_].c_str();
}

COLORREF CPluginApp::ValueTextColor(bool dark_mode) const
{
    if (colors_set_) return value_color_;
    return dark_mode ? RGB(240, 240, 240) : RGB(20, 20, 20);
}

void CPluginApp::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    WB2API_TRACE_LOG("OnExtenedInfo");
    auto parse_color = [](const wchar_t* s, COLORREF& out) -> bool {
        if (!s || !*s) return false;
        wchar_t* end{};
        long v = wcstol(s, &end, 10);
        if (end == s) {
            v = wcstol(s, &end, 16); // 十六进制容错
            if (end == s) return false;
        }
        out = static_cast<COLORREF>(v) & 0xFFFFFF;
        return true;
    };
    switch (index) {
    case EI_LABEL_TEXT_COLOR:
        parse_color(data, label_color_); // 自绘不使用标签色，仅留存
        break;
    case EI_VALUE_TEXT_COLOR:
        // 只有数值色真正解析成功才启用自定义色：label_color_ 没有消费者，
        // 不能让"标签色先到"决定 ValueTextColor 走不走 value_color_。
        if (parse_color(data, value_color_)) colors_set_ = true;
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
    WB2API_TRACE_LOG("OnInitialize");
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
        // start_with_tm 的拉起判定挪进 worker 首轮轮询之后（Worker::MaybeStartWithTm）：
        // 这里曾派生 sleep(2s) 的裸 detach 线程，宿主快速卸载时它没有取消/等待机制，
        // 是动作线程卸载竞态（审查 High 项）的来源之一；worker 线程本身受 Stop() 管控。
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
// 第 7 类 queue=任务队列（wb2api 合并版 /admin/tasks 透出，queue_enabled 缺省 false）。
const char* kKinds[7] = { "checkin", "travel", "activity", "keepalive", "school", "cat", "queue" };
const wchar_t* kKindZh[7] = { L"签到", L"旅行", L"活跃", L"保活", L"开学季", L"夜猫子", L"任务队列" };
const wchar_t* kSettingZh[3] = { L"开机自启(计划任务)", L"随TrafficMonitor启动", L"意外自动拉起" };

int SettingIndex(int idx) { return idx - 17; }  // 17..19 → 0..2

} // namespace

// 布局：0启动 1停止 2重启 3设置 4积分 | 5..11 立即执行(7类) | 12..18 启用勾选(7类) |
// 19..21 开关勾选。任务页加 queue 行后菜单同步补齐两类命令。
int CPluginApp::GetCommandCount() { WB2API_TRACE_LOG("GetCommandCount"); return 22; }

const wchar_t* CPluginApp::GetCommandName(int command_index)
{
    WB2API_TRACE_LOG("GetCommandName");
    // 返回长期稳定指针：接口对生命周期无契约，宿主可能"先枚举全部、后渲染"，
    // 复用 thread_local 缓冲会让前一条命令名失效。
    static const std::vector<std::wstring> kNames = [] {
        std::vector<std::wstring> v;
        v.reserve(22);
        for (const wchar_t* n : { L"启动服务", L"停止服务", L"重启服务", L"打开设置…", L"查询实时积分" })
            v.push_back(n);
        for (int i = 0; i < 7; i++) v.push_back(std::wstring(L"立即执行: ") + kKindZh[i]);
        for (int i = 0; i < 7; i++) v.push_back(std::wstring(L"启用: ") + kKindZh[i]);
        for (int i = 0; i < 3; i++) v.push_back(kSettingZh[i]);
        return v;
    }();
    if (command_index < 0 || command_index >= static_cast<int>(kNames.size())) return L"";
    return kNames[command_index].c_str();
}

int CPluginApp::IsCommandChecked(int command_index)
{
    WB2API_TRACE_LOG("IsCommandChecked");
    if (command_index >= 12 && command_index < 19) {
        Snapshot sn = Worker::Instance().Copy();
        if (!sn.admin_available) return 0;
        std::string want = kKinds[command_index - 12];
        for (auto& t : sn.tasks)
            if (t.kind == want) return t.enabled ? 1 : 0;
        // queue 服务端缺省 false，其余缺省启用：快照未回时按各自缺省显示
        return std::string_view(kKinds[command_index - 12]) == "queue" ? 0 : 1;
    }
    if (command_index >= 19 && command_index < 22) {
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
    WB2API_TRACE_LOG("OnPluginCommand");
    HWND parent = static_cast<HWND>(hWnd);
    auto& wk = Worker::Instance();
    if (command_index == 0) wk.RequestStartService();
    else if (command_index == 1) wk.RequestStopService();
    else if (command_index == 2) wk.RequestRestartService();
    else if (command_index == 3) OpenSettings(parent);
    else if (command_index == 4) wk.RequestRefreshCredits();
    else if (command_index >= 5 && command_index < 12) wk.RequestRunTask(kKinds[command_index - 5]);
    else if (command_index >= 12 && command_index < 19) {
        // 勾选切换：取反当前状态（服务端为准，动作失败会在快照刷新里回正）
        std::string want = kKinds[command_index - 12];
        Snapshot sn = wk.Copy();
        bool cur = std::string_view(want) == "queue" ? false : true; // queue 缺省关，其余缺省开
        for (auto& t : sn.tasks)
            if (t.kind == want) cur = t.enabled;
        wk.RequestToggleTask(want, !cur);
    }
    else if (command_index >= 19 && command_index < 22) {
        int si = SettingIndex(command_index);
        if (si == 0) {
            // 安装/卸载要走 schtasks（~百毫秒），放后台线程别卡菜单关闭
            Settings cur = SettingsStore::Instance().Get();
            bool want = !cur.autostart_task; // 勾选切换：目标状态取反
            std::thread([want] {
                // 传入 worker 停止标志：宿主卸载时 schtasks 等待提前终止，
                // 线程尽快退场，不在已解映射的模块上逗留（审查 High 项）。
                const std::atomic<bool>* cancel = Worker::Instance().CancelFlag();
                std::wstring err;
                if (want) {
                    bool ok = autostart::Install(err, cancel);
                    if (!ok && !Worker::Instance().StopRequested()) LogW(L"autostart install: " + err);
                    // 只回写 autostart_task 一个字段：整结构体回写会覆盖其他线程的并发修改
                    // （取消路径不回写勾选——卸载中设置已无意义）
                    if (!Worker::Instance().StopRequested())
                        SettingsStore::Instance().Modify([ok](Settings& st) { st.autostart_task = ok; });
                } else {
                    autostart::Uninstall(err, cancel); // 删除失败也取消勾选（下次重开任务页可见真实状态）
                    if (!Worker::Instance().StopRequested())
                        SettingsStore::Instance().Modify([](Settings& st) { st.autostart_task = false; });
                }
            }).detach();
            return;
        }
        if (si == 1) {
            Settings after = SettingsStore::Instance().Modify(
                [](Settings& st) { st.start_with_tm = !st.start_with_tm; });
            if (after.start_with_tm) wk.RequestStartService();
        } else if (si == 2) {
            SettingsStore::Instance().Modify(
                [](Settings& st) { st.auto_relaunch = !st.auto_relaunch; });
        }
    }
}

ITMPlugin::OptionReturn CPluginApp::ShowOptionsDialog(void* hParent)
{
    WB2API_TRACE_LOG("ShowOptionsDialog");
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
    WB2API_TRACE_LOG("export.TMPluginGetInstance");
    return &wb2::CPluginApp::Instance();
}

// 排查辅助：记录所有 first-chance 异常的地址与调用栈（弹"遇到不适当的参数"时能看到抛点）。
static LONG WINAPI VectoredExcept(PEXCEPTION_POINTERS ep)
{
    HMODULE hm = nullptr;
    char mod[64] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(ep->ExceptionRecord->ExceptionAddress), &hm) && hm) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(hm, name, MAX_PATH);
        const char* slash = strrchr(name, '\\');
        _snprintf_s(mod, sizeof mod, _TRUNCATE, "%s", slash ? slash + 1 : name);
    }
    wb2::trace::ExceptRecord("vectored", ep->ExceptionRecord->ExceptionCode,
        ep->ExceptionRecord->ExceptionAddress);
    char line[160];
    _snprintf_s(line, sizeof line, _TRUNCATE, "except code=%08lX addr=%p mod=%s",
        ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, mod);
    // VEH 跑在任何线程的任何 first-chance 异常上：异常若落在另一线程持 trace 锁的
    // 临界区内，阻塞取锁会自死锁——TryWrite 拿不到就放弃本条（诊断日志允许丢）。
    wb2::trace::TryWrite(line);
    // C++ 异常（0xE06D7363）：抓调用栈看 throw 点在哪
    if (ep->ExceptionRecord->ExceptionCode == 0xE06D7363) {
        void* frames[24]{};
        USHORT got = CaptureStackBackTrace(0, 24, frames, nullptr);
        wb2::trace::StackRecord(frames, got);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        wb2::g_hInst = hModule;
        DisableThreadLibraryCalls(hModule);
        wb2::trace::Init();
        if (wb2::trace::Active()) // 仅诊断构建/显式开启时注册，避免给宿主加全局异常钩子
            AddVectoredExceptionHandler(1, VectoredExcept);
    } else if (reason == DLL_PROCESS_DETACH) {
        wb2::Worker::Instance().Stop(); // TM 卸载/退出前停线程（Worker::Stop 内部做有界等待，超时 detach 兜底，不卡 DllMain）
    }
    return TRUE;
}

} // extern "C"
