// worker.cpp — 轮询状态机 + 动作队列实现。判定原则（都对应真实踩过的坑）：
//  * 503 且有服务身份 = "活着但没可用账号"，绝不判停止（wb2api 语义）；
//  * 超时/无响应 ≠ 停止（保持旧态，连续 5 次才降级 Dead）；只有"连接被拒"类直接判停止；
//  * 身份校验双保险：X-Service 头 或 body.service —— 二者都不是 workbuddy2api → WrongService；
//  * 判"停止"前顺手查端口归属：被外来进程占用要显示"被占用"而不是"已停止"。
#include "worker.h"
#include "common.h"
#include "http.h"
#include "logger.h"
#include "procctl.h"
#include "settings.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <ctime>
#include <map>

using json = nlohmann::json;

namespace wb2 {
namespace {

int64_t JInt(const json& j, const char* k, int64_t d = 0)
{
    auto it = j.find(k);
    if (it == j.end() || !it->is_number_integer()) return d;
    return it->get<int64_t>();
}
bool JBool(const json& j, const char* k, bool d = false)
{
    auto it = j.find(k);
    if (it == j.end() || !it->is_boolean()) return d;
    return it->get<bool>();
}
std::string JStr(const json& j, const char* k)
{
    auto it = j.find(k);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

// RFC3339 → unix 秒；"0001-01-01..." 零值与空串一律 0（=无）。
int64_t RfcToUnix(const std::string& s)
{
    if (s.empty() || s.size() < 19) return 0;
    int Y, M, D, h, mi, se;
    if (sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &mi, &se) != 6)
        return 0;
    if (Y < 1902) return 0; // Go 零值时间
    std::tm tm{};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
    time_t t = _mkgmtime(&tm);
    // 偏移
    size_t pos = 19;
    while (pos < s.size() && (isdigit((unsigned char)s[pos]) || s[pos] == '.')) pos++;
    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
        int oh = 0, om = 0;
        sscanf(s.c_str() + pos, "%3d:%2d", &oh, &om);
        int total = oh * 3600 + (oh < 0 ? -om : om) * 60;
        t -= total;
    }
    return t <= 0 ? 0 : static_cast<int64_t>(t);
}

int64_t NowSec() { return static_cast<int64_t>(time(nullptr)); }

std::wstring KindLabel(const std::string& kind)
{
    static const std::map<std::string, std::wstring> m = {
        { "checkin", L"签到" }, { "travel", L"猫猫旅行" }, { "activity", L"活跃上报" },
        { "keepalive", L"Token 保活" }, { "school", L"开学季" }, { "cat", L"夜猫子" },
    };
    auto it = m.find(kind);
    return it == m.end() ? Utf8ToWide(kind) : it->second;
}

// 解析 /admin/credits 的 accounts+total（GET 回放缓存与 POST 实时查询两处共用）。
// 服务端契约：creditAccount{uid,nickname,remain,used,size,ok,error}（指针=查询失败缺省）、
// creditTotal{remain,used,size,accounts,ok,failed}，见 wb2api internal/server/admin.go。
static void ParseCreditsJson(const json& cj, CreditsInfo& ci)
{
    ci.have = JBool(cj, "cached");
    ci.ts = JInt(cj, "ts");
    ci.cooldown_until = JInt(cj, "cooldown_until");
    auto geti = [](const json& o, const char* k) -> int64_t {
        auto it = o.find(k);
        return (it != o.end() && it->is_number_integer()) ? it->get<int64_t>() : -1;
    };
    if (cj.contains("total") && cj["total"].is_object()) {
        const json& t = cj["total"];
        ci.total_remain = geti(t, "remain");
        ci.total_used = geti(t, "used");
    }
    if (cj.contains("accounts") && cj["accounts"].is_array()) {
        for (auto& a : cj["accounts"]) {
            if (!a.is_object()) continue;
            CreditRow row;
            row.uid8 = Utf8ToWide(JStr(a, "uid").substr(0, 8));
            row.nickname = Utf8ToWide(JStr(a, "nickname"));
            row.ok = JBool(a, "ok");
            row.remain = geti(a, "remain");
            row.used = geti(a, "used");
            row.size = geti(a, "size");
            row.error = Utf8ToWide(JStr(a, "error"));
            ci.rows.push_back(std::move(row));
        }
    }
}

} // namespace

Worker& Worker::Instance()
{
    static Worker inst;
    return inst;
}

void Worker::Start()
{
    bool expect = false;
    if (!started_.compare_exchange_strong(expect, true)) return;
    stop_ = false;
    th_ = std::thread([this] { RunLoop(); });
}

