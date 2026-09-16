// dialogs.cpp — 设置对话框。约定：
//  * 打开时拷贝一份 Settings 作为"编辑中"副本，点"保存"才落盘并 diff 应用副作用（计划任务等）；
//  * 实时信息（状态/账户/任务/冷却）每秒从 Worker 快照刷新，只改展示控件，不碰输入框；
//  * 任务勾选 = 即时动作（PATCH），不等"保存"——它管理的是服务本体，不是插件配置；
//  * 按钮 busy 态由 Worker::IsActionBusy 驱动防连点；实时积分还有服务端 429 第二道闸；
//  * 只认 BN_CLICKED（按钮不带 BS_NOTIFY）：焦点类通知曾让"打开目录/日志"在 explorer
//    抢/还焦点时连环触发，表现为重复开资源管理器、关掉又自动重开；
//  * 背景统一：初始化时采样 tab 页体的实际绘制颜色做刷子（aero 浅色主题页体是
//    F9F9F9 浅灰——既不是 COLOR_WINDOW 白也不是 COLOR_BTNFACE 灰，用错哪个，
//    标签后面都是一条异色带，视觉上像"阴影"），对话框与静态控件同色消除底条。
#include "dialogs.h"
#include "autostart.h"
#include "common.h"
#include "logger.h"
#include "plugin.h"
#include "procctl.h"
#include "resource.h"
#include "settings.h"
#include "svcinfo.h"
#include "worker.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <ctime>
#include <algorithm>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")



namespace wb2 {
extern HMODULE g_hInst; // 资源所在模块（DllMain 赋值）
}

namespace wb2::dlg {
namespace {

constexpr int KIND_N = 6;
const char* kKinds[KIND_N] = { "checkin", "travel", "activity", "keepalive", "school", "cat" };
const wchar_t* kKindZh[KIND_N] = { L"签到", L"猫猫旅行", L"活跃上报", L"Token保活", L"开学季", L"夜猫子" };

int64_t NowSecX() { return static_cast<int64_t>(time(nullptr)); }

bool SettingsEqual(const Settings& a, const Settings& b)
{
    return a.service_dir == b.service_dir && a.port == b.port &&
        a.poll_interval_sec == b.poll_interval_sec &&
        a.admin_poll_sec == b.admin_poll_sec &&
        a.credits_refresh_interval_min == b.credits_refresh_interval_min &&
        a.show_mode == b.show_mode && a.show_live_credits == b.show_live_credits &&
        a.tooltip_full == b.tooltip_full &&
        a.autostart_task == b.autostart_task && a.start_with_tm == b.start_with_tm &&
        a.auto_relaunch == b.auto_relaunch && a.logging == b.logging;
}

struct TaskRow {
    // time=触发时间输入框（"9,21"），apply=应用按钮（写回服务 config.json）。
    // 与运行列按钮共用 IDC_TASK_BASE 槽位：i*10+0 勾选 / +5 立即执行 / +6 输入框 / +7 应用。
    HWND chk = nullptr, time = nullptr, next = nullptr, status = nullptr, btn = nullptr, apply = nullptr;
    std::wstring synced; // 上次已回填进 time 框的快照值（防止每秒刷新覆盖用户编辑中内容）
};

struct Ctx {
    Settings work, orig;
    bool changed_flag = false;
    HWND dlg = nullptr, tab = nullptr;
    std::vector<HWND> pages[5];
    int cur_page = 0;
    HFONT font = nullptr;
    HBRUSH bg_brush = nullptr; // 页体色刷：对话框背景与静态控件共用，消"灰底条阴影"
    // 页①
    HWND state_lbl = nullptr, sub_lbl = nullptr, err_lbl = nullptr, action_lbl = nullptr;
    HWND dir_edt = nullptr, port_edt = nullptr, poll_edt = nullptr;
    HWND btn_start = nullptr, btn_stop = nullptr, btn_restart = nullptr;
    HWND chk_auto = nullptr, chk_tm = nullptr, chk_relaunch = nullptr, chk_log = nullptr;
    COLORREF state_color = RGB(120, 120, 120);
    // 页②
    HWND acc_list = nullptr, btn_cred = nullptr, lbl_cool = nullptr, citv_edt = nullptr;
    int64_t acc_sig = -1;
    // 页③
    TaskRow task[KIND_N];
    HWND task_warn = nullptr, btn_runall = nullptr, task_note = nullptr;
    // 页④
    HWND rad[3] = {}, chk_live = nullptr, chk_tipfull = nullptr;
    // 页⑤
    HWND admin_edt = nullptr, admin_stat = nullptr;
    // 次要说明文字集合：CTLCOLORSTATIC 里统一画灰
    std::vector<HWND> hints;
};

// 子控件统一按"tab 显示区左上角 + DLU 坐标"创建；id/page 归属见 resource.h。
HWND MkWnd(Ctx& c, LPCWSTR cls, LPCWSTR text, DWORD style, DWORD exstyle,
    int l, int t, int w, int h, int id, int page)
{
    RECT rc{ l, t, l + w, t + h };
    MapDialogRect(c.dlg, &rc);
    RECT disp{};
    GetWindowRect(c.tab, &disp);
    ScreenToClient(c.dlg, reinterpret_cast<POINT*>(&disp.left));
    ScreenToClient(c.dlg, reinterpret_cast<POINT*>(&disp.right));
    TabCtrl_AdjustRect(c.tab, FALSE, &disp);
    HWND hw = CreateWindowExW(exstyle, cls, text, WS_CHILD | WS_GROUP | style,
        disp.left + rc.left, disp.top + rc.top, rc.right - rc.left, rc.bottom - rc.top,
        c.dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), nullptr, nullptr);
    SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(c.font), TRUE);
    if (page >= 0) c.pages[page].push_back(hw);
    return hw;
}

