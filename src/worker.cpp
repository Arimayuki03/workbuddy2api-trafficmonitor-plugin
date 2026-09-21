// worker.cpp — 轮询状态机 + 动作队列实现。判定原则（都对应真实踩过的坑）：
//  * 503 且有服务身份 = "活着但没可用账号"，绝不判停止（wb2api 语义）；其余非 200（404/500…）
//    是真实故障，走与超时同款防抖降级，不伪装成"暂无可用账号"；
//  * 超时/无响应 ≠ 停止（保持旧态，连续 5 次才降级 Dead）；只有"连接被拒"类直接判停止；
//  * 身份校验双保险：X-Service 头 或 body.service —— 二者都不是 workbuddy2api → WrongService；
//  * 判"停止"前顺手查端口归属：被外来进程占用要显示"被占用"而不是"已停止"。
#include "worker.h"
#include "common.h"
#include "http.h"
#include "logger.h"
#include "procctl.h"
#include "settings.h"
#include "trace.h"      // WB2API_TRACE_LOG（/v1/stats 404 静默期跳过日志；trace.h 在构建脚本里）
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cstdlib>  // std::llabs（自愈校验用；别依赖 json.hpp 传递包含）
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

// tooltip 字符预算。宿主 MFC 对喂给 CToolTipCtrl::UpdateTipText 的文本有 1024 字符硬限
// （超限抛 CInvalidArgException → 弹"遇到不适当的参数。"），而 TM 把所有插件的 tooltip
// 拼成一条：TM 自身 ~300、其余插件占用不可控。实测本插件完整展开（8 账号+6 任务+模型
// 用量）739 字符曾三插件同载弹框；拼接总额插件侧无法观测，只能按最坏共存定预算：
// 700 ≈ 原版完整形态（实测 739）去掉最长一段账户明细的余量——正常运行显示效果与
// v1.5.0 一致，只在账户数暴增等极端轮询结果下才逐行收敛。
constexpr size_t kTipBudget = 700;

// 用 "\r\n" 连接（tooltip 专用：宿主 MFC 对裸 \n 渲染异常）。
std::wstring JoinLines(const std::vector<std::wstring>& lines)
{
    std::wstring out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += L"\r\n";
        out += lines[i];
    }
    return out;
}

// 该账号是否被用户隐藏出 tooltip（tip_hidden_uids 线性查找：账号 ≤ 常规两位数）
bool TipUidHidden(const Settings& st, const std::wstring& uid8)
{
    for (auto& u : st.tip_hidden_uids)
        if (u == uid8) return true;
    return false;
}