void Worker::Stop()
{
    if (!started_.load()) return;
    stop_ = true;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    started_ = false;
}

Snapshot Worker::Copy() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

SvcState Worker::State() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return snap_.state;
}

std::wstring Worker::DisplayValue() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return value_w_;
}

std::wstring Worker::TooltipText() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return tooltip_w_;
}

void Worker::RefreshSoon()
{
    refresh_flag_ = true;
    cv_.notify_all();
}

void Worker::Update(std::function<void(Snapshot&)> fn)
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn(snap_);
        BuildDisplayLocked();
    }
}

bool Worker::BeginAct(const std::string& key)
{
    std::lock_guard<std::mutex> lk(act_mu_);
    return busy_.insert(key).second;
}
void Worker::EndAct(const std::string& key)
{
    std::lock_guard<std::mutex> lk(act_mu_);
    busy_.erase(key);
}
bool Worker::IsActionBusy(const std::string& key) const
{
    std::lock_guard<std::mutex> lk(act_mu_);
    return busy_.count(key) > 0;
}

void Worker::NoteLocked(Snapshot& s, const std::wstring& note)
{
    s.action_note = note;
    s.action_note_ts = (int64_t)time(nullptr);
}

// ============================================================================
// 轮询主循环
// ============================================================================

void Worker::RunLoop()
{
    LogI(L"worker 线程启动");
    while (!stop_.load()) {
        ULONGLONG now = GetTickCount64();
        Settings s = SettingsStore::Instance().Get();
        if (now >= next_base_ || refresh_flag_.exchange(false)) {
            PollOnce();
            next_base_ = GetTickCount64() + 1000ULL * s.poll_interval_sec;
        }
        if (GetTickCount64() >= next_admin_) {
            PollAdmin();
            next_admin_ = GetTickCount64() + 1000ULL * s.admin_poll_sec;
        }
        if (GetTickCount64() >= next_auths_) {
            ScanAuthExpiry();
            next_auths_ = GetTickCount64() + 300000ULL; // 5 分钟一轮，纯本地
        }
        std::unique_lock<std::mutex> lk(cv_mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(500),
            [this] { return stop_.load() || refresh_flag_.load(); });
    }
    LogI(L"worker 线程退出");
}