HWND MkLabel(Ctx& c, LPCWSTR t, int l, int tp, int w, int h, int page, DWORD extra = 0)
{
    return MkWnd(c, L"STATIC", t, SS_LEFT | SS_NOPREFIX | extra, 0, l, tp, w, h, 0, page);
}
// 次要说明文字：WM_CTLCOLORSTATIC 里统一画成灰色，与正文形成层级。
HWND MkHint(Ctx& c, LPCWSTR t, int l, int tp, int w, int h, int page)
{
    HWND hw = MkLabel(c, t, l, tp, w, h, page);
    c.hints.push_back(hw);
    return hw;
}
// 一律不带 BS_NOTIFY：它会启用 BN_SETFOCUS/BN_KILLFOCUS 通知，explorer 抢/还焦点时
// 这些通知也走 WM_COMMAND，曾被当成点击处理——表现为"打开目录/日志"一次点出多个
// 资源管理器、关掉窗口焦点回到按钮又自动重开。动作只认 BN_CLICKED（见 WM_COMMAND 门禁）。
HWND MkCheck(Ctx& c, LPCWSTR t, int l, int tp, int w, int id, int page)
{
    return MkWnd(c, L"BUTTON", t, BS_AUTOCHECKBOX | WS_TABSTOP, 0, l, tp, w, 10, id, page);
}
HWND MkBtn(Ctx& c, LPCWSTR t, int l, int tp, int w, int h, int id, int page)
{
    return MkWnd(c, L"BUTTON", t, BS_PUSHBUTTON | WS_TABSTOP, 0, l, tp, w, h, id, page);
}
HWND MkEdit(Ctx& c, LPCWSTR t, int l, int tp, int w, int id, int page, DWORD extra = 0)
{
    // 平边框（WS_BORDER，去掉 CLIENTEDGE 凹陷框）：与纯色页体更协调。
    return MkWnd(c, L"EDIT", t, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | extra,
        0, l, tp, w, 12, id, page);
}

#ifndef HDM_GETITEMRECT
#define HDM_GETITEMRECT (HDM_FIRST + 7)
#endif
// 账户表子类化：主题 ListView 的网格线(240,240,240 浅灰)在默认 WM_PAINT 里最后画，
// NM_CUSTOMDRAW 阶段的补画会被盖掉。只能在默认绘制完成后追加表头切割线——
// 表头下沿那条与数据行分隔的横线在浅色主题里太淡（贴着表头渐变底），要加深一档。
LRESULT CALLBACK AccListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR id, DWORD_PTR ref)
{
    // 列宽拖动（HDN_ITEMCHANGING/CHANGED，含双击分隔线自适应）：ListView 默认绘制只
    // 增量重绘受影响区域，浅色主题的网格线会在被拖列的旧位置留下竖线残影。默认处理后
    // 强制整客户区重绘——行列线永远按最新列宽整帧画，拖动实时反馈照常保留。
    // 配合 LVS_EX_DOUBLEBUFFER：重绘先进内存位图再整帧上屏，不会"擦背景→画内容"
    // 两段式交替出现的白闪。只在 ITEMCHANGED（宽度确实变了）后重绘，ITEMCHANGING
    // 每像素拖动会连发多次，多绘无益。
    if (msg == WM_NOTIFY) {
        LPNMHEADERW nm = reinterpret_cast<LPNMHEADERW>(lp);
        if (nm && nm->hdr.code == HDN_ITEMCHANGEDW) {
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            InvalidateRect(h, nullptr, FALSE);
            return r;
        }
    }
    LRESULT r = DefSubclassProc(h, msg, wp, lp);
    if (msg == WM_PAINT) {
        // 表头高度：表头客户区与列表客户区同原点(顶部满宽)，item0 的 bottom 即高度。
        // 不用 HDM_GETITEMHEIGHT——老 SDK 头里没有这个常量，手猜值翻过车。
        RECT hrc{};
        HWND hdr = ListView_GetHeader(h);
        // 表头高度取 item0 的 rect.bottom（客户区同原点）。不用 HDM_GETITEMHEIGHT——
        // 老头文件里没有这个常量，手猜消息值翻过车（返回 0，线画到顶边框上）。
        if (hdr && SendMessageW(hdr, HDM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&hrc))) {
            HDC dc = GetDC(h);
            if (dc) {
                RECT rc{};
                GetClientRect(h, &rc);
                RECT line{ 0, static_cast<int>(hrc.bottom), rc.right, static_cast<int>(hrc.bottom) + 1 };
                // 100,100,100 ≈ 控件主题边框色，比网格线深两档、比纯黑柔
                HBRUSH br = CreateSolidBrush(RGB(100, 100, 100));
                FillRect(dc, &line, br);
                DeleteObject(br);
                ReleaseDC(h, dc);
            }
        }
    }
    return r;
}

std::wstring GetText(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}
int GetInt(HWND h, int dflt)
{
    std::wstring s = GetText(h);
    if (s.empty()) return dflt;
    return _wtoi(s.c_str());
}

void ShowPage(Ctx& c, int idx)
{
    for (int p = 0; p < 5; p++)
        for (HWND h : c.pages[p])
            ShowWindow(h, p == idx ? SW_SHOW : SW_HIDE);
    c.cur_page = idx;
}

// 触发时间编辑框内容："9,21"（纯小时列表，逗号分隔）；无数据 "-"。
std::wstring HoursText(const TaskInfo& t)
{
    std::wstring s;
    for (int hv : t.hours) {
        if (!s.empty()) s += L",";
        s += std::to_wstring(hv);
    }
    return s.empty() ? L"-" : s;
}

void FillAccountList(Ctx& c, const Snapshot& sn)
{
    ListView_DeleteAllItems(c.acc_list);
    for (auto& a : sn.accounts) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = ListView_GetItemCount(c.acc_list);
        it.pszText = const_cast<LPWSTR>(a.nickname.c_str());
        int row = ListView_InsertItem(c.acc_list, &it);
        if (row < 0) continue;
        auto setcol = [&](int col, const std::wstring& v) {
            ListView_SetItemText(c.acc_list, row, col, const_cast<LPWSTR>(v.c_str()));
        };
        setcol(1, a.realm.empty() ? L"cn" : a.realm);
        setcol(2, FormatThousands(a.credits));
        // 实时列："剩余（已用/总量）"——used/size 来自 /admin/credits，查询失败/未查时只有 "-"
        std::wstring livev = L"-";
        for (auto& r : sn.credits.rows) {
            if (r.uid8 == a.uid8) {
                if (!r.ok) livev = L"失败";
                else if (r.used >= 0 && r.size >= 0)
                    livev = WideFormat(L"%s（%s/%s）", FormatThousands(r.remain).c_str(),
                        FormatThousands(r.used).c_str(), FormatThousands(r.size).c_str());
                else livev = FormatThousands(r.remain);
                break;
            }
        }
        setcol(3, livev);
        std::wstring st;
        if (a.disabled) st = a.reason.empty() ? L"已禁用" : L"已禁用 " + a.reason;
        else if (a.cooling) {
            // 服务端 cooling 是三合一口径（冷却/熔断/连败降权任一未到期）。按"哪一翼
            // 撑到最远"细分标注，降权再带上连败计数（阈值 5 次，见服务端 degrade_threshold）。
            int64_t nowx = NowSecX();
            if (a.degrade_until > nowx && a.degrade_until >= a.until && a.degrade_until >= a.breaker_until)
                st = WideFormat(L"降权至 %s(连败%d)", FormatTimeShort(a.degrade_until).c_str(), a.consec_fails);
            else if (a.breaker_until > nowx && a.breaker_until >= a.until)
                st = L"熔断至 " + FormatTimeShort(a.breaker_until);
            else
                st = a.until > nowx ? L"冷却至 " + FormatTimeShort(a.until) : L"冷却中";
        }
        else if (a.rl_models > 0) st = a.rl_until > NowSecX()
            ? WideFormat(L"模型限额×%d(至%s)", (int)a.rl_models, FormatTimeShort(a.rl_until).c_str())
            : WideFormat(L"模型限额×%d", (int)a.rl_models);
        else if (a.in_flight > 0) st = WideFormat(L"请求中(%d)", a.in_flight);
        else st = L"正常";
        setcol(4, st);
        setcol(5, a.token_expiry > NowSecX()
            ? WideFormat(L"%lld天", (a.token_expiry - NowSecX()) / 86400) : L"-");
    }
}

