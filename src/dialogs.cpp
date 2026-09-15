// dialogs.cpp — 设置对话框。约定：
//  * 打开时拷贝一份 Settings 作为"编辑中"副本，点"保存"才落盘并 diff 应用副作用（计划任务等）；
//  * 实时信息（状态/账户/任务/冷却）每秒从 Worker 快照刷新，只改展示控件，不碰输入框；
//  * 任务勾选 = 即时动作（PATCH），不等"保存"——它管理的是服务本体，不是插件配置；
//  * 按钮 busy 态由 Worker::IsActionBusy 驱动防连点；实时积分还有服务端 429 第二道闸。
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
#include <ctime>
#include <vector>

#pragma comment(lib, "comctl32.lib")
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
        a.api_key_manual == b.api_key_manual && a.poll_interval_sec == b.poll_interval_sec &&
        a.admin_poll_sec == b.admin_poll_sec &&
        a.credits_refresh_interval_min == b.credits_refresh_interval_min &&
        a.show_mode == b.show_mode && a.show_live_credits == b.show_live_credits &&
        a.autostart_task == b.autostart_task && a.start_with_tm == b.start_with_tm &&
        a.auto_relaunch == b.auto_relaunch && a.logging == b.logging;
}

struct TaskRow {
    HWND chk = nullptr, hours = nullptr, next = nullptr, status = nullptr, btn = nullptr;
};