void Worker::PollOnce()
{
    Settings s = SettingsStore::Instance().Get();
    std::wstring base = WideFormat(L"http://127.0.0.1:%d", s.port);
    HttpResponse hr = HttpJson(L"GET", base + L"/healthz", "", "", 2500);

    Snapshot patch;
    bool id_ok = false;
    if (hr.ok()) {
        json body;
        try { body = json::parse(hr.body); } catch (...) {}
        id_ok = (hr.x_service == L"workbuddy2api") || (JStr(body, "service") == "workbuddy2api");
        if (id_ok) {
            patch.total = (int)JInt(body, "total");
            patch.healthy = (int)JInt(body, "healthy");
            if (body.contains("realm_servable") && body["realm_servable"].is_object()) {
                patch.servable_cn = JBool(body["realm_servable"], "cn");
                patch.servable_global = JBool(body["realm_servable"], "global");
            }
            patch.state = hr.status == 200 ? SvcState::Running : SvcState::Unservable;
            consecutive_soft_fail_ = 0;
        } else {
            patch.state = SvcState::WrongService;
            patch.last_error = L"该端口有 HTTP 服务应答，但不是 workbuddy2api（可能装错目录/端口冲突）";
        }
    } else {
        // 传输层失败 → 归类
        if (HttpErrIsUnreachable(hr.err)) {
            proc::Listener l = proc::FindPortListener(s.port);
            if (!l.found) {
                patch.state = SvcState::Stopped;
                consecutive_soft_fail_ = 0;
            } else if (!l.is_our_service) {
                patch.state = SvcState::WrongService;
                patch.pid = l.pid;
                patch.last_error = WideFormat(L"端口 %d 被其他程序占用：PID %lu %s",
                    s.port, l.pid, l.exe_path.c_str());
            } else {
                // 自家进程占着端口却拒绝 HTTP —— 半死状态
                patch.state = SvcState::Dead;
                patch.pid = l.pid;
                consecutive_soft_fail_++;
            }
        } else {
            // 超时/其他：防抖。连续 5 次才降级。
            consecutive_soft_fail_++;
            patch.state = SvcState::Unknown; // 占位，Update 里保持旧态
            if (consecutive_soft_fail_ >= 5) patch.state = SvcState::Dead;
        }
        if (hr.err.size()) patch.last_error = hr.err;
    }

    std::string key = SettingsStore::Instance().CurrentApiKey();
    if (id_ok) {
        HttpResponse sr = HttpJson(L"GET", base + L"/status", key, "", 4000);
        if (sr.status == 401) {
            patch.need_key_note = true;
            if (patch.last_error.empty())
                patch.last_error = L"/status 401：api_key 未配置或不符（在设置页填写，或放好服务目录 config.json）";
        } else if (sr.status == 200) {
            patch.need_key_note = false;
            json sj;
            try { sj = json::parse(sr.body); } catch (...) {}
            if (sj.is_object()) {
                patch.cooling = (int)JInt(sj, "cooling");
                patch.disabled_n = (int)JInt(sj, "disabled");
                patch.sticky = (int)JInt(sj, "sticky_sessions");
                if (sj.contains("accounts") && sj["accounts"].is_array()) {
                    for (auto& a : sj["accounts"]) {
                        if (!a.is_object()) continue;
                        AccountInfo ai;
                        std::string uid = JStr(a, "uid");
                        ai.uid8 = Utf8ToWide(uid.substr(0, 8));
                        ai.nickname = Utf8ToWide(JStr(a, "nickname"));
                        if (ai.nickname.empty()) ai.nickname = ai.uid8;
                        ai.realm = Utf8ToWide(JStr(a, "realm"));
                        ai.credits = JInt(a, "credits");
                        ai.cooling = JBool(a, "cooling");
                        ai.disabled = JBool(a, "disabled");
                        ai.until = RfcToUnix(JStr(a, "until"));
                        ai.reason = Utf8ToWide(JStr(a, "reason"));
                        ai.in_flight = (int)JInt(a, "in_flight");
                        ai.uid = uid;
                        patch.accounts.push_back(std::move(ai));
                    }
                    patch.accounts_valid = true;
                }
            }
        }
    }

    Update([&](Snapshot& sn) {
        if (patch.state != SvcState::Unknown) sn.state = patch.state;
        sn.last_error = patch.last_error;
        sn.pid = patch.pid;
        sn.need_key_note = patch.need_key_note;
        if (id_ok) {
            sn.total = patch.total; sn.healthy = patch.healthy;
            sn.servable_cn = patch.servable_cn; sn.servable_global = patch.servable_global;
            sn.cooling = patch.cooling; sn.disabled_n = patch.disabled_n; sn.sticky = patch.sticky;
            if (patch.accounts_valid) { sn.accounts = std::move(patch.accounts); sn.accounts_valid = true; }
            sn.last_ok_ts = (int64_t)time(nullptr);
        }
    });

    // —— 异常自动拉起 ——
    Snapshot sn = Copy();
    s = SettingsStore::Instance().Get();
    if (sn.state == SvcState::Stopped && s.auto_relaunch && !s.user_stopped) {
        ULONGLONG now = GetTickCount64();
        while (!relaunch_ts_.empty() && now - relaunch_ts_.front() > 600000ULL) relaunch_ts_.pop_front();
        if (relaunch_ts_.size() < 3) {
            relaunch_ts_.push_back(now);
            LogW(L"auto-relaunch: 检测到服务停止且非用户主动，第 " +
                std::to_wstring(relaunch_ts_.size()) + L" 次拉起");
            RequestStartService();
        }
    } else if (StateIsOn(sn.state)) {
        relaunch_ts_.clear();
    }

    // —— 实时积分自动刷新（默认关；开着则严格按服务端冷却节奏） ——
    if (s.credits_refresh_interval_min > 0 && sn.admin_available && NowSec() >= (int64_t)next_auto_credit_ / 1000) {
        if (sn.credits.cooldown_until <= NowSec() && !IsActionBusy("credits")) {
            RequestRefreshCredits();
        }
        next_auto_credit_ = (ULONGLONG)(NowSec() + s.credits_refresh_interval_min * 60) * 1000;
    }
}