// 账户表列宽一次性预设：设计列宽按 96 DPI 标定（昵称88 域34 估算58 状态76 令牌剩50，
// 实时列吃余量），乘以"客户区实际宽 ÷ 设计总宽"的缩放系数分给固定列——高 DPI 下
// 控件像素变宽、列宽同步变大，恰好填满、不留无表头的空列（固定像素对不上控件宽
// 的截图 bug 来源）。只在建表时调一次，之后永不重设：运行期自适应会在 WM_SIZE/
// 翻页时把用户手动拖好的列宽弹回去（体验差，也造成表格闪动）。
void FitAccountColumnsOnce(Ctx& c)
{
    if (!c.acc_list) return;
    RECT rc{};
    GetClientRect(c.acc_list, &rc);
    int total = rc.right - rc.left;
    if (total <= 0) return; // 页②还没显示过：翻到页②时客户区才有宽
    static const int kDesign[] = { 88, 34, 58, 0, 76, 50 }; // 下标 3 = 实时列（吃余量）
    const int kFixedDesign = 88 + 34 + 58 + 76 + 50;        // 固定列设计合计 306
    const int kTotalDesign = kFixedDesign + 152;            // + 实时列设计宽 152 = 458
    int fixed = kFixedDesign * total / kTotalDesign;
    int live = total - fixed;
    if (live < 60) live = 60; // 极窄窗口下实时列保底可读
    // 累计取整：每列宽 = 缩放后的前缀和差值，整数除法误差不累计，总和恰好填满
    int prefix = 0, prev_end = 0;
    for (int i = 0; i < 6; i++) {
        int w;
        if (!kDesign[i]) {
            w = live;
        } else {
            prefix += kDesign[i];
            int end = prefix * fixed / kFixedDesign;
            w = end - prev_end;
            prev_end = end;
        }
        ListView_SetColumnWidth(c.acc_list, i, w);
    }
}

