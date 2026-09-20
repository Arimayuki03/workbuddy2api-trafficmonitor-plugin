// worker.h — 后台轮询线程 + 异步动作队列 + 快照存储。
// 线程模型（写死在约定里，别在 UI 线程做任何同步网络调用）：
//   worker 线程：周期轮 /healthz /status（poll_interval_sec）与 /admin/*（admin_poll_sec）；
//   动作线程：Start/Stop/RunTask/Toggle/RefreshCredits 一次性线程（同类互斥）；
//   UI 线程（DrawItem/tooltip/dialog 定时器）：只 Copy() 快照与读预生成文本。
#pragma once
#include "svcinfo.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace wb2 {

class Worker {
public:
    static Worker& Instance();

    void Start();                    // OnInitialize 调；幂等；会先等上一任超时残留线程退场
    void Stop();                     // DLL_PROCESS_DETACH：置位停止→有界等待→超时 detach 兜底（不能裸 join：见实现注释）
    bool Started() const { return started_.load(); }

    Snapshot Copy() const;
    SvcState State() const;              // 轻量：DrawItem 高频路径专用
    std::wstring DisplayValue() const;   // 预生成任务栏文本（锁内构建，UI 只拿副本）
    std::wstring TooltipText() const;

    // 动作接口：返回 false=同类动作已在执行（UI 据此禁用按钮）。
    bool RequestStartService();
    bool RequestStopService();
    bool RequestRestartService();
    bool RequestRunTask(const std::string& kind);            // kind 或 "all"
    bool RequestToggleTask(const std::string& kind, bool enabled);
    bool RequestSetTaskHours(const std::string& kind, const std::vector<int>& hours); // 写回服务 config.json（重启生效）
    bool RequestRefreshCredits();
    // 账号手动停用/恢复（上游 a20d06f 端点）：op ∈ "disable"|"enable"|"revive"。
    // disable=摘出选号池（签到/保活照常）；enable=解手动位；revive=解自动禁用位。
    // busy 键 "acct:<uid>"：同号操作互斥，不同号可并行。
    bool RequestAccountOp(const std::string& uid, const char* op);
    bool IsActionBusy(const std::string& key) const;         // "svc"/"credits"/"task:<kind>"/"acct:<uid>"
    // 把实时积分自动刷新周期异步同步到服务端冷却（PATCH /admin/credits-interval，
    // 热生效+写回服务 config.json；服务端区间 60–86400 秒）。动作线程内执行，结果经
    // action_note 回显；minutes<=0（关闭自动刷新）不下发直接返回。
    void RequestSyncCreditsInterval(int minutes);

    // 卸载/退出取消标志（复用轮询线程的 stop_）：动作线程与 procctl/autostart 的阻塞
    // 循环据此提前退出，防 detached 动作线程在宿主卸载后返回到已解映射的代码页。
    const std::atomic<bool>* CancelFlag() const { return &stop_; }
    bool StopRequested() const { return stop_.load(); }

    void RefreshSoon();              // 请求下一循环立即跑（动作完成后调用）

private:
    Worker() = default;
    void RunLoop(const std::atomic<bool>& run_stop); // run_stop=本轮线程专属停止标志（防超时 detach 的旧线程被新一轮 Start"复活"）
    void MaybeStartWithTm();         // start_with_tm：首轮轮询后拉起（worker 线程内，不派生裸线程）
    void PollOnce();                 // healthz+status（+防抖状态机+异常拉起）
    void PollAdmin();                // /admin/tasks + /admin/credits（服务端本地缓存，零上游）
    void ScanAuthExpiry();           // 本地 auths\*.json token 到期时间（零上游）
    void Update(std::function<void(Snapshot&)> fn);
    void BuildDisplayLocked();       // value/tooltip 预生成（必须在锁内调用）
    static void NoteLocked(Snapshot& s, const std::wstring& note);
    void SetUserStopped(bool v);     // 用户主动停止标志持久化（抑制 auto-relaunch / start_with_tm）
    bool BeginAct(const std::string& key);
    void EndAct(const std::string& key);

    mutable std::mutex mu_;          // 保护 snap_/value_w_/tooltip_w_/token_expiry_
    Snapshot snap_;
    std::wstring value_w_, tooltip_w_;
    std::map<std::string, int64_t> token_expiry_; // uid → unix 到期秒

    mutable std::mutex act_mu_;
    std::set<std::string> busy_;

    std::thread th_;
    std::mutex cv_mu_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{ false };        // 动作线程取消标志（CancelFlag）；Start 复位
    // 轮询线程本轮专属停止标志：Stop 置位后旧线程在当前迭代收尾必退出；每轮 Start 换新。
    // 专属而非复用 stop_，防"Stop 超时 detach 的旧线程"在新 Start 把 stop_ 复位后复活成双轮询线程。
    std::shared_ptr<std::atomic<bool>> run_flag_ = std::make_shared<std::atomic<bool>>(false);
    HANDLE zombie_ = nullptr;                // Stop 超时 detach 的旧线程句柄（下一任 Start 有界等待后关闭）
    std::atomic<bool> started_{ false };
    std::atomic<bool> refresh_flag_{ false };

    // 轮询节奏记账（worker 线程私有）
    ULONGLONG next_base_ = 0, next_admin_ = 0, next_auths_ = 0;
    ULONGLONG next_auto_credit_ = 0; // 实时积分自动刷新的下次尝试时间
    bool start_tm_done_ = false;     // start_with_tm 只试一次（worker 线程私有）
    int consecutive_soft_fail_ = 0;  // 超时类连续失败计数（防抖）
    std::deque<ULONGLONG> relaunch_ts_; // 异常拉起退避窗口
};

} // namespace wb2