void Worker::PollAdmin()
{
    Settings s = SettingsStore::Instance().Get();
    if (!StateIsOn(Copy().state)) return; // 服务不在就别白发请求
    std::string key = SettingsStore::Instance().CurrentApiKey();
    std::wstring base = WideFormat(L"http://127.0.0.1:%d", s.port);

    HttpResponse tr = HttpJson(L"GET", base + L"/admin/tasks", key, "", 3000);
    if (tr.status == 404) {
        Update([](Snapshot& sn) { sn.admin_available = false; });
        return;
    }
    if (tr.status != 200) return; // 401/超时：维持上次结论
    json tj;
    try { tj = json::parse(tr.body); } catch (...) { return; }
    if (!tj.is_object() || !tj.contains("tasks") || !tj["tasks"].is_array()) return;
    std::vector<TaskInfo> tasks;
    for (auto& t : tj["tasks"]) {
        if (!t.is_object()) continue;
        TaskInfo ti;
        ti.kind = JStr(t, "kind");
        ti.label = Utf8ToWide(JStr(t, "label"));
        if (ti.label.empty()) ti.label = KindLabel(ti.kind);
        ti.enabled = JBool(t, "enabled", true);
        if (t.contains("hours") && t["hours"].is_array())
            for (auto& hv : t["hours"])
                if (hv.is_number_integer()) ti.hours.push_back(hv.get<int>());
        ti.next_fire = FormatRfc3339Short(JStr(t, "next_fire"));  // RFC3339 → 本地 HH:mm
        ti.running = JBool(t, "running");
        ti.last_run = JInt(t, "last_run_unix");
        ti.last_result = Utf8ToWide(JStr(t, "last_result"));
        tasks.push_back(std::move(ti));
    }
    bool admin_ok = true;
    Update([&](Snapshot& sn) {
        sn.admin_available = admin_ok;
        sn.tasks = std::move(tasks);
        sn.tasks_ts = (int64_t)time(nullptr);
    });

    HttpResponse cr = HttpJson(L"GET", base + L"/admin/credits", key, "", 3000);
    if (cr.status != 200) return;
    json cj;
    try { cj = json::parse(cr.body); } catch (...) { return; }
    CreditsInfo ci;
    ParseCreditsJson(cj, ci);
    Update([&](Snapshot& sn) { sn.credits = std::move(ci); });
}

// ============================================================================
// 本地 auths 扫描（token 到期；纯文件读，零上游）
// ============================================================================

void Worker::ScanAuthExpiry()
{
    Settings s = SettingsStore::Instance().Get();
    if (s.service_dir.empty()) return;
    std::wstring pattern = s.service_dir + L"\\auths\\workbuddy*.json";
    WIN32_FIND_DATAW find{};
    HANDLE hf = FindFirstFileW(pattern.c_str(), &find);
    if (hf == INVALID_HANDLE_VALUE) return;
    std::map<std::string, int64_t> out;
    do {
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring full = s.service_dir + L"\\auths\\" + find.cFileName;
        std::string raw;
        HANDLE h = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) raw.append(buf, got);
        CloseHandle(h);
        json j;
        try { j = json::parse(raw); } catch (...) { continue; }
        std::string uid;
        if (j.contains("account") && j["account"].is_object()) uid = JStr(j["account"], "uid");
        if (uid.empty()) continue;
        int64_t exp = 0;
        if (j.contains("auth") && j["auth"].is_object()) exp = JInt(j["auth"], "expiresAt");
        if (exp > 0) out[uid] = exp;
    } while (FindNextFileW(hf, &find));
    FindClose(hf);

    Update([&](Snapshot& sn) {
        token_expiry_.swap(out);
        for (auto& a : sn.accounts) {
            auto it = token_expiry_.find(a.uid);
            a.token_expiry = it == token_expiry_.end() ? 0 : it->second;
        }
    });
}

// ============================================================================
// 预生成显示文本（任务栏值 + tooltip）——锁内构建，UI 侧永远只拿现成字符串
// ============================================================================

namespace {

// 积分合计：优先服务端实时缓存（show_live_credits 开且有值），否则 /status 估算和。
int64_t CreditsFor(const Snapshot& sn, const Settings& s, bool* live)
{
    if (live) *live = false;
    if (s.show_live_credits && sn.credits.have) {
        if (live) *live = true;
        return sn.credits.total_remain;
    }
    if (!sn.accounts_valid) return -1;
    int64_t sum = 0;
    for (auto& a : sn.accounts) sum += a.credits;
    return sum;
}

} // namespace