void Refresh(Ctx& c)
{
    Snapshot sn = Worker::Instance().Copy();

    // —— 页① 状态 ——
    const wchar_t* stxt = L"状态：未知";
    c.state_color = RGB(120, 120, 120);
    switch (sn.state) {
    case SvcState::Running:      stxt = L"状态：● 运行中"; c.state_color = RGB(16, 130, 40); break;
    case SvcState::Unservable:   stxt = L"状态：● 运行中（暂无可用账号）"; c.state_color = RGB(215, 130, 0); break;
    case SvcState::Starting:     stxt = L"状态：● 启动中…"; c.state_color = RGB(30, 110, 220); break;
    case SvcState::Stopped:      stxt = L"状态：○ 已停止"; break;
    case SvcState::WrongService: stxt = L"状态：✕ 端口被其他程序占用"; c.state_color = RGB(200, 30, 30); break;
    case SvcState::Dead:         stxt = L"状态：✕ 进程无响应"; c.state_color = RGB(200, 30, 30); break;
    }
    std::wstring st2 = std::wstring(stxt);
    if (sn.pid) st2 += WideFormat(L"（PID %lu）", sn.pid);
    SetWindowTextW(c.state_lbl, st2.c_str());
    InvalidateRect(c.state_lbl, nullptr, TRUE);
    std::wstring sub = WideFormat(L"地址 http://127.0.0.1:%d", c.work.port);
    if (sn.last_ok_ts) sub += L"  ·  最后成功 " + FormatTimeShort(sn.last_ok_ts);
    SetWindowTextW(c.sub_lbl, sub.c_str());
    SetWindowTextW(c.err_lbl, sn.last_error.empty() ? L" " : sn.last_error.c_str());
    InvalidateRect(c.err_lbl, nullptr, TRUE);
    SetWindowTextW(c.action_lbl,
        (sn.action_note.empty() || NowSecX() - sn.action_note_ts > 25) ? L" " : sn.action_note.c_str());

    bool svc_busy = Worker::Instance().IsActionBusy("svc");
    bool on = StateIsOn(sn.state);
    bool off = sn.state == SvcState::Stopped || sn.state == SvcState::Unknown || sn.state == SvcState::Dead;
    EnableWindow(c.btn_start, !svc_busy && off);
    EnableWindow(c.btn_stop, !svc_busy && on);
    EnableWindow(c.btn_restart, !svc_busy);

    // —— 页② 积分 ——
    bool cred_busy = Worker::Instance().IsActionBusy("credits");
    EnableWindow(c.btn_cred, !cred_busy && on && sn.admin_available && sn.credits.cooldown_until <= NowSecX());
    std::wstring cool;
    if (!sn.admin_available) cool = L"/admin 未启用：无法查询实时积分（见「定时任务」页提示）";
    else if (sn.credits.cooldown_until > NowSecX())
        cool = WideFormat(L"服务端限频：还差 %lld 秒（上次查询 %s）",
            sn.credits.cooldown_until - NowSecX(), FormatTimeShort(sn.credits.ts).c_str());
    else if (sn.credits.have)
        cool = WideFormat(L"上次 %s · 总剩 %s · 已用 %s · 现在可查",
            FormatTimeShort(sn.credits.ts).c_str(), FormatThousands(sn.credits.total_remain).c_str(),
            sn.credits.total_used >= 0 ? FormatThousands(sn.credits.total_used).c_str() : L"-");
    else cool = L"尚未查询 · 按钮会逐号向服务端发起实时余额查询";
    SetWindowTextW(c.lbl_cool, cool.c_str());
    int64_t sig = sn.last_ok_ts * 131 + sn.credits.ts * 7 + static_cast<int64_t>(sn.accounts.size());
    if (on && sn.accounts_valid && sig != c.acc_sig) {
        c.acc_sig = sig;
        FillAccountList(c, sn);
    }

    // —— 页③ 任务 ——
    std::wstring warn = L" ";
    if (!sn.admin_available)
        warn = L"⚠ /admin 未启用：升级 wb2api.exe 并在其 config.json 设 \"admin\":{\"enabled\":true} 后重启服务";
    SetWindowTextW(c.task_warn, warn.c_str());
    for (int i = 0; i < KIND_N; i++) {
        const TaskInfo* t = nullptr;
        for (auto& x : sn.tasks)
            if (x.kind == kKinds[i]) t = &x;
        bool busy = Worker::Instance().IsActionBusy(std::string("task:") + kKinds[i]);
        if (t) {
            SendMessageW(c.task[i].chk, BM_SETCHECK, t->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            std::wstring nxt = t->enabled
                ? (t->next_fire.empty() ? std::wstring(L"-") : t->next_fire)
                : std::wstring(L"停用");
            SetWindowTextW(c.task[i].next, nxt.c_str());
            std::wstring status;
            if (t->running) status = L"执行中…";
            else if (t->last_run) status = FormatTimeShort(t->last_run) + L" " + t->last_result;
            else status = L"本进程未执行过";
            SetWindowTextW(c.task[i].status, status.c_str());
            // 触发时间输入框：非焦点时回填服务端快照（焦点=用户可能正在编辑，跳过）。
            std::wstring want = HoursText(*t);
            if (GetFocus() != c.task[i].time && c.task[i].synced != want) {
                SetWindowTextW(c.task[i].time, want.c_str());
                c.task[i].synced = want;
            }
        } else {
            SetWindowTextW(c.task[i].next, L"-");
            SetWindowTextW(c.task[i].status, L"-");
        }
        EnableWindow(c.task[i].chk, sn.admin_available && t && !busy);
        EnableWindow(c.task[i].btn, sn.admin_available && t && !busy && on);
        EnableWindow(c.task[i].apply,
            sn.admin_available && t && !Worker::Instance().IsActionBusy(std::string("taskhours:") + kKinds[i]));
    }
    EnableWindow(c.btn_runall, sn.admin_available && on && !Worker::Instance().IsActionBusy("task:all"));
    SetWindowTextW(c.task_note, sn.tasks_ts
        ? WideFormat(L"快照更新于 %s · 勾选=即时生效并写回 config.json", FormatTimeShort(sn.tasks_ts).c_str()).c_str()
        : L"等待任务快照…");

    // —— 页⑤ ——
    SetWindowTextW(c.admin_stat,
        sn.admin_available ? L"/admin：可用" : L"/admin：未检测到（服务未升级或开关未启用）");
}

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Ctx* cp = reinterpret_cast<Ctx*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        // 此刻 USERDATA 尚未写入：必须从 CREATESTRUCT 取实例指针（经典坑）。
        // WM_INITDIALOG 的 lParam 就是 DialogBoxParamW 的 dwInitParam（即 &Ctx），
        // 不是 CREATESTRUCT*（那是 WM_CREATE 的约定）。直接强转即可。
        cp = reinterpret_cast<Ctx*>(lp);
        SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cp));
        cp->dlg = hDlg;
        cp->font = reinterpret_cast<HFONT>(SendMessageW(hDlg, WM_GETFONT, 0, 0));
        cp->tab = GetDlgItem(hDlg, IDC_TAB);
        // 页体对齐色：tab 页体在 aero 主题下是 F9F9F9 浅灰，但不同主题/系统会变
        // （COLOR_WINDOW 白、COLOR_BTNFACE 灰、深色主题更暗）。不猜系统色——
        // 让 tab 控件在 WM_PRINTCLIENT 里把自己画进内存位图，采样 tab 显示区
        // 中心一点的像素作为页体真实色。采样失败则回退 COLOR_WINDOW。
        {
            RECT rc{};
            TabCtrl_GetItemRect(cp->tab, 0, &rc); // 仅确认 tab 有尺寸，取显示区用 AdjustRect
            RECT disp{ 0, 0, 200, 60 };
            TabCtrl_AdjustRect(cp->tab, FALSE, &disp);
            COLORREF body = CLR_NONE;
            int cx = (disp.left + disp.right) / 2, cy = (disp.top + disp.bottom) / 2;
            if (HDC wdc = GetWindowDC(cp->tab)) {
                HDC mdc = CreateCompatibleDC(wdc);
                HBITMAP bmp = CreateCompatibleBitmap(wdc, disp.right - disp.left + 40, disp.bottom - disp.top + 40);
                HBITMAP old = reinterpret_cast<HBITMAP>(SelectObject(mdc, bmp));
                SetWindowOrgEx(mdc, disp.left - 20, disp.top - 20, nullptr);
                SendMessageW(cp->tab, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mdc), PRF_CLIENT);
                body = GetPixel(mdc, cx, cy);
                SelectObject(mdc, old);
                DeleteObject(bmp);
                DeleteDC(mdc);
                ReleaseDC(cp->tab, wdc);
                // 全黑多半是位图没画上（个别主题包装器不支持 WM_PRINTCLIENT），回退
                if (body == RGB(0, 0, 0)) body = CLR_NONE;
            }
            cp->bg_brush = CreateSolidBrush(body != CLR_NONE ? body : GetSysColor(COLOR_WINDOW));
        }
        struct { LPCWSTR t; } tabs[] = { { L"服务" }, { L"账户与积分" }, { L"定时任务" }, { L"显示" }, { L"高级" } };
        for (int i = 0; i < 5; i++) {
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<LPWSTR>(tabs[i].t);
            TabCtrl_InsertItem(cp->tab, i, &ti);
        }

        // —— 页① 服务 ——
        // 布局：状态区 → 服务目录行 → 端口/轮询行 → 按钮行 → 自启动选项 → 提示区。
        // 标签右对齐到 x=50，输入框统一从 x=54 起（DLU），行距 20。
        cp->state_lbl = MkLabel(*cp, L"状态：未知", 8, 6, 404, 10, 0);
        cp->sub_lbl = MkLabel(*cp, L"", 8, 20, 404, 10, 0);
        MkLabel(*cp, L"服务目录:", 8, 42, 42, 10, 0);
        cp->dir_edt = MkEdit(*cp, cp->work.service_dir.c_str(), 54, 40, 280, IDC_EDT_DIR, 0);
        MkBtn(*cp, L"浏览…", 338, 40, 46, 14, IDC_BTN_BROWSE, 0);
        MkLabel(*cp, L"端口:", 8, 62, 42, 10, 0);
        cp->port_edt = MkEdit(*cp, std::to_wstring(cp->work.port).c_str(), 54, 60, 40, IDC_EDT_PORT, 0, ES_NUMBER);
        MkLabel(*cp, L"轮询秒:", 100, 62, 40, 10, 0);
        cp->poll_edt = MkEdit(*cp, std::to_wstring(cp->work.poll_interval_sec).c_str(), 142, 60, 40, IDC_EDT_POLL, 0, ES_NUMBER);
        MkHint(*cp, L"鉴权自动读服务目录 config.json 的 api_key，无需在此填写。轮询 ≥10 秒（/status 免费，无风控压力）。",
            8, 76, 404, 10, 0);
        cp->btn_start = MkBtn(*cp, L"启动服务", 8, 92, 60, 14, IDC_BTN_START, 0);
        cp->btn_stop = MkBtn(*cp, L"停止服务", 74, 92, 60, 14, IDC_BTN_STOP, 0);
        cp->btn_restart = MkBtn(*cp, L"重启服务", 140, 92, 60, 14, IDC_BTN_RESTART, 0);
        cp->chk_auto = MkCheck(*cp, L"开机自动启动（计划任务，登录+10秒延迟）", 8, 114, 250, IDC_CHK_AUTOSTART, 0);
        cp->chk_tm = MkCheck(*cp, L"随 TrafficMonitor 启动时拉起", 8, 128, 200, IDC_CHK_TM, 0);
        cp->chk_relaunch = MkCheck(*cp, L"意外停止自动拉起（10 分钟最多 3 次）", 8, 142, 250, IDC_CHK_RELAUNCH, 0);
        cp->chk_log = MkCheck(*cp, L"调试日志（写插件配置目录）", 8, 156, 200, IDC_CHK_LOG, 0);
        SendMessageW(cp->chk_auto, BM_SETCHECK, cp->work.autostart_task ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(cp->chk_tm, BM_SETCHECK, cp->work.start_with_tm ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(cp->chk_relaunch, BM_SETCHECK, cp->work.auto_relaunch ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(cp->chk_log, BM_SETCHECK, cp->work.logging ? BST_CHECKED : BST_UNCHECKED, 0);
        cp->err_lbl = MkLabel(*cp, L" ", 8, 174, 404, 10, 0);
        cp->action_lbl = MkLabel(*cp, L" ", 8, 188, 404, 10, 0);

        // —— 页② 账户与积分 ——
        // 列宽一次性预设（见 FitAccountColumnsOnce）：建表时按客户区宽定死，之后
        // 不随窗口缩放/翻页重设——用户可自由拖动列宽，不会被弹回。
        cp->acc_list = MkWnd(*cp, WC_LISTVIEWW, L"",
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER, 0, 8, 8, 462, 140, IDC_LST_ACC, 1);
        // LVS_EX_DOUBLEBUFFER：列表整帧先进内存位图再上屏，拖列宽/滚动的重绘不闪。
        ListView_SetExtendedListViewStyle(cp->acc_list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        // 默认 aero 主题下 LVS_EX_GRIDLINES 的竖线与表头分隔线错位数像素、表头下沿缺一条
        // 横线（经典毛病）。切 Explorer 主题后表头与网格线走同一套绘制，行列线对齐。
        SetWindowTheme(cp->acc_list, L"Explorer", nullptr);
        SetWindowSubclass(cp->acc_list, AccListProc, 1, 0);
        struct Col { LPCWSTR t; int w; };
        const Col cols[] = { { L"昵称", 88 }, { L"域", 34 }, { L"估算", 58 }, { L"实时(已用/总量)", 150 }, { L"状态", 76 }, { L"令牌剩", 50 } };
        for (int i = 0; i < 6; i++) {
            LVCOLUMNW cv{};
            cv.mask = LVCF_TEXT | LVCF_WIDTH;
            cv.pszText = const_cast<LPWSTR>(cols[i].t);
            cv.cx = cols[i].w;
            ListView_InsertColumn(cp->acc_list, i, &cv);
        }
        // 建表后一次性定列宽：此刻客户区宽即初始可视宽，之后不再重设（可自由拖动）。
        FitAccountColumnsOnce(*cp);
        cp->btn_cred = MkBtn(*cp, L"查询实时积分", 8, 156, 80, 14, IDC_BTN_CRED, 1);
        cp->lbl_cool = MkLabel(*cp, L"", 94, 158, 370, 10, 1);
        MkLabel(*cp, L"自动刷新周期(分钟,0=关,≥1):", 8, 178, 150, 10, 1);
        cp->citv_edt = MkEdit(*cp, std::to_wstring(cp->work.credits_refresh_interval_min).c_str(), 160, 176, 34, IDC_EDT_CINTERVAL, 1, ES_NUMBER);
        MkHint(*cp, L"估算=本地账本，插件轮询零成本；实时=服务端逐号查上游余额并回写账本，括号内为该号已用/原始总量。",
            8, 194, 404, 10, 1);
        MkHint(*cp, L"周期保存时自动同步到服务端冷却（PATCH /admin/credits-interval，免重启、写回服务端 config.json 留 .bak）。",
            8, 206, 404, 10, 1);
        MkHint(*cp, L"插件只按服务端允许的节奏查询，不会触发 429；双击账户行可看该号每模型实测成本台账。",
            8, 218, 404, 10, 1);

        // —— 页③ 定时任务 ——
        // 表头一行 + 每任务一行：勾选 | 触发时间(可编辑) | 下次 | 上次/状态 | 立即执行 | 应用。
        // 状态列加宽到 172 并把"应用"压到 x=414：旧布局 150 宽度装不下长结果
        // （"ok=0 already=4 fail=0 skipped=0"约 40 字符）被截换行，视觉上像被下一行遮挡。
        // 页面可用宽度 ≈472 DLU（对话框 500 减边框/tab 边距），列宽合计 8+70+44+8+40+8+172+8+44+8+40 ≈ 458。
        const struct { LPCWSTR t; int x; int w; } hdr[] = {
            { L"任务", 10, 64 }, { L"触发时间(点)", 82, 46 }, { L"下次", 150, 40 },
            { L"状态(上次结果)", 194, 176 }, { L"", 374, 44 }, { L"", 420, 40 },
        };
        for (int k = 0; k < 6; k++) MkLabel(*cp, hdr[k].t, hdr[k].x, 24, hdr[k].w, 10, 2);
        cp->task_warn = MkLabel(*cp, L" ", 8, 8, 404, 10, 2);
        for (int i = 0; i < KIND_N; i++) {
            int y = 42 + i * 26;
            cp->task[i].chk = MkCheck(*cp, kKindZh[i], 8, y, 70, IDC_TASK_BASE + i * 10, 2);
            // 时间输入框不用 ES_NUMBER：内容是"9,21"逗号分隔小时列表
            cp->task[i].time = MkEdit(*cp, L"-", 82, y - 2, 44, IDC_TASK_BASE + i * 10 + 6, 2);
            cp->task[i].next = MkLabel(*cp, L"-", 150, y, 40, 10, 2);
            cp->task[i].status = MkLabel(*cp, L"-", 194, y, 176, 10, 2);
            cp->task[i].btn = MkBtn(*cp, L"立即执行", 374, y - 2, 44, 14, IDC_TASK_BASE + i * 10 + 5, 2);
            cp->task[i].apply = MkBtn(*cp, L"应用", 420, y - 2, 40, 14, IDC_TASK_BASE + i * 10 + 7, 2);
        }
        cp->btn_runall = MkBtn(*cp, L"全部执行", 8, 202, 60, 14, IDC_BTN_RUNALL, 2);
        cp->task_note = MkLabel(*cp, L"", 76, 204, 390, 10, 2);
        MkHint(*cp, L"触发时间=24 小时制小时列表（逗号分隔，如 9,21）；「应用」写回服务 config.json，重启服务后生效。",
            8, 220, 404, 10, 2);
        MkHint(*cp, L"立即执行在服务进程内跑（与定时任务同一把锁）；状态列显示上次执行结果与耗时。", 8, 232, 404, 10, 2);

        // —— 页④ 显示 ——
        cp->rad[0] = MkWnd(*cp, L"BUTTON", L"状态 + 账号数（如 4/4）", BS_AUTORADIOBUTTON | WS_TABSTOP, 0, 8, 8, 220, 10, IDC_RAD_ACCOUNT, 3);
        cp->rad[1] = MkWnd(*cp, L"BUTTON", L"状态 + 积分（如 5.6k）", BS_AUTORADIOBUTTON | WS_TABSTOP, 0, 8, 24, 220, 10, IDC_RAD_CREDITS, 3);
        cp->rad[2] = MkWnd(*cp, L"BUTTON", L"仅状态词（运行/停止）", BS_AUTORADIOBUTTON | WS_TABSTOP, 0, 8, 40, 220, 10, IDC_RAD_ONLY, 3);
        cp->chk_live = MkCheck(*cp, L"积分优先显示实时值（有缓存时）", 8, 58, 240, IDC_CHK_LIVECRD, 3);
        cp->chk_tipfull = MkCheck(*cp, L"悬浮提示完整展开（多插件同载弹参数错误时关闭此项）", 8, 74, 340, IDC_CHK_TIPFULL, 3);
        // MkWnd 给所有控件都加了 WS_GROUP，会让每个单选各自成组、点不互相取消；
        // 清掉后两个的 WS_GROUP，让三个 radio（Z 序相邻）构成同一个互斥组；
        // 再清 WS_TABSTOP（标准组语义：仅组首有 Tab 停靠，组内靠方向键移动）。
        for (int i = 1; i < 3; i++)
            SetWindowLongW(cp->rad[i], GWL_STYLE, GetWindowLongW(cp->rad[i], GWL_STYLE) & ~(WS_GROUP | WS_TABSTOP));
        MkHint(*cp, L"状态点颜色：绿=运行且可用 · 橙=在跑无可用账号 · 灰=已停止 · 红=端口被占/无响应", 8, 92, 404, 10, 3);
        MkHint(*cp, L"任务栏宽度按最长样例预留；单击任务栏上的本栏位即可打开此设置窗。", 8, 106, 404, 10, 3);
        if (cp->work.show_mode >= 0 && cp->work.show_mode <= 2)
            SendMessageW(cp->rad[cp->work.show_mode], BM_SETCHECK, BST_CHECKED, 0);
        else
            SendMessageW(cp->rad[0], BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(cp->chk_live, BM_SETCHECK, cp->work.show_live_credits ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(cp->chk_tipfull, BM_SETCHECK, cp->work.tooltip_full ? BST_CHECKED : BST_UNCHECKED, 0);

        // —— 页⑤ 高级 ——
        MkLabel(*cp, L"管理接口轮询(秒,≥15):", 8, 10, 100, 10, 4);
        cp->admin_edt = MkEdit(*cp, std::to_wstring(cp->work.admin_poll_sec).c_str(), 110, 8, 34, IDC_EDT_ADMIN, 4, ES_NUMBER);
        cp->admin_stat = MkLabel(*cp, L"", 8, 28, 460, 10, 4);
        MkBtn(*cp, L"打开插件配置目录", 8, 46, 100, 14, IDC_BTN_OPENCFG, 4);
        MkBtn(*cp, L"打开服务目录", 112, 46, 80, 14, IDC_BTN_OPENSVC, 4);
        MkBtn(*cp, L"打开服务日志", 196, 46, 80, 14, IDC_BTN_OPENLOG, 4);
        MkBtn(*cp, L"打开插件日志", 280, 46, 80, 14, IDC_BTN_OPENPLOG, 4);
        MkLabel(*cp, L"WorkBuddy2API TrafficMonitor 插件 v1.2.0 · MIT", 8, 70, 404, 10, 4);
        MkHint(*cp, L"https://github.com/Arimayuki03/workbuddy2api-trafficmonitor-plugin", 8, 84, 404, 10, 4);
        MkHint(*cp, L"设计约束：/healthz /status /admin 均为本机回环接口；本插件永不调用 /v1/chat/completions，", 8, 104, 460, 10, 4);
        MkHint(*cp, L"与你的 API 使用互不影响。实时积分查询由服务端冷却与单飞兜底，防止任何路径触发上游风控。", 8, 118, 460, 10, 4);

        ShowPage(*cp, 0);
        SetTimer(hDlg, 7, 1000, nullptr);
        Refresh(*cp);
        return TRUE;
    }
    case WM_TIMER:
        if (wp == 7 && cp) Refresh(*cp);
        return TRUE;
    case WM_NOTIFY: {
        if (!cp) return FALSE;
        LPNMHDR nh = reinterpret_cast<LPNMHDR>(lp);
        if (nh->idFrom == IDC_TAB && nh->code == TCN_SELCHANGE) {
            ShowPage(*cp, static_cast<int>(TabCtrl_GetCurSel(cp->tab)));
            return TRUE;
        }
        // 双击（或回车）账户行 → 该号每模型实测成本台账（/status accounts[].model_costs，
        // wb2api 上游 2493532 透出）。口径与 wb2api 的 status-report.ps1 一致：
        // 每1k=实测千 token 均价（EMA，≤0 即实测免费），6 小时无观测服务端自动删行。
        if (cp->acc_list && nh->hwndFrom == cp->acc_list && nh->code == LVN_ITEMACTIVATE) {
            int idx = (int)ListView_GetNextItem(cp->acc_list, -1, LVNI_SELECTED);
            Snapshot sn = Worker::Instance().Copy();
            if (idx < 0 || idx >= (int)sn.accounts.size()) return TRUE;
            const AccountInfo& a = sn.accounts[idx];
            std::wstring box = a.nickname + L"（" + (a.realm.empty() ? L"cn" : a.realm) +
                L"）每模型实测成本\n\n";
            if (a.costs.empty()) {
                box += L"（暂无观测：该号还没处理过可记账的请求，或观测已过 6 小时被服务端回收）";
            } else {
                box += L"模型｜每1k均价｜样本｜末次观测\n";
                for (auto& m : a.costs) {
                    // 3 位小数（同 status-report.ps1 的 N3 口径）：实测单价常见 0.00x 量级，
                    // 2 位会把 0.0034 显示成 "0.00"，与"免费"混淆。
                    box += WideFormat(L"%s｜%s｜%d｜%s\n", m.model.c_str(),
                        m.per1k <= 0 ? L"免费" : WideFormat(L"%.3f", m.per1k).c_str(),
                        m.samples,
                        m.last_seen ? FormatTimeShort(m.last_seen).c_str() : L"-");
                }
                box += L"\n选号按便宜优先；≤0=实测免费，数字为积分/千 token。";
            }
            MessageBoxW(hDlg, box.c_str(), L"成本台账", MB_OK);
            return TRUE;
        }
        return FALSE;
    }
    case WM_COMMAND: {
        if (!cp) return FALSE;
        int id = LOWORD(wp), code = HIWORD(wp);
        Ctx& c = *cp;
        // 门禁：只放行 BN_CLICKED。BS_NOTIFY 焦点类通知（BN_SETFOCUS/BN_KILLFOCUS）也走
        // WM_COMMAND，explorer 弹窗抢/还焦点会连环触发——曾让"打开目录/日志"一次点出
        // 多个窗口、关掉又自动重开。输入框 EN_CHANGE 等同样无需处理。
        // IDOK/IDCANCEL 由对话框管理器合成，不走此门禁。
        if (id != IDOK && id != IDCANCEL && code != BN_CLICKED) return TRUE;
        switch (id) {
        case IDOK: {
            c.work.service_dir = TrimW(GetText(c.dir_edt));
            c.work.port = GetInt(c.port_edt, 7863);
            c.work.poll_interval_sec = GetInt(c.poll_edt, 30);
            c.work.admin_poll_sec = GetInt(c.admin_edt, 60);
            c.work.credits_refresh_interval_min = GetInt(c.citv_edt, 0);
            c.work.autostart_task = SendMessageW(c.chk_auto, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.work.start_with_tm = SendMessageW(c.chk_tm, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.work.auto_relaunch = SendMessageW(c.chk_relaunch, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.work.logging = SendMessageW(c.chk_log, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.work.show_live_credits = SendMessageW(c.chk_live, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.work.tooltip_full = SendMessageW(c.chk_tipfull, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.work.show_mode = SendMessageW(c.rad[0], BM_GETCHECK, 0, 0) == BST_CHECKED ? SM_STATE_ACCOUNT
                : (SendMessageW(c.rad[1], BM_GETCHECK, 0, 0) == BST_CHECKED ? SM_STATE_CREDITS : SM_STATE_ONLY);
            std::wstring bad;
            if (c.work.service_dir.empty()) bad = L"服务目录不能为空";
            else if (c.work.port <= 0 || c.work.port > 65535) bad = L"端口非法";
            else if (c.work.poll_interval_sec < 10 || c.work.poll_interval_sec > 600) bad = L"轮询间隔需 10–600 秒";
            else if (c.work.admin_poll_sec < 15 || c.work.admin_poll_sec > 600) bad = L"管理轮询需 15–600 秒";
            else if (c.work.credits_refresh_interval_min < 0 || c.work.credits_refresh_interval_min > 1440)
                bad = L"自动刷新积分需 0(关) 或 1–1440 分钟";
            if (!bad.empty()) {
                MessageBoxW(hDlg, bad.c_str(), L"设置未保存", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            // 积分自动刷新周期先于落盘同步到服务端：改失败只提示不阻断保存
            // （服务端可能未升级/未开 admin），插件侧仍按本地值尽力而为。
            if (c.work.credits_refresh_interval_min != c.orig.credits_refresh_interval_min) {
                SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                std::wstring sync_err = Worker::Instance().SyncCreditsIntervalBlocking(
                    c.work.credits_refresh_interval_min);
                if (!sync_err.empty()) {
                    MessageBoxW(hDlg, (L"周期已保存到插件，但同步服务端失败：\n" + sync_err +
                        L"\n\n服务端仍按其 config.json 里的冷却执行；升级 wb2api 后重试。").c_str(),
                        L"服务端冷却同步失败", MB_OK | MB_ICONWARNING);
                }
            }
            if (c.work.autostart_task != c.orig.autostart_task) {
                SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                if (c.work.autostart_task) {
                    std::wstring err;
                    if (!autostart::Install(err)) {
                        c.work.autostart_task = c.orig.autostart_task;
                        MessageBoxW(hDlg, err.c_str(), L"开机自启注册失败", MB_OK | MB_ICONERROR);
                    }
                } else {
                    std::wstring err;
                    autostart::Uninstall(err);
                }
            }
            if (c.work.logging != c.orig.logging && !c.work.config_dir.empty())
                LogInit(c.work.config_dir + L"\\WorkBuddy2ApiPlugin.log", c.work.logging);
            bool changed = !SettingsEqual(c.work, c.orig);
            if (changed) {
                Settings cur = c.work;
                cur.user_stopped = c.orig.user_stopped; // 运行期标志不经对话框
                SettingsStore::Instance().Update(cur);
                Worker::Instance().RefreshSoon();
            }
            c.changed_flag = changed;
            KillTimer(hDlg, 7);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            KillTimer(hDlg, 7);
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        case IDC_BTN_START: Worker::Instance().RequestStartService(); return TRUE;
        case IDC_BTN_STOP: Worker::Instance().RequestStopService(); return TRUE;
        case IDC_BTN_RESTART: Worker::Instance().RequestRestartService(); return TRUE;
        case IDC_BTN_CRED: Worker::Instance().RequestRefreshCredits(); return TRUE;
        case IDC_BTN_BROWSE: {
            // SHBrowseForFolderW 内部走 COM（shell folder 枚举）且要求调用线程是 STA。
            // 插件对话框线程从未初始化 COM：弹窗内部消息循环等一个永远不会到的跨套间
            // 回应，整个设置窗（连同宿主 UI 线程）直接卡死。任何返回值都配对
            // CoUninitialize；同线程已初始化过（S_FALSE/SEC 重点）也不多还一次。
            HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(cohr)) return TRUE; // COM 起不来：放弃弹窗也不能吊死 UI
            BROWSEINFOW bi{};
            bi.hwndOwner = hDlg;
            bi.lpszTitle = L"选择 workbuddy2api 服务目录（含 wb2api.exe 的文件夹）";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                std::vector<wchar_t> path(MAX_PATH * 4, 0);
                if (SHGetPathFromIDListW(pidl, path.data())) SetWindowTextW(c.dir_edt, path.data());
                CoTaskMemFree(pidl);
            }
            CoUninitialize();
            return TRUE;
        }
        case IDC_BTN_OPENCFG:
            if (!c.work.config_dir.empty())
                ShellExecuteW(hDlg, L"open", c.work.config_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        case IDC_BTN_OPENSVC: {
            std::wstring d = GetText(c.dir_edt);
            if (!d.empty()) ShellExecuteW(hDlg, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        case IDC_BTN_OPENLOG: {
            std::wstring d = GetText(c.dir_edt);
            if (!d.empty()) ShellExecuteW(hDlg, L"open", (d + L"\\logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        case IDC_BTN_OPENPLOG: {
            std::wstring p = c.work.config_dir + L"\\WorkBuddy2ApiPlugin.log";
            if (PathFileExistsW(p.c_str()))
                ShellExecuteW(hDlg, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else
                MessageBoxW(hDlg, L"插件日志未启用或尚无内容（页①勾选调试日志并保存）",
                    L"插件日志", MB_OK | MB_ICONINFORMATION);
            return TRUE;
        }
        default: {
            if (id >= IDC_TASK_BASE && id < IDC_TASK_BASE + KIND_N * 10 && code == BN_CLICKED) {
                int base = (id - IDC_TASK_BASE) / 10;
                int slot = (id - IDC_TASK_BASE) % 10;
                if (base < KIND_N) {
                    if (slot == 0) {
                        bool ck = SendMessageW(c.task[base].chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        Worker::Instance().RequestToggleTask(kKinds[base], ck);
                    } else if (slot == 5) {
                        Worker::Instance().RequestRunTask(kKinds[base]);
                    } else if (slot == 7) {
                        // 「应用」：把输入框的"9,21"解析成小时列表写回服务 config.json（重启生效）
                        std::wstring raw = GetText(c.task[base].time);
                        std::vector<int> hours;
                        bool bad = false;
                        // 按逗号（中英文皆可）/空白切分；只收 0-23 的纯数字
                        size_t pos = 0;
                        while (pos < raw.size() && !bad) {
                            size_t e = raw.find_first_of(L",， \t", pos);
                            if (e == std::wstring::npos) e = raw.size();
                            if (e > pos) {
                                std::wstring seg = raw.substr(pos, e - pos);
                                int hv = _wtoi(seg.c_str());
                                if (hv < 0 || hv > 23 ||
                                    seg.find_first_not_of(L"0123456789") != std::wstring::npos)
                                    bad = true;
                                else hours.push_back(hv);
                            }
                            pos = e + 1;
                        }
                        if (bad || hours.empty())
                            MessageBoxW(hDlg, L"触发时间需为 0-23 的小时数字，用逗号分隔（如 9,21）",
                                L"格式不对", MB_OK | MB_ICONWARNING);
                        else
                            Worker::Instance().RequestSetTaskHours(kKinds[base], hours);
                    }
                }
                return TRUE;
            }
            if (id == IDC_BTN_RUNALL && code == BN_CLICKED) {
                Worker::Instance().RequestRunTask("all");
                return TRUE;
            }
            return FALSE;
        }
        }
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN:
        // 对话框边距与按钮贴边处默认走类刷 COLOR_BTNFACE，在白色页体旁露出浅灰带；
        // 同样统一到页体刷。checkbox/radio 按文档走 WM_CTLCOLORSTATIC（见下），
        // pushbutton 主题化后不消费 CTLCOLORBTN，此处只兜底非主题场景。
        if (cp && cp->bg_brush)
            return reinterpret_cast<INT_PTR>(cp->bg_brush);
        return FALSE;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        HWND h = reinterpret_cast<HWND>(lp);
        SetBkMode(dc, TRANSPARENT);
        if (!cp || !cp->bg_brush)
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        // 静态控件/复选框背景与对话框同用页体色刷：原 GetSysColorBrush(COLOR_BTNFACE)
        // 在白底主题下给每条标签拖出一圈灰带（截图里像"阴影"）。
        if (h == cp->state_lbl || h == cp->err_lbl)
            SetTextColor(dc, cp->state_color);
        else if (std::find(cp->hints.begin(), cp->hints.end(), h) != cp->hints.end())
            SetTextColor(dc, RGB(128, 128, 128)); // 次要说明文字
        return reinterpret_cast<INT_PTR>(cp->bg_brush);
    }
    case WM_CLOSE:
        if (cp) { KillTimer(hDlg, 7); EndDialog(hDlg, IDCANCEL); }
        return TRUE;
    case WM_NCDESTROY:
        if (cp && cp->bg_brush) { DeleteObject(cp->bg_brush); cp->bg_brush = nullptr; }
        return FALSE;
    default:
        return FALSE;
    }
}

} // namespace

ITMPlugin::OptionReturn ShowSettingsDialog(HWND parent)
{
    static std::once_flag once;
    std::call_once(once, [] {
        INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
    });
    if (!parent) parent = GetActiveWindow();
    Ctx c;
    c.orig = SettingsStore::Instance().Get();
    c.work = c.orig;
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_SETTINGS), parent, DlgProc,
        reinterpret_cast<LPARAM>(&c));
    return c.changed_flag ? ITMPlugin::OR_OPTION_CHANGED : ITMPlugin::OR_OPTION_UNCHANGED;
}

} // namespace wb2::dlg