// 预算内取舍：从尾部逐行舍弃非骨架行（自然顺序即"模型用量 → 积分汇总 → 定时任务 →
// 账户明细"，离骨架越远越不重要）；若删到节标题成为末行则连标题一起删，不留悬空标题。
// 骨架行（状态头/地址/健康概要/提示/最近操作/尾注）永不舍弃。全删后仍超预算（极端长
// 字段）才对最后一行硬截加省略号。有舍弃时在骨架尾注前补一行"…"提示信息被省略。
std::wstring FitTipBudget(std::vector<std::wstring> lines, size_t budget)
{
    auto is_skeleton = [](const std::wstring& s) {
        if (s.empty()) return false;
        if (s.rfind(L"WorkBuddy2API：", 0) == 0) return true; // 状态头
        if (s.rfind(L"地址：", 0) == 0) return true;
        if (s.rfind(L"健康 ", 0) == 0) return true; // 健康概要（含在途满载附注）
        if (s.rfind(L"提示：", 0) == 0) return true; // 轮询层 last_error
        if (s.rfind(L"最近操作：", 0) == 0) return true;
        return s == L"单击此栏位打开设置";
    };
    auto is_title = [](const std::wstring& s) {
        return s.size() >= 4 && s.rfind(L"——", 0) == 0 &&
            s.find(L"——", 2) != std::wstring::npos;
    };
    auto total = [&lines] {
        size_t t = 0;
        for (auto& l : lines) t += l.size() + 2;
        return t;
    };

    bool dropped = false;
    for (size_t t = total(); t > budget;) {
        // 找末个非骨架行（尾注在最后，其前的提示/操作行是骨架，故倒序首个命中即有效）
        size_t last = lines.size();
        while (last-- > 0)
            if (!is_skeleton(lines[last])) break;
        if (last == SIZE_MAX) break; // 只剩骨架：无的可删
        t -= lines[last].size() + 2;
        lines.erase(lines.begin() + last);
        dropped = true;
        // 删完成员后节标题成了末个非骨架行 → 连标题删（悬空标题没有信息量）
        size_t prev = last;
        while (prev-- > 0)
            if (!is_skeleton(lines[prev])) break;
        if (prev != SIZE_MAX && is_title(lines[prev])) {
            t -= lines[prev].size() + 2;
            lines.erase(lines.begin() + prev);
        }
    }
    std::wstring out = JoinLines(lines);
    if (dropped) {
        // 在骨架尾注前补"…"（预算装得下才补；补不进说明骨架自身已顶满，放弃提示）
        std::wstring note = L"单击此栏位打开设置";
        size_t note_pos = out.rfind(L"\r\n" + note);
        std::wstring ell = L"\r\n…";
        if (note_pos != std::wstring::npos && out.size() + ell.size() <= budget) {
            out.insert(note_pos, ell);
        } else if (note_pos == std::wstring::npos && out.size() + ell.size() <= budget) {
            out += ell; // 无尾注（不该发生）：直接缀尾
        }
    }
    // 极端兜底：骨架行自身超预算（如超长 nickname 混进骨架不会发生，纯防呆）——硬截。
    if (out.size() > budget) {
        out.resize(budget - 1); // 原地截断（自引用 assign 标准不保证安全）
        out += L"…";
    }
    return out;
}
double JNum(const json& j, const char* k, double d = 0)
{
    auto it = j.find(k);
    if (it == j.end() || !it->is_number()) return d;
    return it->get<double>();
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
    // 与 wb2api 合并版 kindNames 对齐（checkin/travel/activity/keepalive/school/cat/queue）。
    // queue=任务中心执行队列的定时排程（网页端任务中心「启动执行队列」的定时版），
    // 服务端 queue_enabled 缺省 false。
    static const std::map<std::string, std::wstring> m = {
        { "checkin", L"签到" }, { "travel", L"猫猫旅行" }, { "activity", L"活跃上报" },
        { "keepalive", L"Token 保活" }, { "school", L"开学季" }, { "cat", L"夜猫子" },
        { "queue", L"任务队列" },
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
    // 上一任 Stop 超时 detach 的残留线程：有界等待其退场后关句柄（句柄不关会随每次
    // 超时累积泄漏）。等不到也照关——句柄只是内核引用，线程对象在其退出时自灭。
    // 旧线程持有上一代专属 run_flag_（已置位 true，且永不复位），最多跑完当前一次
    // 轮询就退出；下面换新标志对象，绝不复位旧标志（僵尸若还卡在长 HTTP 里，
    // 复位会把它"复活"成与新一代并存的第二个常驻轮询线程）。
    if (zombie_ != nullptr) {
        WaitForSingleObject(zombie_, 2000);
        CloseHandle(zombie_);
        zombie_ = nullptr;
    }
    run_flag_ = std::make_shared<std::atomic<bool>>(false);
    th_ = std::thread([this, f = run_flag_] { RunLoop(*f); });
}

void Worker::Stop()
{
    if (!started_.exchange(false)) return;
    stop_ = true;            // 动作线程取消（CancelFlag 消费方）
    run_flag_->store(true);  // 轮询线程本轮专属停止
    cv_.notify_all();
    if (!th_.joinable()) return;
    if (th_.get_id() == std::this_thread::get_id()) {
        // 延迟卸载路径：DllMain(DETACH) 在轮询线程自身栈上补发，join 自己必死锁。
        // 只能置位后 detach 离场，让线程跑完收尾。
        th_.detach();
        return;
    }
    // 有界等待：进程退出时 ExitProcess 已终止线程，句柄即刻有信号（实测 join <1ms）。
    // 3 秒兜底：万一线程还活着（正卡在一次 HTTP），detach 后其模块引用会继续钉住
    // 映像，代码页不会失效（已实测）；句柄记入 zombie_，由下一任 Start 有界等待后
    // 关闭——旧线程持有专属 run_flag_（已置位），必在当前轮询迭代收尾退出，
    // started_ 不再需要抬回 true（旧实现因此永久废掉后续 Start，已修复）。
    if (WaitForSingleObject(th_.native_handle(), 3000) == WAIT_OBJECT_0) {
        th_.join();
    } else {
        zombie_ = th_.native_handle();
        th_.detach();
    }
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

void Worker::RunLoop(const std::atomic<bool>& run_stop)
{
    LogI(L"worker 线程启动");
    while (!run_stop.load()) {
        ULONGLONG now = GetTickCount64();
        Settings s = SettingsStore::Instance().Get();
        if (now >= next_base_ || refresh_flag_.exchange(false)) {
            MaybeStartWithTm();
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
            [this, &run_stop] { return run_stop.load() || refresh_flag_.load(); });
    }
    LogI(L"worker 线程退出");
}

// start_with_tm：随 TM 启动拉起服务。旧实现派生 sleep(2s) 的裸 detach 线程（卸载竞态，
// 见 plugin.cpp EnsureInited 注释）；挪到首轮 PollOnce 之前执行——此刻 TM 桌面/网络
// 就绪度与旧方案的"2 秒后"相当（TM 加载插件到首绘本就有间隙），且判定失败只顺延到
// 下轮轮询重试一次，同类互斥由 BeginAct("svc") 保证。
void Worker::MaybeStartWithTm()
{
    if (start_tm_done_) return;
    start_tm_done_ = true;
    Settings s = SettingsStore::Instance().Get();
    if (!s.start_with_tm || s.service_dir.empty()) return;
    if (State() != SvcState::Stopped) return; // 已在跑/被占用：不干预
    LogI(L"start_with_tm: 拉起服务");
    RequestStartService();
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
            // 非 200 细分：503=wb2api"活着但无可用账号"（Unservable，绝不判停止）；
            // 其余非 200（404/500/401…）是真实故障，不能一律判成 Unservable 把橙点
            // 显示成"暂无可用账号"——走与超时同款防抖：保持旧态 + 错误行标状态码，
            // 连续 5 次自然降级 Dead。
            if (hr.status == 200) {
                patch.state = SvcState::Running;
                consecutive_soft_fail_ = 0;
            } else if (hr.status == 503) {
                patch.state = SvcState::Unservable;
                consecutive_soft_fail_ = 0; // 有应答且身份对=联络成功，打断防抖连败
            } else {
                consecutive_soft_fail_++;
                patch.state = SvcState::Unknown; // 占位，Update 里保持旧态
                if (consecutive_soft_fail_ >= 5) patch.state = SvcState::Dead;
                patch.last_error = WideFormat(L"/healthz 返回 HTTP %lu", hr.status);
            }
        } else {
            patch.state = SvcState::WrongService;
            patch.last_error = L"该端口有 HTTP 服务应答，但不是 workbuddy2api（可能装错目录/端口冲突）";
        }
    } else {
        // 传输层失败 → 归类
        if (hr.transport == TransportError::Unreachable) {
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
            if (patch.last_error.empty())
                patch.last_error = L"/status 401：api_key 未配置或不符（在设置页填写，或放好服务目录 config.json）";
        } else if (sr.status == 200) {
            json sj;
            try { sj = json::parse(sr.body); } catch (...) {}
            if (sj.is_object()) {
                patch.cooling = (int)JInt(sj, "cooling");
                patch.disabled_n = (int)JInt(sj, "disabled");
                patch.in_flight_full = (int)JInt(sj, "in_flight_full");
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
                        // 运维手动停用位（上游 a20d06f）：与自动禁用并列独立，叠加态分别透出。
                        ai.manual_disabled = JBool(a, "manual_disabled");
                        ai.until = RfcToUnix(JStr(a, "until"));
                        ai.reason = Utf8ToWide(JStr(a, "reason"));
                        if (ai.disabled && ai.reason.empty())
                            ai.reason = Utf8ToWide(JStr(a, "disabled_reason"));
                        // 手动停用原因独立于自动禁用原因：两位叠加时各自显示，不合并。
                        ai.manual_reason = Utf8ToWide(JStr(a, "manual_reason"));
                        // 三合一冷却的另外两翼：熔断与连败降权（上游 #114）。
                        // 服务端 Cooling=true 时可能是三者任一，恢复时刻展示取最远。
                        ai.breaker_until = RfcToUnix(JStr(a, "breaker_until"));
                        ai.degrade_until = RfcToUnix(JStr(a, "degrade_until"));
                        ai.consec_fails = (int)JInt(a, "consecutive_fails");
                        // 模型级限额（issue #36）：账号健康但这些模型还在独立冷却。
                        // 明细行带模型名与双时钟（until=截断后冷却截止，reset_at=上游
                        // 原始重置墙钟），状态列/tooltip 指名道姓；rl_models 保留总数口径。
                        if (a.contains("rate_limited_models") && a["rate_limited_models"].is_array()) {
                            for (auto& m : a["rate_limited_models"]) {
                                if (!m.is_object()) continue;
                                ai.rl_models++;
                                RlModel rm;
                                rm.model = Utf8ToWide(JStr(m, "model"));
                                rm.until = RfcToUnix(JStr(m, "until"));
                                rm.reset_at = RfcToUnix(JStr(m, "reset_at"));
                                rm.reason = Utf8ToWide(JStr(m, "reason"));
                                if (rm.until > ai.rl_until) ai.rl_until = rm.until;
                                ai.rl_detail.push_back(std::move(rm));
                            }
                        }
                        // 每模型在途台账（fork 扩展）：模型名 → 计数，只含正在请求的模型。
                        if (a.contains("in_flight_by_model") && a["in_flight_by_model"].is_object()) {
                            for (auto it = a["in_flight_by_model"].begin();
                                 it != a["in_flight_by_model"].end(); ++it) {
                                if (!it.value().is_number_integer()) continue;
                                ai.in_flight_models.emplace_back(
                                    Utf8ToWide(it.key()), (int)it.value().get<int>());
                            }
                        }
                        // 成本台账（上游 2493532）：每模型一行，双行弹窗展示（不进 tooltip，控长度）。
                        if (a.contains("model_costs") && a["model_costs"].is_array()) {
                            for (auto& m : a["model_costs"]) {
                                if (!m.is_object()) continue;
                                ModelCost mc;
                                mc.model = Utf8ToWide(JStr(m, "model"));
                                mc.per1k = JNum(m, "cost_per_1k");
                                mc.samples = (int)JInt(m, "samples");
                                mc.last_seen = RfcToUnix(JStr(m, "last_seen"));
                                ai.costs.push_back(std::move(mc));
                            }
                        }
                        ai.in_flight = (int)JInt(a, "in_flight");
                        ai.uid = uid;
                        patch.accounts.push_back(std::move(ai));
                    }
                    patch.accounts_valid = true;
                }
            }
        }
    }

    // 整表替换后立刻用 token_expiry_ 缓存回填令牌到期：/status 不带该字段，新解析
    // 的 token_expiry 恒 0；若只等下一轮 ScanAuthExpiry（5 分钟一次），令牌列每个
    // 轮询周期都会闪回 "-"（用户可见的空白 bug）。缓存 miss 保持 0=未知。
    Update([&](Snapshot& sn) {
        if (patch.state != SvcState::Unknown) sn.state = patch.state;
        sn.last_error = patch.last_error;
        sn.pid = patch.pid;
        if (id_ok) {
            sn.total = patch.total; sn.healthy = patch.healthy;
            sn.cooling = patch.cooling; sn.disabled_n = patch.disabled_n; sn.sticky = patch.sticky;
            sn.in_flight_full = patch.in_flight_full;
            if (patch.accounts_valid) {
                sn.accounts = std::move(patch.accounts); sn.accounts_valid = true;
                for (auto& a : sn.accounts) {
                    auto it = token_expiry_.find(a.uid);
                    if (it != token_expiry_.end()) a.token_expiry = it->second;
                }
            }
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

    // —— 实时积分自动刷新（默认关；开着则严格贴服务端冷却节奏） ——
    // 节奏以服务端实际冷却为准：next_auto_credit_ 只做"别空转"的闸，
    // 冷却没过就每 5 秒重查 cooldown_until，一到点立刻补查——即使本函数
    // 很少被走到（冷却 >> 轮询周期），准点性也由这里兜住。
    if (s.credits_refresh_interval_min > 0 && sn.admin_available && !IsActionBusy("credits")) {
        int64_t nowx = NowSec();
        if (sn.credits.cooldown_until <= nowx) {
            if (nowx >= (int64_t)next_auto_credit_ / 1000) {
                RequestRefreshCredits();
                next_auto_credit_ = (ULONGLONG)(nowx + 5) * 1000; // 5 秒后仍未过闸再试
            }
        }
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

    // /v1/stats 按模型用量（GET，与 /status 同源 api_key 鉴权）：credit=该模型累计
    // 消耗积分，服务端进程内存聚合（重启清零）。404=旧版服务端无此接口：记下探测
    // 时刻并静默 30 分钟再重探——旧版周期性白发请求是浪费，但保留低频重探才能在
    // 服务端热升级后自动恢复（重启探活由 state 变化兜底，不依赖本间隔）。
    {
        bool skip = false;
        Update([&](Snapshot& sn) {
            if (!sn.stats_available && sn.stats_ts > 0)
                skip = NowSec() - sn.stats_ts < 1800;
        });
        if (skip) { WB2API_TRACE_LOG("PollAdmin: skip /v1/stats (404 cooldown)"); return; }
    }
    HttpResponse vr = HttpJson(L"GET", base + L"/v1/stats", key, "", 3000);
    if (vr.status == 200) {
        json vj;
        try { vj = json::parse(vr.body); } catch (...) { return; }
        if (!vj.is_object() || !vj.contains("models") || !vj["models"].is_array()) return;
        std::vector<ModelUsage> usage;
        for (auto& m : vj["models"]) {
            if (!m.is_object()) continue;
            ModelUsage mu;
            mu.model = Utf8ToWide(JStr(m, "model"));
            if (mu.model.empty()) continue;
            mu.requests = JInt(m, "requests");
            mu.credit = JNum(m, "credit");
            mu.credit_per_req = JNum(m, "credit_per_req");
            mu.total_tokens = JInt(m, "total_tokens");
            // 上游积分倍率原文（上游 5009a1f，"x0.06" 形态；目录冷缓存/未下发时
            // omitempty 整体省略）。缺失≠免费：空串原样保留，显示侧整体省略。
            mu.ratio = Utf8ToWide(JStr(m, "credits"));
            usage.push_back(std::move(mu));
        }
        Update([&](Snapshot& sn) {
            sn.stats_available = true;
            sn.stats_ts = (int64_t)time(nullptr);
            sn.usage = std::move(usage);
        });
    } else if (vr.status == 404) {
        // stats_ts 记最后探测时刻（非成功时刻）：既是 30 分钟重探间隔的锚点，
        // 也让 tooltip 的"需升级服务端"提示有条件成立（老版本从不置 stats_ts 的话
        // 提示永不显示——OCR 审查确认的口径分裂）。
        Update([](Snapshot& sn) { sn.stats_available = false; sn.usage.clear();
                                  sn.stats_ts = (int64_t)time(nullptr); });
    }
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
        // 双形态探测（同服务端 internal/auth/auth.go Parse）：嵌套形取
        // account.uid + auth.expiresAt；扁平形（手写/旧版）取顶层 uid/expiresAt。
        // 两形态 expiresAt 均为 Unix 秒。
        std::string uid;
        int64_t exp = 0;
        if (j.contains("auth") && j["auth"].is_object()) {
            if (j.contains("account") && j["account"].is_object()) uid = JStr(j["account"], "uid");
            exp = JInt(j["auth"], "expiresAt");
        } else {
            uid = JStr(j, "uid");
            exp = JInt(j, "expiresAt");
        }
        if (uid.empty()) continue;
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
    if (StateIsOn(sn.state)) {
        std::wstring cnt = WideFormat(L"健康 %d/%d · 冷却 %d · 禁用 %d · 粘性会话 %d",
            sn.healthy, sn.total, sn.cooling, sn.disabled_n, sn.sticky);
        // 满载只在出现时占一行字：绿点但 chat 503 的元凶就是它（ServableNow 排除占满号）。
        if (sn.in_flight_full > 0)
            cnt += WideFormat(L" · 在途满载 %d", sn.in_flight_full);
        lines.push_back(cnt);
        // 手动停用计数（上游 a20d06f）：服务端把 manual_disabled 也计入 disabled 总数，
        // 这里从 accounts 侧拆出"其中手动停用 N 个"，两种停用一眼可分。
        int manual = 0;
        if (sn.accounts_valid)
            for (auto& a : sn.accounts) if (a.manual_disabled) manual++;
        if (manual > 0) lines.push_back(WideFormat(L"其中手动停用 %d 个（右键账户行可恢复）", manual));
    }
    if (!sn.last_error.empty()) lines.push_back(L"提示：" + sn.last_error);

    // 账户明细区（tooltip_accounts）：每账号一行是 tooltip 最大的长度来源（实测 8 号
    // ~430 字符）。设置里可关掉，只留上面的健康概要——多插件同载挤占 1024 总额时的
    // 主要手段；关闭后 FitTipBudget 的逐行舍弃仍有兜底作用。
    // 积分只显示实时值（v1.9.0）：估算=本地账本，随轮询单调扣减、长期偏差大，悬浮窗
    // 这种小字号场景宁可少也不误导；实时缓存缺失时该号就不显示积分数，需要精确值的
    // 去设置窗账户表（查询实时积分）。
    if (st.tooltip_accounts && StateIsOn(sn.state) && sn.accounts_valid && !sn.accounts.empty()) {
        lines.push_back(L"—— 账户 ——");
        int shown = 0;
        for (auto& a : sn.accounts) {
            // 单账户显隐（右键账户行切换）：隐藏的号不占 tooltip 行，但仍计入
            // 健康概要等汇总行（那里没有 per-account 信息，无需改动）。
            // 隐藏判断必须在 shown 计数之前，否则隐藏号白耗 8 行显示名额。
            if (TipUidHidden(st, a.uid8)) continue;
            if (shown++ >= 8) { lines.push_back(L"…"); break; }
            std::wstring note;
            if (a.disabled && a.manual_disabled) note = L"禁用+停用";
            else if (a.manual_disabled) note = a.manual_reason.empty() ? L"已停用" : L"已停用(" + a.manual_reason + L")";
            else if (a.disabled) note = L"已禁用";
            else if (a.cooling) note = L"冷却中";
            else if (a.in_flight > 0) note = L"请求中";
            else note = L"正常";
            // 在途模型（fork 的 in_flight_by_model）：账号有在途请求时指名道姓，
            // "请求中"升级为"请求中 glm-4.6×2"；无台账（旧版服务端/恰好归零）回退纯计数。
            if (a.in_flight > 0 && !a.in_flight_models.empty()) {
                note = L"请求中";
                // 与限流明细同口径最多列 2 个：账号并发多模型时防 tooltip 顶到
                // 宿主 1024 字符硬限（BuildDisplayLocked 尾注）。
                int shown_if = 0;
                for (auto& [mname, mcnt] : a.in_flight_models) {
                    if (shown_if >= 2) {
                        note += WideFormat(L" +%d个", (int)a.in_flight_models.size() - shown_if);
                        break;
                    }
                    note += WideFormat(L" %s×%d", mname.c_str(), mcnt);
                    shown_if++;
                }
            }
            // 模型级限流明细（issue #36 的 rate_limited_models）：指名道姓带恢复时刻，
            // 最多列 2 个防 tooltip 膨胀，更多用 "共N个" 收口（"A、B共3个"是总数惯用法）。
            if (!a.rl_detail.empty()) {
                std::wstring rls;
                int shown_rl = 0;
                for (auto& rm : a.rl_detail) {
                    if (shown_rl >= 2) break;
                    rls += (rls.empty() ? L"" : L"、") + rm.model;
                    shown_rl++;
                }
                if ((int)a.rl_detail.size() > 2)
                    rls += WideFormat(L"共%d个", (int)a.rl_detail.size());
                note += WideFormat(L" 限流[%s", rls.c_str());
                if (a.rl_until > NowSec())
                    note += WideFormat(L" %s恢复", FormatTimeShort(a.rl_until).c_str());
                note += L"]";
            }
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
            lines.push_back(WideFormat(L"  %s (%s) %s%s%s", a.nickname.c_str(),
                a.realm.empty() ? L"cn" : a.realm.c_str(),
                note.c_str(), live.c_str(), tk.c_str()));
        }
    }

    if (StateIsOn(sn.state)) {
        if (sn.admin_available && !sn.tasks.empty()) {
            // tooltip_tasks 开关（v1.7.0，默认关）：定时任务区不进 tooltip 的原因是它
            // "always there, rarely useful"——6 行 × 20+ 字符的稳定开销换多数时间无观测
            // 价值的信息；需要盯任务触发时刻的可在设置④显示页打开。关闭时健康概要等
            // 汇总行不受影响，任务详情仍看设置窗③与插件命令菜单。
            if (st.tooltip_tasks) {
                lines.push_back(L"—— 定时任务 ——");
                for (auto& t : sn.tasks) {
                    std::wstring mark = t.running ? L"执行中" : (t.enabled ? L"启用" : L"停用");
                    std::wstring nxt = (t.running || !t.enabled || t.next_fire.empty())
                        ? L"" : WideFormat(L" 下次%s", t.next_fire.c_str());
                    lines.push_back(L"  " + t.label + L"[" + mark + nxt + L"]");
                }
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
        // 按模型用量（/v1/stats）：每个模型自服务启动以来消耗的积分与请求数。
        // credit 是上游 usage.credit 的累计和（真实扣费口径）；服务重启清零。
        if (sn.stats_available && !sn.usage.empty()) {
            lines.push_back(L"—— 模型用量（本次运行累计）——");
            int shown_u = 0;
            for (auto& u : sn.usage) {
                if (shown_u++ >= 6) { lines.push_back(L"  …"); break; }
                // 积分倍率（上游 5009a1f 的 /v1/stats credits 字段）：有则缀尾，
                // 空串整体省略（缺失≠免费，不显示 "x0.00"）。
                std::wstring ratio = u.ratio.empty() ? L"" : L"（" + u.ratio + L"）";
                lines.push_back(WideFormat(L"  %s：%s分 / %d次（均 %s/次）%s",
                    u.model.c_str(), FormatCreditNum(u.credit).c_str(),
                    (int)u.requests, FormatCreditNum(u.credit_per_req).c_str(),
                    ratio.c_str()));
            }
        } else if (!sn.stats_available && sn.stats_ts > 0 && NowSec() - sn.stats_ts < 1800) {
            // 最近 30 分钟内探测过（200 或 404 都置 stats_ts）且当前不可用：
            // 提示升级服务端；超 30 分钟视为探测信息过期，不再占 tooltip 行。
            lines.push_back(L"模型用量：需 wb2api ≥ v1/stats 接口（升级服务端）");
        }
    }

    if (!sn.action_note.empty() && NowSec() - sn.action_note_ts < 25)
        lines.push_back(L"最近操作：" + sn.action_note);
    lines.push_back(L"单击此栏位打开设置");

    // 拼接为 tooltip。注意：
    //  * 换行必须用 "\r\n"——宿主把它直接喂给 MFC CToolTipCtrl，裸 \n 在部分宿主路径上渲染异常；
    //  * TM 会把所有插件的 tooltip 拼成一条再喂给 MFC CToolTipCtrl::UpdateTipText，后者对
    //    超过 1024 字符的文本抛 CInvalidArgException（宿主弹"遇到不适当的参数。"）。
    //    拼接总额插件侧无法观测，只能给自家文本上硬预算（kTipBudget）：完整展开模式超预算
    //    时从尾部整行舍弃，骨架必留（FitTipBudget）。折叠模式（tooltip_full=false）是
    //    更省的手动逃生口：每行 64 字符 / 4 行 / 总长 150 字符兜底。
    if (st.tooltip_full) {
        tooltip_w_.assign(JoinLines(lines));
        if (tooltip_w_.size() > kTipBudget)
            tooltip_w_.assign(FitTipBudget(std::move(lines), kTipBudget));
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
// 覆盖防护：写回前复核 mtime，与服务端（任务勾选 PATCH 会落盘）并发写时放弃本次并
// 提示重试，避免用旧副本把服务端刚持久化的改动静默回滚（审查 Medium 项）。
static std::wstring SetTaskHours(const std::wstring& config_path, const std::string& key,
    const std::vector<int>& hours)
{
    using json = nlohmann::json;
    HANDLE h = CreateFileW(config_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"读取 config.json 失败（路径不对或无权限）";
    FILETIME mt{};
    BOOL mt_ok = GetFileTime(h, nullptr, nullptr, &mt);
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

    // mtime 复核：读与写之间文件被改过（服务端并发落盘）→ 放弃，让用户重试
    HANDLE ck = CreateFileW(config_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ck != INVALID_HANDLE_VALUE) {
        FILETIME mt2{};
        BOOL ok2 = GetFileTime(ck, nullptr, nullptr, &mt2);
        CloseHandle(ck);
        if (ok2 && mt_ok &&
            (mt2.dwLowDateTime != mt.dwLowDateTime || mt2.dwHighDateTime != mt.dwHighDateTime))
            return L"config.json 在读取后又被修改（服务端并发写入），为防覆盖已放弃；请重试";
    }

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
        if (stop_.load()) { EndAct(key); return; } // 宿主卸载：不开工直接退场
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
        if (!stop_.load()) RefreshSoon();
    }).detach();
    return true;
}

void Worker::SetUserStopped(bool v)
{
    // 原子读-改-写：先 Get 再 Update 的两步写法会与 UI/动作线程并发保存设置互相覆盖。
    SettingsStore::Instance().Modify([v](Settings& st) { st.user_stopped = v; });
}

bool Worker::RequestStartService()
{
    if (!BeginAct("svc")) return false;
    std::thread([this] {
        if (stop_.load()) { EndAct("svc"); return; } // 宿主卸载：不开工直接退场
        SetUserStopped(false);
        Update([](Snapshot& sn) {
            sn.state = SvcState::Starting;
            NoteLocked(sn, L"正在启动服务…");
        });
        std::wstring err;
        bool ok = proc::StartService(err, &stop_);
        if (stop_.load()) { EndAct("svc"); return; } // 取消后不碰快照/日志，尽快退场
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
        if (stop_.load()) { EndAct("svc"); return; }
        SetUserStopped(true); // 抑制自动拉起（持久化：跨 TM 重启也记住"是用户主动停的"）
        std::wstring err;
        bool ok = proc::StopService(err, &stop_);
        if (stop_.load()) { EndAct("svc"); return; }
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
        if (stop_.load()) { EndAct("svc"); return; }
        SetUserStopped(false);
        std::wstring err;
        if (!proc::StopService(err, &stop_)) {
            if (stop_.load()) { EndAct("svc"); return; }
            Update([&](Snapshot& sn) { NoteLocked(sn, L"重启失败（停止步骤）：" + err); });
            EndAct("svc");
            RefreshSoon();
            return;
        }
        if (stop_.load()) { EndAct("svc"); return; }
        Update([](Snapshot& sn) {
            sn.state = SvcState::Starting;
            NoteLocked(sn, L"正在重启服务…");
        });
        bool ok = proc::StartService(err, &stop_);
        if (stop_.load()) { EndAct("svc"); return; }
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
        if (stop_.load()) { EndAct(key); return; }
        Settings s = SettingsStore::Instance().Get();
        std::string bearer = SettingsStore::Instance().CurrentApiKey();
        std::wstring url = WideFormat(L"http://127.0.0.1:%d/admin/tasks/run", s.port);
        json body{ { "kind", kind } };
        HttpResponse r = HttpJson(L"POST", url, bearer, body.dump(), 8000);
        if (stop_.load()) { EndAct(key); return; }
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
        if (stop_.load()) { EndAct(key); return; }
        Settings s = SettingsStore::Instance().Get();
        std::string bearer = SettingsStore::Instance().CurrentApiKey();
        std::wstring url = WideFormat(L"http://127.0.0.1:%d/admin/tasks", s.port);
        json body{ { "kind", kind }, { "enabled", enabled } };
        HttpResponse r = HttpJson(L"PATCH", url, bearer, body.dump(), 4000);
        if (stop_.load()) { EndAct(key); return; }
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
        if (stop_.load()) { EndAct("credits"); return; }
        Settings s = SettingsStore::Instance().Get();
        std::string bearer = SettingsStore::Instance().CurrentApiKey();
        std::wstring url = WideFormat(L"http://127.0.0.1:%d/admin/credits", s.port);
        // 每号一次上游查询（服务端已限速+冷却）；本地网络到 loopback，放宽到 120s 纯防卡死。
        HttpResponse r = HttpJson(L"POST", url, bearer, "{}", 120000);
        if (stop_.load()) { EndAct("credits"); return; }
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

// 账号手动停用/恢复（上游 a20d06f，POST /admin/accounts/{uid}/{op}）。
// disable=摘出选号池（独立 manual_disabled 位，签到/保活/排程照常）；
// enable=解手动位；revive=解自动禁用位（disabled）；两位都清账号才回池。
// 响应回显操作后双位状态（uid/manual_disabled/disabled/changed），端点幂等：
// 重复调用只更新原因，不报错。busy 键 "acct:<uid>"：同号互斥，不同号并行。
bool Worker::RequestAccountOp(const std::string& uid, const std::string& op)
{
    if (uid.empty()) return false;
    const std::string key = "acct:" + uid;
    if (!BeginAct(key)) return false;
    std::thread([this, uid, op, key] {
        if (stop_.load()) { EndAct(key); return; }
        Settings s = SettingsStore::Instance().Get();
        std::string bearer = SettingsStore::Instance().CurrentApiKey();
        std::wstring url = WideFormat(L"http://127.0.0.1:%d/admin/accounts/%s/%s",
            s.port, Utf8ToWide(uid).c_str(), Utf8ToWide(op).c_str());
        // 空体即可（服务端 reason 可选）；4 秒：loopback 内存操作，纯防卡死。
        HttpResponse r = HttpJson(L"POST", url, bearer, "{}", 4000);
        if (stop_.load()) { EndAct(key); return; }
        const wchar_t* opzh = op == "disable" ? L"停用"
            : op == "enable" ? L"恢复" : L"复活";
        std::wstring note;
        if (r.status == 200) {
            json j;
            bool parsed = true;
            try { j = json::parse(r.body); } catch (...) { parsed = false; }
            // 200 但响应体不是 JSON（协议回归）时绝不写回：默认构造的 json 会让
            // JBool 全取 false，账号会被误标为"已恢复"，只能提示等下轮 /status 纠正。
            if (parsed) {
                // 拿回显的双位状态直接定位该号：多数场景下下一轮 /status 也会带回同值，
                // 这里先写一次让 UI 立即反映（服务端口径：changed=false 也算成功）。
                bool md = JBool(j, "manual_disabled");
                bool da = JBool(j, "disabled");
                std::string mr = JStr(j, "manual_reason");
                Update([&](Snapshot& sn) {
                    for (auto& a : sn.accounts) {
                        if (a.uid != uid) continue;
                        a.manual_disabled = md;
                        a.disabled = da;
                        // 手动位与原因成对维护：解手动位时同步清原因，避免快照残留
                        // 旧原因文本（UI 虽有 manual_disabled 守卫，防御性清干净）。
                        if (md) a.manual_reason = Utf8ToWide(mr);
                        else a.manual_reason.clear();
                    }
                    NoteLocked(sn, WideFormat(L"账号已%s（手动停用=%s 自动禁用=%s）", opzh,
                        md ? L"是" : L"否", da ? L"是" : L"否"));
                });
            } else {
                note = L"操作成功但响应解析失败（协议异常），等待下轮状态刷新";
            }
        } else if (r.status == 404) {
            // 两种可能：admin 未启用（路由整体不注册，纯文本 404）或 uid 不存在
            //（JSON 信封 not_found）。信封带 error.message，区分提示。
            json j;
            try { j = json::parse(r.body); } catch (...) {}
            std::wstring msg;
            if (j.contains("error") && j["error"].is_object())
                msg = Utf8ToWide(JStr(j["error"], "message"));
            note = msg.empty()
                ? L"/admin/accounts 不可用（admin 未启用或 wb2api 版本过旧）"
                : L"操作失败：" + msg;
        } else if (r.status == 401) {
            note = L"api_key 不符（401）";
        } else {
            note = r.err.empty() ? WideFormat(L"操作失败（HTTP %lu）", r.status) : r.err;
        }
        if (!note.empty()) Update([&](Snapshot& sn) { NoteLocked(sn, note); });
        EndAct(key);
        RefreshSoon();
    }).detach();
    return true;
}

// 把自动刷新周期异步同步到服务端冷却（PATCH /admin/credits-interval，动作线程）。
// 结果经 action_note 回显；0=关闭自动刷新不下发（服务端区间 60–86400 秒，下发 0 必 400）。
// 不在 UI 线程同步等结果：服务僵死时一次 PATCH 会把设置窗连同宿主消息泵冻结到超时
// （worker.h 线程模型铁律：UI 线程禁止任何同步网络调用）。
void Worker::RequestSyncCreditsInterval(int minutes)
{
    // 0=关闭自动刷新：插件侧不会再自动查询，服务端冷却维持原状即可。
    if (minutes <= 0) return;
    const std::string key = "credits_interval";
    if (!BeginAct(key)) return; // 上一次同步还没回来：丢弃本次（保存路径连点防护）
    std::thread([this, minutes, key] {
        if (stop_.load()) { EndAct(key); return; }
        Settings s = SettingsStore::Instance().Get();
        std::string bearer = SettingsStore::Instance().CurrentApiKey();
        std::wstring base = WideFormat(L"http://127.0.0.1:%d", s.port);
        json body{ { "interval_sec", minutes * 60 } };
        // 5 秒：loopback 小请求的宽裕上限
        HttpResponse r = HttpJson(L"PATCH", base + L"/admin/credits-interval", bearer, body.dump(), 5000);
        if (stop_.load()) { EndAct(key); return; }
        std::wstring note;
        if (r.status == 200) {
            json j; try { j = json::parse(r.body); } catch (...) {}
            // 自愈：读回服务端实收值，分钟粒度取整的偏差（<1 分钟）直接接受
            int64_t applied = JInt(j, "interval_sec", -1);
            if (applied >= 0 && std::llabs(applied - (int64_t)minutes * 60) >= 60)
                note = WideFormat(L"服务端实收间隔 %lld 秒与请求 %d 分钟不一致", applied, minutes);
            else
                note = WideFormat(L"自动刷新周期已同步服务端（%d 分钟）", minutes);
        } else if (r.status == 404) {
            note = L"wb2api 版本过旧，无 /admin/credits-interval 接口（升级服务后重试）";
        } else if (r.status == 400) {
            json j; try { j = json::parse(r.body); } catch (...) {}
            // 服务端错误走 OpenAI 风格：{"error":{"message":...}}
            std::wstring msg;
            if (j.contains("error") && j["error"].is_object())
                msg = Utf8ToWide(JStr(j["error"], "message"));
            note = msg.empty() ? L"服务端拒绝该间隔（400）" : msg;
        } else if (r.status == 401) {
            note = L"api_key 不符（401）";
        } else {
            note = r.err.empty() ? WideFormat(L"同步失败（HTTP %lu）", r.status) : r.err;
        }
        Update([&](Snapshot& sn) { NoteLocked(sn, note); });
        EndAct(key);
        RefreshSoon();
    }).detach();
}

} // namespace wb2