void Worker::BuildDisplayLocked()
{
    Settings st = SettingsStore::Instance().Get();
    const Snapshot& sn = snap_;
    auto& v = value_w_;

    switch (sn.state) {
    case SvcState::Running:
    case SvcState::Unservable:
    case SvcState::Starting: {
        switch (st.show_mode) {
        case SM_STATE_CREDITS: {
            int64_t c = CreditsFor(sn, st, nullptr);
            v = c >= 0 ? FormatCreditsCompact(c) : L"-";
            break;
        }
        case SM_STATE_ONLY:
            v = sn.state == SvcState::Starting ? L"启动" : L"运行";
            break;
        default:
            v = WideFormat(L"%d/%d", sn.healthy, sn.total);
        }
        break;
    }
    case SvcState::Stopped:
        v = st.show_mode == SM_STATE_ONLY ? L"停止" : L"-";
        break;
    case SvcState::WrongService:
        v = L"占用";
        break;
    case SvcState::Dead:
        v = L"无响应";
        break;
    default:
        v = L"…";
    }

    // —— tooltip ——
    std::vector<std::wstring> lines;
    const wchar_t* state_txt = L"未知";
    switch (sn.state) {
    case SvcState::Running: state_txt = L"运行中"; break;
    case SvcState::Unservable: state_txt = L"运行中（暂无可用账号）"; break;
    case SvcState::Starting: state_txt = L"启动中…"; break;
    case SvcState::Stopped: state_txt = L"已停止"; break;
    case SvcState::WrongService: state_txt = L"端口被其他程序占用"; break;
    case SvcState::Dead: state_txt = L"进程无响应"; break;
    }
    std::wstring head = WideFormat(L"WorkBuddy2API：%s", state_txt);
    if (sn.pid) head += WideFormat(L"（PID %lu）", sn.pid);
    lines.push_back(head);
    lines.push_back(WideFormat(L"地址：http://127.0.0.1:%d", st.port));
    if (StateIsOn(sn.state))
        lines.push_back(WideFormat(L"健康 %d/%d · 冷却 %d · 禁用 %d · 粘性会话 %d",
            sn.healthy, sn.total, sn.cooling, sn.disabled_n, sn.sticky));
    if (!sn.last_error.empty()) lines.push_back(L"提示：" + sn.last_error);

    if (StateIsOn(sn.state) && sn.accounts_valid && !sn.accounts.empty()) {
        lines.push_back(L"—— 账户（估算）——");
        int shown = 0;
        for (auto& a : sn.accounts) {
            if (shown++ >= 8) { lines.push_back(L"…"); break; }
            std::wstring note;
            if (a.disabled) note = L"已禁用";
            else if (a.cooling) note = L"冷却中";
            else if (a.in_flight > 0) note = L"请求中";
            else note = L"正常";
            std::wstring live;
            if (sn.credits.have) {
                for (auto& c : sn.credits.rows) {
                    if (c.uid8 == a.uid8) {
                        if (!c.ok) live = L" 实时:失败";
                        else if (c.used >= 0 && c.size >= 0)
                            live = WideFormat(L" 实时:%lld(已用%lld)", c.remain, c.used);
                        else live = WideFormat(L" 实时:%lld", c.remain);
                        break;
                    }
                }
            }
            std::wstring tk;
            if (a.token_expiry > NowSec())
                tk = WideFormat(L" 令牌剩%lld天", (a.token_expiry - NowSec()) / 86400);
            lines.push_back(WideFormat(L"  %s (%s) %s分 %s%s%s", a.nickname.c_str(),
                a.realm.empty() ? L"cn" : a.realm.c_str(),
                FormatThousands(a.credits).c_str(), note.c_str(), live.c_str(), tk.c_str()));
        }
    }

    if (StateIsOn(sn.state)) {
        if (sn.admin_available && !sn.tasks.empty()) {
            lines.push_back(L"—— 定时任务 ——");
            for (auto& t : sn.tasks) {
                std::wstring mark = t.running ? L"执行中" : (t.enabled ? L"启用" : L"停用");
                std::wstring nxt = (t.running || !t.enabled || t.next_fire.empty())
                    ? L"" : WideFormat(L" 下次%s", t.next_fire.c_str());
                lines.push_back(L"  " + t.label + L"[" + mark + nxt + L"]");
            }
        } else if (!sn.admin_available) {
            lines.push_back(L"任务管理：需开启 wb2api 的 admin.enabled（README）");
        }
        if (sn.credits.have) {
            std::wstring usedtxt = sn.credits.total_used >= 0
                ? WideFormat(L" · 已用 %s", FormatThousands(sn.credits.total_used).c_str()) : L"";
            lines.push_back(WideFormat(L"实时积分：总剩 %s%s（%s 查询；下次可查 %s）",
                FormatThousands(sn.credits.total_remain).c_str(), usedtxt.c_str(),
                FormatTimeShort(sn.credits.ts).c_str(),
                FormatTimeShort(sn.credits.cooldown_until).c_str()));
        }
    }

    if (!sn.action_note.empty() && NowSec() - sn.action_note_ts < 25)
        lines.push_back(L"最近操作：" + sn.action_note);
    lines.push_back(L"单击此栏位打开设置");

    // 拼接为 tooltip。注意：
    //  * 换行必须用 "\r\n"——宿主把它直接喂给 MFC CToolTipCtrl，裸 \n 在部分宿主路径上渲染异常；
    //  * TM 会把所有插件的 tooltip 拼成一条再喂给 MFC CToolTipCtrl::UpdateTipText，后者对
    //    超过 1024 字符的文本抛 CInvalidArgException（宿主弹"遇到不适当的参数。"）。
    //    TM 自身文本 ~300 字符，其余插件（MijiaPower 等）占用不可控；完整 tooltip 实测 351 字符时
    //    三插件同载触发弹框。默认完整展开（tooltip_full），同载弹框时用户可在设置里切到折叠配额：
    //    每行 64 字符 / 4 行 / 总长 150 字符兜底。
    if (st.tooltip_full) {
        tooltip_w_.clear();
        for (size_t i = 0; i < lines.size(); i++) {
            if (i) tooltip_w_ += L"\r\n";
            tooltip_w_ += lines[i];
        }
        return;
    }
    auto ClipLine = [](std::wstring& s, size_t maxw) {
        if (s.size() > maxw) s.assign(s, 0, maxw - 1).append(L"…");
    };
    for (auto& l : lines) ClipLine(l, 64);
    // 行数上限 4：状态头、地址、账户/任务摘要、操作提示（超出折叠）
    if (lines.size() > 4) {
        std::wstring head = lines[0];
        std::wstring mid = lines[2];
        std::wstring tail = lines.back();
        lines.clear();
        lines.push_back(head);
        lines.push_back(mid);
        lines.push_back(L"…（详情已折叠）");
        lines.push_back(tail);
    }
    tooltip_w_.clear();
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) tooltip_w_ += L"\r\n";
        tooltip_w_ += lines[i];
    }
    // 终极兜底：总长超 150 就只保留状态行 + 提示行
    if (tooltip_w_.size() > 150) {
        std::wstring head = lines.empty() ? L"" : lines.front();
        tooltip_w_ = head + L"\r\n…\r\n单击此栏位打开设置";
    }
}