struct Ctx {
    Settings work, orig;
    bool changed_flag = false;
    HWND dlg = nullptr, tab = nullptr;
    std::vector<HWND> pages[5];
    int cur_page = 0;
    HFONT font = nullptr;
    // 页①
    HWND state_lbl = nullptr, sub_lbl = nullptr, err_lbl = nullptr, action_lbl = nullptr;
    HWND dir_edt = nullptr, port_edt = nullptr, key_edt = nullptr, poll_edt = nullptr;
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
    HWND rad[3] = {}, chk_live = nullptr;
    // 页⑤
    HWND admin_edt = nullptr, admin_stat = nullptr;
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
HWND MkCheck(Ctx& c, LPCWSTR t, int l, int tp, int w, int id, int page)
{
    return MkWnd(c, L"BUTTON", t, BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP, 0, l, tp, w, 10, id, page);
}
HWND MkBtn(Ctx& c, LPCWSTR t, int l, int tp, int w, int h, int id, int page)
{
    return MkWnd(c, L"BUTTON", t, BS_PUSHBUTTON | BS_NOTIFY | WS_TABSTOP, 0, l, tp, w, h, id, page);
}
HWND MkEdit(Ctx& c, LPCWSTR t, int l, int tp, int w, int id, int page, DWORD extra = 0)
{
    return MkWnd(c, L"EDIT", t, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | extra,
        WS_EX_CLIENTEDGE, l, tp, w, 12, id, page);
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

std::wstring HoursText(const TaskInfo& t)
{
    std::wstring s;
    for (int hv : t.hours) {
        if (!s.empty()) s += L",";
        s += std::to_wstring(hv);
    }
    return s.empty() ? L"-" : s + L" 点";
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
        std::wstring livev = L"-";
        for (auto& r : sn.credits.rows) {
            if (r.uid8 == a.uid8) { livev = r.ok ? FormatThousands(r.remain) : L"失败"; break; }
        }
        setcol(3, livev);
        std::wstring st;
        if (a.disabled) st = a.reason.empty() ? L"已禁用" : L"已禁用 " + a.reason;
        else if (a.cooling) st = a.until > NowSecX() ? L"冷却至 " + FormatTimeShort(a.until) : L"冷却中";
        else if (a.in_flight > 0) st = WideFormat(L"请求中(%d)", a.in_flight);
        else st = L"正常";
        setcol(4, st);
        setcol(5, a.token_expiry > NowSecX()
            ? WideFormat(L"%lld天", (a.token_expiry - NowSecX()) / 86400) : L"-");
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
        cool = WideFormat(L"上次 %s · 总剩 %s · 现在可查",
            FormatTimeShort(sn.credits.ts).c_str(), FormatThousands(sn.credits.total_remain).c_str());
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
            SetWindowTextW(c.task[i].hours, HoursText(*t).c_str());
            std::wstring nxt = t->enabled
                ? (t->next_fire.empty() ? std::wstring(L"-") : t->next_fire)
                : std::wstring(L"停用");
            SetWindowTextW(c.task[i].next, nxt.c_str());
            std::wstring status;
            if (t->running) status = L"执行中…";
            else if (t->last_run) status = L"上次 " + FormatTimeShort(t->last_run) + L" " + t->last_result;
            else status = L"本进程未执行过";
            SetWindowTextW(c.task[i].status, status.c_str());
        } else {
            SetWindowTextW(c.task[i].hours, L"-");
            SetWindowTextW(c.task[i].next, L"-");
            SetWindowTextW(c.task[i].status, L"-");
        }
        EnableWindow(c.task[i].chk, sn.admin_available && t && !busy);
        EnableWindow(c.task[i].btn, sn.admin_available && t && !busy && on);
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
        struct { LPCWSTR t; } tabs[] = { { L"服务" }, { L"账户与积分" }, { L"定时任务" }, { L"显示" }, { L"高级" } };
        for (int i = 0; i < 5; i++) {
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<LPWSTR>(tabs[i].t);
            TabCtrl_InsertItem(cp->tab, i, &ti);
        }

        // —— 页① 服务 ——
        cp->state_lbl = MkLabel(*cp, L"状态：未知", 8, 8, 404, 10, 0);
        cp->sub_lbl = MkLabel(*cp, L"", 8, 22, 404, 10, 0);
        MkLabel(*cp, L"服务目录:", 8, 44, 42, 10, 0);
        cp->dir_edt = MkEdit(*cp, cp->work.service_dir.c_str(), 52, 42, 300, IDC_EDT_DIR, 0);
        MkBtn(*cp, L"浏览…", 356, 42, 46, 14, IDC_BTN_BROWSE, 0);
        MkLabel(*cp, L"端口:", 8, 64, 42, 10, 0);
        cp->port_edt = MkEdit(*cp, std::to_wstring(cp->work.port).c_str(), 52, 62, 40, IDC_EDT_PORT, 0, ES_NUMBER);
        MkLabel(*cp, L"API Key:", 98, 64, 40, 10, 0);
        cp->key_edt = MkEdit(*cp, cp->work.api_key_manual.c_str(), 140, 62, 160, IDC_EDT_KEY, 0);
        MkLabel(*cp, L"轮询秒:", 306, 64, 40, 10, 0);
        cp->poll_edt = MkEdit(*cp, std::to_wstring(cp->work.poll_interval_sec).c_str(), 346, 62, 30, IDC_EDT_POLL, 0, ES_NUMBER);
        MkLabel(*cp, L"Key 留空 = 自动读服务目录 config.json；手填则覆盖。轮询 ≥10 秒（/status 免费，无风控压力）。", 8, 78, 404, 10, 0);
        cp->btn_start = MkBtn(*cp, L"启动服务", 8, 94, 60, 14, IDC_BTN_START, 0);
        cp->btn_stop = MkBtn(*cp, L"停止服务", 74, 94, 60, 14, IDC_BTN_STOP, 0);
        cp->btn_restart = MkBtn(*cp, L"重启服务", 140, 94, 60, 14, IDC_BTN_RESTART, 0);
        cp->chk_auto = MkCheck(*cp, L"开机自动启动（计划任务，登录+10秒延迟）", 8, 116, 250, IDC_CHK_AUTOSTART, 0);
        cp->chk_tm = MkCheck(*cp, L"随 TrafficMonitor 启动时拉起", 8, 130, 200, IDC_CHK_TM, 0);
        cp->chk_relaunch = MkCheck(*cp, L"意外停止自动拉起（10 分钟最多 3 次）", 8, 144, 250, IDC_CHK_RELAUNCH, 0);
        cp->chk_log = MkCheck(*cp, L"调试日志（写插件配置目录）", 8, 158, 200, IDC_CHK_LOG, 0);
        SendMessageW(cp->chk_auto, BM_SETCHECK, cp->work.autostart_task ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(cp->chk_tm, BM_SETCHECK, cp->work.start_with_tm ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(cp->chk_relaunch, BM_SETCHECK, cp->work.auto_relaunch ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(cp->chk_log, BM_SETCHECK, cp->work.logging ? BST_CHECKED : BST_UNCHECKED, 0);
        cp->err_lbl = MkLabel(*cp, L" ", 8, 176, 404, 10, 0);
        cp->action_lbl = MkLabel(*cp, L" ", 8, 190, 404, 10, 0);

        // —— 页② 账户与积分 ——
        cp->acc_list = MkWnd(*cp, WC_LISTVIEWW, L"",
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER, 0, 8, 8, 400, 140, IDC_LST_ACC, 1);
        ListView_SetExtendedListViewStyle(cp->acc_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        struct Col { LPCWSTR t; int w; };
        const Col cols[] = { { L"昵称", 88 }, { L"域", 34 }, { L"估算", 58 }, { L"实时", 62 }, { L"状态", 116 }, { L"令牌剩", 56 } };
        for (int i = 0; i < 6; i++) {
            LVCOLUMNW cv{};
            cv.mask = LVCF_TEXT | LVCF_WIDTH;
            cv.pszText = const_cast<LPWSTR>(cols[i].t);
            cv.cx = cols[i].w;
            ListView_InsertColumn(cp->acc_list, i, &cv);
        }
        cp->btn_cred = MkBtn(*cp, L"查询实时积分", 8, 156, 80, 14, IDC_BTN_CRED, 1);
        cp->lbl_cool = MkLabel(*cp, L"", 94, 158, 320, 10, 1);
        MkLabel(*cp, L"自动刷新周期(分钟,0=关,≥10):", 8, 178, 150, 10, 1);
        cp->citv_edt = MkEdit(*cp, std::to_wstring(cp->work.credits_refresh_interval_min).c_str(), 160, 176, 34, IDC_EDT_CINTERVAL, 1, ES_NUMBER);
        MkLabel(*cp, L"估算=本地账本，插件轮询零成本；实时=服务端逐号查上游余额（默认限频 10 分钟）。自动刷新默认关闭，开也要 ≥10 分钟，防风控。", 8, 194, 404, 20, 1);

        // —— 页③ 定时任务 ——
        cp->task_warn = MkLabel(*cp, L" ", 8, 8, 404, 10, 2);
        for (int i = 0; i < KIND_N; i++) {
            int y = 26 + i * 26;
            cp->task[i].chk = MkCheck(*cp, kKindZh[i], 8, y, 70, IDC_TASK_BASE + i * 10, 2);
            cp->task[i].hours = MkLabel(*cp, L"-", 82, y, 66, 10, 2);
            cp->task[i].next = MkLabel(*cp, L"-", 150, y, 44, 10, 2);
            cp->task[i].status = MkLabel(*cp, L"-", 196, y, 150, 10, 2);
            cp->task[i].btn = MkBtn(*cp, L"立即执行", 348, y - 2, 58, 14, IDC_TASK_BASE + i * 10 + 5, 2);
        }
        cp->btn_runall = MkBtn(*cp, L"全部执行", 8, 188, 60, 14, IDC_BTN_RUNALL, 2);
        cp->task_note = MkLabel(*cp, L"", 76, 190, 340, 10, 2);
        MkLabel(*cp, L"立即执行在服务进程内跑（与定时任务同一把锁，不会再有 task.exe 抢写状态文件的竞争）。", 8, 206, 404, 10, 2);

        // —— 页④ 显示 ——
        cp->rad[0] = MkWnd(*cp, L"BUTTON", L"状态 + 账号数（如 4/4）", BS_AUTORADIOBUTTON | WS_TABSTOP, 0, 8, 8, 220, 10, IDC_RAD_ACCOUNT, 3);
        cp->rad[1] = MkWnd(*cp, L"BUTTON", L"状态 + 积分（如 5.6k）", BS_AUTORADIOBUTTON | WS_TABSTOP, 0, 8, 24, 220, 10, IDC_RAD_CREDITS, 3);
        cp->rad[2] = MkWnd(*cp, L"BUTTON", L"仅状态词（运行/停止）", BS_AUTORADIOBUTTON | WS_TABSTOP, 0, 8, 40, 220, 10, IDC_RAD_ONLY, 3);
        cp->chk_live = MkCheck(*cp, L"积分优先显示实时值（有缓存时）", 8, 58, 240, IDC_CHK_LIVECRD, 3);
        MkLabel(*cp, L"状态点颜色：绿=运行且可用 · 橙=在跑无可用账号 · 灰=已停止 · 红=端口被占/无响应", 8, 76, 404, 10, 3);
        MkLabel(*cp, L"任务栏宽度按最长样例预留；单击任务栏上的本栏位即可打开此设置窗。", 8, 90, 404, 10, 3);
        if (cp->work.show_mode >= 0 && cp->work.show_mode <= 2)
            SendMessageW(cp->rad[cp->work.show_mode], BM_SETCHECK, BST_CHECKED, 0);
        else
            SendMessageW(cp->rad[0], BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(cp->chk_live, BM_SETCHECK, cp->work.show_live_credits ? BST_CHECKED : BST_UNCHECKED, 0);

        // —— 页⑤ 高级 ——
        MkLabel(*cp, L"管理接口轮询(秒,≥15):", 8, 10, 100, 10, 4);
        cp->admin_edt = MkEdit(*cp, std::to_wstring(cp->work.admin_poll_sec).c_str(), 110, 8, 34, IDC_EDT_ADMIN, 4, ES_NUMBER);
        cp->admin_stat = MkLabel(*cp, L"", 8, 28, 400, 10, 4);
        MkBtn(*cp, L"打开插件配置目录", 8, 46, 100, 14, IDC_BTN_OPENCFG, 4);
        MkBtn(*cp, L"打开服务目录", 112, 46, 80, 14, IDC_BTN_OPENSVC, 4);
        MkBtn(*cp, L"打开服务日志", 196, 46, 80, 14, IDC_BTN_OPENLOG, 4);
        MkBtn(*cp, L"打开插件日志", 280, 46, 80, 14, IDC_BTN_OPENPLOG, 4);
        MkLabel(*cp, L"WorkBuddy2API TrafficMonitor 插件 v1.0.0 · MIT", 8, 70, 404, 10, 4);
        MkLabel(*cp, L"https://github.com/Arimayuki03/workbuddy2api-trafficmonitor-plugin", 8, 84, 404, 10, 4);
        MkLabel(*cp, L"设计约束：/healthz /status /admin 均为本机回环接口；本插件永不调用 /v1/chat/completions，", 8, 104, 404, 10, 4);
        MkLabel(*cp, L"与你的 API 使用互不影响。实时积分查询由服务端冷却与单飞兜底，防止任何路径触发上游风控。", 8, 118, 404, 10, 4);

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
        return FALSE;
    }
    case WM_COMMAND: {
        if (!cp) return FALSE;
        int id = LOWORD(wp), code = HIWORD(wp);
        Ctx& c = *cp;
        switch (id) {
        case IDOK: {
            c.work.service_dir = TrimW(GetText(c.dir_edt));
            c.work.port = GetInt(c.port_edt, 7863);
            c.work.api_key_manual = TrimW(GetText(c.key_edt));
            c.work.poll_interval_sec = GetInt(c.poll_edt, 30);
            c.work.admin_poll_sec = GetInt(c.admin_edt, 60);
            c.work.credits_refresh_interval_min = GetInt(c.citv_edt, 0);
            c.work.autostart_task = SendMessageW(c.chk_auto, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.work.start_with_tm = SendMessageW(c.chk_tm, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.work.auto_relaunch = SendMessageW(c.chk_relaunch, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.work.logging = SendMessageW(c.chk_log, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.work.show_live_credits = SendMessageW(c.chk_live, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.work.show_mode = SendMessageW(c.rad[0], BM_GETCHECK, 0, 0) == BST_CHECKED ? SM_STATE_ACCOUNT
                : (SendMessageW(c.rad[1], BM_GETCHECK, 0, 0) == BST_CHECKED ? SM_STATE_CREDITS : SM_STATE_ONLY);
            std::wstring bad;
            if (c.work.service_dir.empty()) bad = L"服务目录不能为空";
            else if (c.work.port <= 0 || c.work.port > 65535) bad = L"端口非法";
            else if (c.work.poll_interval_sec < 10 || c.work.poll_interval_sec > 600) bad = L"轮询间隔需 10–600 秒";
            else if (c.work.admin_poll_sec < 15 || c.work.admin_poll_sec > 600) bad = L"管理轮询需 15–600 秒";
            else if (c.work.credits_refresh_interval_min != 0 && c.work.credits_refresh_interval_min < 10)
                bad = L"自动刷新积分需 0(关) 或 ≥10 分钟";
            if (!bad.empty()) {
                MessageBoxW(hDlg, bad.c_str(), L"设置未保存", MB_OK | MB_ICONWARNING);
                return TRUE;
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
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        HWND h = reinterpret_cast<HWND>(lp);
        SetBkMode(dc, TRANSPARENT);
        if (cp && (h == cp->state_lbl || h == cp->err_lbl))
            SetTextColor(dc, cp->state_color);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_CLOSE:
        if (cp) { KillTimer(hDlg, 7); EndDialog(hDlg, IDCANCEL); }
        return TRUE;
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
