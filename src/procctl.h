// procctl.h — wb2api 进程控制：端口→PID→镜像路径校验、启停、就绪等待。
// 铁律：只允许操作"路径与设置里 service_dir 一致"的 wb2api.exe——防杀错进程。
#pragma once
#include <string>
#include <windows.h>

namespace wb2::proc {

struct Listener {
    bool found = false;
    DWORD pid = 0;
    std::wstring exe_path; // 小写全路径；查询失败为 L"?"
    bool is_our_service = false; // exe_path == <service_dir>\wb2api.exe（不区分大小写）
};

Listener FindPortListener(int port);

// <service_dir>\wb2api.exe 全路径（来自当前 Settings）。
std::wstring ServiceExePath();
// exe + config.json 是否就位；错误给中文描述。
bool ServiceFilesOk(std::wstring& err);

// 启动服务：CreateProcessW(CREATE_NO_WINDOW, CWD=service_dir)。
// 端口已被自家服务占用 → 视为已启动只等就绪；被外来进程占用 → 报错不动它。
bool StartService(std::wstring& err);
// 优雅停止：/admin/shutdown（若可用）→ 3 秒内等端口释放 → 兜底 TerminateProcess。
bool StopService(std::wstring& err);
// /healthz 出 JSON（200 或 503 均算就绪——503 只是没可用账号）。
bool WaitHealthzReady(int timeout_ms);

} // namespace wb2::proc
