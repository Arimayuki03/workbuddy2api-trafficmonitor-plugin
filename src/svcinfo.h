// svcinfo.h — 快照数据模型（worker 写、UI/自绘读；一律在 Worker 的锁内整体替换）。
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <windows.h>

namespace wb2 {

// 服务状态机。判定细节见 worker.cpp PollOnce 注释（503≠停止、超时≠停止、身份头校验）。
enum class SvcState {
    Unknown,       // 首轮轮询前
    Running,       // /healthz 200 + 身份匹配：运行且可服务
    Unservable,    // /healthz 503 + 身份匹配：进程活着但没有可用账号
    Starting,      // 用户点了启动、就绪探测中（动作线程写）
    Stopped,       // 端口无监听 / 拒绝连接
    WrongService,  // 端口被"不是 wb2api 的进程"占用，或应答无服务身份
    Dead,          // 端口仍被 wb2api 占用但连续多轮 HTTP 无响应（僵死）
};

inline bool StateIsOn(SvcState s) { return s == SvcState::Running || s == SvcState::Unservable || s == SvcState::Starting; }

// 每模型实测成本台账行（wb2api /status accounts[].model_costs，上游 2493532）：
// per1k=实测每千 token 均价（EMA，≤0=实测免费）；6 小时无观测服务端即删行。
struct ModelCost {
    std::wstring model;
    double per1k = 0;
    int samples = 0;          // 累计观测次数（EMA 收敛度参考）
    int64_t last_seen = 0;    // 末次观测（unix 秒）
};

struct AccountInfo {
    std::string uid;          // 原始 uid（auths 文件扫描合并用，不上界面）
    std::wstring uid8;
    std::wstring nickname;
    std::wstring realm;
    int64_t credits = 0;      // /status 估算
    // 服务端 cooling 标志是"三合一"口径：until（账号级冷却）、breaker_until（熔断）、
    // degrade_until（连败降权，上游 #114）任一未到期即为 true。恢复时刻展示取三者最远。
    bool cooling = false;
    bool disabled = false;
    std::wstring reason;
    int64_t until = 0;            // 账号级冷却至（unix 秒；0=无）
    int64_t breaker_until = 0;    // 熔断截止（0=无）
    int64_t degrade_until = 0;    // 连败降权截止（0=无）
    int consec_fails = 0;         // 连续失败计数（降权进度）
    // 模型级限额（issue #36）：账号整体健康、个别模型仍在独立冷却。
    // 非空即这些模型暂不可用，其他模型照常可选。
    size_t rl_models = 0;
    int64_t rl_until = 0;         // 限额模型中最远恢复时刻
    std::vector<ModelCost> costs; // 成本台账（双击账户行弹窗展示）
    int in_flight = 0;
    int64_t token_expiry = 0; // 本地 auths 文件扫描（unix 秒；0=未知）
};

struct TaskInfo {
    std::string kind;         // checkin/travel/activity/keepalive/school/cat
    std::wstring label;       // 中文名（服务端下发）
    bool enabled = true;
    std::vector<int> hours;
    std::wstring next_fire;   // "HH:mm"；禁用为空
    bool running = false;
    int64_t last_run = 0;
    std::wstring last_result;
};

struct CreditRow {
    std::wstring uid8, nickname;
    int64_t remain = -1;      // -1 = 该号查询失败
    int64_t used = -1;        // 累计已用（账号原始总量口径的消耗部分）；-1 = 未知
    int64_t size = -1;        // 套餐总量（remain+used 的原始总额度）；-1 = 未知
    bool ok = false;
    std::wstring error;
};

// live 积分快照（服务端缓存/查询的回放）
struct CreditsInfo {
    bool have = false;        // 服务端至少成功查过一次
    int64_t ts = 0;           // 查询完成时间
    int64_t total_remain = -1;
    int64_t total_used = -1;  // 全部账号已用合计
    std::vector<CreditRow> rows;
    int64_t cooldown_until = 0; // 服务端下次可查时间（unix 秒）
};

struct Snapshot {
    SvcState state = SvcState::Unknown;
    DWORD pid = 0;            // 端口持有者（仅 On/Wrong/Dead 时有意义）

    // /healthz
    int total = 0, healthy = 0;
    bool servable_cn = false, servable_global = false;

    // /status
    std::vector<AccountInfo> accounts;
    bool accounts_valid = false;  // /status 至少成功解析过一次（tooltip 显示账户区的前提）
    // in_flight_full：健康但在途名额占满的账号数（账号在此计数里是 healthy，
    // 但 chat 实际选不到它——全占满时 healthz 按 ServableNow 口径仍报 503）。
    int cooling = 0, disabled_n = 0, sticky = 0, in_flight_full = 0;
    int64_t status_ts = 0;

    // /admin/*
    bool admin_available = false;   // /admin/tasks 返回过 200
    std::vector<TaskInfo> tasks;
    int64_t tasks_ts = 0;
    CreditsInfo credits;

    // 观测/诊断
    std::wstring last_error;        // 最近一次中文错误（空=正常）
    bool need_key_note = false;     // /status 401：api_key 未配置或不符
    int64_t last_ok_ts = 0;

    // 动作回显（一次性提示，UI 页脚/通知用；带时间戳供"几秒内才显示"判断）
    std::wstring action_note;
    int64_t action_note_ts = 0;
};

} // namespace wb2