// ============================================================================
// 动作（全部异步线程；同类互斥靠 busy_ 集合；结果写进 action_note 供 UI/tooltip 显示）
// ============================================================================

// SetTaskHours 落盘实现（动作线程内调用）：把服务 config.json 里 schedule.<key> 的
// 整数数组替换为 hours。服务端没有改 hours 的接口（且 hours 热改需重启进程），
// 所以由插件直写配置文件：解析后整体重dump（未知字段原值保留，键序会按字典序重排、
// 统一 2 空格缩进），改前备份 .bak，tmp+rename 原子替换；内容未变则不落盘不提示。
static std::wstring SetTaskHours(const std::wstring& config_path, const std::string& key,
    const std::vector<int>& hours)
{
    using json = nlohmann::json;
    HANDLE h = CreateFileW(config_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"读取 config.json 失败（路径不对或无权限）";
    std::string raw;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) raw.append(buf, got);
    CloseHandle(h);

    json root;
    try { root = json::parse(raw); } catch (...) { return L"config.json 不是合法 JSON，拒绝写回"; }
    if (!root.is_object()) return L"config.json 顶层不是对象，拒绝写回";
    if (!root.contains("schedule") || !root["schedule"].is_object())
        root["schedule"] = json::object();
    json arr = json::array();
    for (int hv : hours) arr.push_back(hv);
    root["schedule"][key] = arr;

    std::string new_raw = root.dump(2) + "\n";
    if (new_raw == raw) return L""; // 内容未变：不落盘、不提示

    // .bak 备份（覆盖式，与 /admin/tasks 的 patchConfigBool 行为一致）
    HANDLE b = CreateFileW((config_path + L".bak").c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (b != INVALID_HANDLE_VALUE) {
        WriteFile(b, raw.data(), (DWORD)raw.size(), &got, nullptr);
        CloseHandle(b);
    } else return L"备份 config.json.bak 失败，已放弃写入";

    HANDLE w = CreateFileW((config_path + L".tmp").c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) return L"创建临时文件失败";
    bool ok = WriteFile(w, new_raw.data(), (DWORD)new_raw.size(), &got, nullptr) &&
        got == new_raw.size();
    CloseHandle(w);
    if (!ok || !MoveFileExW((config_path + L".tmp").c_str(), config_path.c_str(),
        MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW((config_path + L".tmp").c_str());
        return L"写入 config.json 失败（文件被占用？）";
    }
    return L"";
}

bool Worker::RequestSetTaskHours(const std::string& kind, const std::vector<int>& hours)
{
    std::string key = "taskhours:" + kind;
    if (!BeginAct(key)) return false;
    std::thread([this, kind, hours, key] {
        Settings s = SettingsStore::Instance().Get();
        std::wstring note;
        if (s.service_dir.empty()) {
            note = L"未配置服务目录，无法写回 config.json";
        } else {
            std::wstring cfg = s.service_dir + L"\\config.json";
            // 小时快照以"服务端下发的当前值"为准做合法性检查（0-23）
            std::wstring err = SetTaskHours(cfg, kind + "_hours", hours);
            if (err.empty()) {
                std::wstring hv;
                for (int hvv : hours) {
                    if (!hv.empty()) hv += L",";
                    hv += std::to_wstring(hvv);
                }
                note = WideFormat(L"%s 触发时间已写回 config.json（%s 点），重启服务后生效",
                    KindLabel(kind).c_str(), hv.c_str());
            } else note = KindLabel(kind) + L"：" + err;
        }
        Update([&](Snapshot& sn) { NoteLocked(sn, note); });
        EndAct(key);
        RefreshSoon();
    }).detach();
    return true;
}

void Worker::SetUserStopped(bool v)
{
    Settings s = SettingsStore::Instance().Get();
    if (s.user_stopped != v) {
        s.user_stopped = v;
        SettingsStore::Instance().Update(s);
    }
}

bool Worker::RequestStartService()
{
    if (!BeginAct("svc")) return false;
    std::thread([this] {
        SetUserStopped(false);
        Update([](Snapshot& sn) {
            sn.state = SvcState::Starting;
            NoteLocked(sn, L"正在启动服务…");
        });
        std::wstring err;
        bool ok = proc::StartService(err);
        Update([&](Snapshot& sn) {
            NoteLocked(sn, ok ? L"服务已启动" : L"启动失败：" + err);
        });
        if (!ok) LogE(L"start failed: " + err);
        EndAct("svc");
        RefreshSoon();
    }).detach();
    return true;
}

bool Worker::RequestStopService()
{
    if (!BeginAct("svc")) return false;
    std::thread([this] {
        SetUserStopped(true); // 抑制自动拉起（持久化：跨 TM 重启也记住"是用户主动停的"）
        std::wstring err;
        bool ok = proc::StopService(err);
        Update([&](Snapshot& sn) {
            NoteLocked(sn, ok ? L"服务已停止" : L"停止失败：" + err);
            if (ok) { sn.state = SvcState::Stopped; sn.pid = 0; }
        });
        EndAct("svc");
        RefreshSoon();
    }).detach();
    return true;
}

bool Worker::RequestRestartService()
{
    if (!BeginAct("svc")) return false;
    std::thread([this] {
        SetUserStopped(false);
        std::wstring err;
        if (!proc::StopService(err)) {
            Update([&](Snapshot& sn) { NoteLocked(sn, L"重启失败（停止步骤）：" + err); });
            EndAct("svc");
            RefreshSoon();
            return;
        }
        Update([](Snapshot& sn) {
            sn.state = SvcState::Starting;
            NoteLocked(sn, L"正在重启服务…");
        });
        bool ok = proc::StartService(err);
        Update([&](Snapshot& sn) {
            NoteLocked(sn, ok ? L"服务已重启" : L"重启失败：" + err);
        });
        EndAct("svc");
        RefreshSoon();
    }).detach();
    return true;
}

bool Worker::RequestRunTask(const std::string& kind)
{
    std::string key = "task:" + kind;
    if (!BeginAct(key)) return false;
    std::thread([this, kind, key] {
        Settings s = SettingsStore::Instance().Get();
        std::string bearer = SettingsStore::Instance().CurrentApiKey();
        std::wstring url = WideFormat(L"http://127.0.0.1:%d/admin/tasks/run", s.port);
        json body{ { "kind", kind } };
        HttpResponse r = HttpJson(L"POST", url, bearer, body.dump(), 8000);
        std::wstring note;
        if (r.status == 202) {
            json j; try { j = json::parse(r.body); } catch (...) {}
            std::wstring names;
            if (j.contains("started") && j["started"].is_array()) {
                for (auto& x : j["started"]) {
                    if (!x.is_string()) continue;
                    if (!names.empty()) names += L"、";
                    names += KindLabel(x.get<std::string>());
                }
            }
            if (names.empty()) names = KindLabel(kind);
            note = L"已触发：" + names + L"（执行进度稍后自动刷新）";
        } else if (r.status == 409) {
            note = L"该类任务正在执行中，等它跑完再触发";
        } else if (r.status == 404) {
            note = L"/admin 未启用：升级 wb2api 并在 config.json 设 admin.enabled=true 后重启服务";
        } else if (r.status == 401) {
            note = L"api_key 不符（401），检查设置页或服务 config.json";
        } else {
            note = r.err.empty() ? WideFormat(L"触发失败（HTTP %lu）", r.status) : r.err;
        }
        Update([&](Snapshot& sn) { NoteLocked(sn, note); });
        EndAct(key);
        RefreshSoon();
    }).detach();
    return true;
}

bool Worker::RequestToggleTask(const std::string& kind, bool enabled)
{
    std::string key = "task:" + kind;
    if (!BeginAct(key)) return false;
    std::thread([this, kind, enabled, key] {
        Settings s = SettingsStore::Instance().Get();
        std::string bearer = SettingsStore::Instance().CurrentApiKey();
        std::wstring url = WideFormat(L"http://127.0.0.1:%d/admin/tasks", s.port);
        json body{ { "kind", kind }, { "enabled", enabled } };
        HttpResponse r = HttpJson(L"PATCH", url, bearer, body.dump(), 4000);
        std::wstring note;
        bool mark = enabled;
        if (r.status == 200) {
            json j; try { j = json::parse(r.body); } catch (...) {}
            bool persisted = JBool(j, "persisted");
            note = KindLabel(kind) + (enabled ? L"已启用" : L"已停用");
            if (!persisted) note += L"（未持久化：" + Utf8ToWide(JStr(j, "note")) + L"）";
        } else {
            mark = !enabled; // 失败回滚本地勾选
            if (r.status == 404) note = L"/admin 未启用，无法改任务开关";
            else if (r.status == 401) note = L"api_key 不符（401）";
            else note = r.err.empty() ? WideFormat(L"设置失败（HTTP %lu）", r.status) : r.err;
        }
        Update([&](Snapshot& sn) {
            for (auto& t : sn.tasks) {
                if (t.kind == kind) {
                    t.enabled = mark;
                    if (!mark) t.next_fire.clear();
                }
            }
            NoteLocked(sn, note);
        });
        EndAct(key);
        RefreshSoon();
    }).detach();
    return true;
}

bool Worker::RequestRefreshCredits()
{
    if (!BeginAct("credits")) return false;
    std::thread([this] {
        Settings s = SettingsStore::Instance().Get();
        std::string bearer = SettingsStore::Instance().CurrentApiKey();
        std::wstring url = WideFormat(L"http://127.0.0.1:%d/admin/credits", s.port);
        // 每号一次上游查询（服务端已限速+冷却）；本地网络到 loopback，放宽到 120s 纯防卡死。
        HttpResponse r = HttpJson(L"POST", url, bearer, "{}", 120000);
        std::wstring note;
        if (r.status == 200) {
            json j;
            try { j = json::parse(r.body); } catch (...) {}
            CreditsInfo ci;
            ParseCreditsJson(j, ci);
            Update([&](Snapshot& sn) {
                sn.credits = std::move(ci);
                NoteLocked(sn, WideFormat(L"实时积分已刷新（总剩 %s）",
                    FormatThousands(sn.credits.total_remain).c_str()));
            });
            note.clear();
        } else if (r.status == 429) {
            json j; try { j = json::parse(r.body); } catch (...) {}
            int ra = (int)JInt(j, "retry_after_sec", 0);
            int64_t cu = JInt(j, "cooldown_until");
            note = WideFormat(L"查询太频繁（服务端风控）：%d 秒后可再查", ra);
            if (cu > 0) Update([&](Snapshot& sn) { sn.credits.cooldown_until = cu; });
        } else if (r.status == 404) {
            note = L"/admin 未启用，无法查实时积分";
        } else if (r.status == 401) {
            note = L"api_key 不符（401）";
        } else {
            note = r.err.empty() ? WideFormat(L"查询失败（HTTP %lu）", r.status) : r.err;
        }
        if (!note.empty()) Update([&](Snapshot& sn) { NoteLocked(sn, note); });
        EndAct("credits");
        RefreshSoon();
    }).detach();
    return true;
}

} // namespace wb2
