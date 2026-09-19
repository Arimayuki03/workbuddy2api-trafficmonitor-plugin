// procctl.cpp — 见 procctl.h 注释。全部函数只允许在 worker/action 线程调用。
#include "procctl.h"
#include "common.h"
#include "logger.h"
#include "settings.h"
#include "http.h"
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shlwapi.lib")

namespace wb2::proc {
namespace {

std::wstring ToLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

std::wstring QueryImagePath(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"?";
    wchar_t buf[MAX_PATH * 4]{};
    DWORD size = ARRAYSIZE(buf);
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &size)) out = buf;
    else out = L"?";
    CloseHandle(h);
    return out;
}

} // namespace

Listener FindPortListener(int port)
{
    Listener r;
    const DWORD nets[2] = { AF_INET, AF_INET6 };
    for (DWORD net : nets) {
        DWORD cb = 0;
        GetExtendedTcpTable(nullptr, &cb, FALSE, net, TCP_TABLE_OWNER_PID_LISTENER, 0);
        if (cb == 0) continue;
        std::string buf(cb, '\0');
        if (GetExtendedTcpTable(buf.data(), &cb, FALSE, net, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
            continue;
        const MIB_TCPTABLE_OWNER_PID* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
        const MIB_TCP6TABLE_OWNER_PID* t6 = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf.data());
        if (net == AF_INET) {
            for (DWORD i = 0; i < t->dwNumEntries; i++) {
                // dwLocalPort 的低 16 位按网络字节序存端口（实测 7863 的原始值为 0x0000B71E），
                // 必须 ntohs 转主机序后再比较。与下方 IPv6 分支的 ntohl(x)>>16 等价。
                if (ntohs(static_cast<WORD>(t->table[i].dwLocalPort & 0xFFFF)) == static_cast<WORD>(port)) {
                    r.found = true;
                    r.pid = t->table[i].dwOwningPid;
                    r.exe_path = QueryImagePath(r.pid);
                }
            }
        } else {
            for (DWORD i = 0; i < t6->dwNumEntries; i++) {
                // 实测 IPv6 表同样低 16 位网络序；ntohl(x)>>16 恒等于 ntohs(x&0xFFFF)，公式保留。
                if ((ntohl(t6->table[i].dwLocalPort) >> 16) == static_cast<DWORD>(port)) {
                    r.found = true;
                    r.pid = t6->table[i].dwOwningPid;
                    r.exe_path = QueryImagePath(r.pid);
                }
            }
        }
        if (r.found) break;
    }
    if (r.found) {
        std::wstring want = ToLower(ServiceExePath());
        r.is_our_service = !want.empty() && ToLower(r.exe_path) == want;
    }
    return r;
}

std::wstring ServiceExePath()
{
    Settings s = SettingsStore::Instance().Get();
    if (s.service_dir.empty()) return L"";
    return s.service_dir + L"\\wb2api.exe";
}

bool ServiceFilesOk(std::wstring& err)
{
    Settings s = SettingsStore::Instance().Get();
    if (s.service_dir.empty()) { err = L"未配置服务目录"; return false; }
    if (!PathFileExistsW(ServiceExePath().c_str())) {
        err = L"找不到 " + ServiceExePath() + L"（请检查服务目录）";
        return false;
    }
    std::wstring cfg = s.service_dir + L"\\config.json";
    if (!PathFileExistsW(cfg.c_str())) {
        err = L"服务目录里没有 config.json（wb2api 启动必需）";
        return false;
    }
    return true;
}

bool StartService(std::wstring& err)
{
    if (!ServiceFilesOk(err)) return false;
    Settings s = SettingsStore::Instance().Get();
    Listener l = FindPortListener(s.port);
    bool created = false; // 本次调用是否真正创建了进程（healthz 超时文案据此区分口径）
    if (l.found) {
        if (l.is_our_service) {
            // 已在跑（可能还没就绪）：交给下方就绪等待
            LogI(L"start: 端口已被自家服务占用，等待就绪");
        } else {
            err = WideFormat(L"端口 %d 被其他程序占用（PID %lu %s），拒绝启动",
                s.port, l.pid, l.exe_path.c_str());
            return false;
        }
    } else {
        // 关键：wb2api 的 config/auths/data 全部相对 CWD 解析，工作目录必须设为服务目录。
        std::wstring exe = ServiceExePath();
        std::wstring cmd = L"\"" + exe + L"\" -config config.json";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back(0);
        BOOL ok = CreateProcessW(exe.c_str(), cmd_buf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, s.service_dir.c_str(), &si, &pi);
        if (!ok) {
            err = WideFormat(L"启动失败（CreateProcess %lu）", GetLastError());
            LogE(L"start: " + err);
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LogI(L"start: 进程已创建，等待就绪");
        created = true;
    }
    if (!WaitHealthzReady(15000)) {
        // 走"端口已被自家服务占用"分支时根本没有创建进程：超时多半是原本就占着端口的
        // （可能僵死的）服务没就绪，与"新拉起的进程没就绪"分开表述，避免误导排查方向。
        err = created
            ? L"进程已创建，但 15 秒内 /healthz 未就绪（查看服务目录 logs\\server.log）"
            : L"端口上的 wb2api 15 秒内未就绪（疑似已有僵死进程，可尝试重启服务；查看服务目录 logs\\server.log）";
        return false;
    }
    return true;
}

bool StopService(std::wstring& err)
{
    Settings s = SettingsStore::Instance().Get();
    Listener l = FindPortListener(s.port);
    if (!l.found) return true; // 已经不在
    if (!l.is_our_service) {
        err = WideFormat(L"端口 %d 被其他程序占用（PID %lu %s），拒绝停止外来进程",
            s.port, l.pid, l.exe_path.c_str());
        return false;
    }
    // 优先优雅停（admin 可用时：flush state.json 后干净退出）
    std::string key = SettingsStore::Instance().CurrentApiKey();
    std::wstring url = WideFormat(L"http://127.0.0.1:%d/admin/shutdown", s.port);
    HttpResponse resp = HttpJson(L"POST", url, key, "{}", 1500);
    if (resp.status == 200) {
        LogI(L"stop: /admin/shutdown 已受理");
    } else {
        LogI(L"stop: admin 不可用，直接结束进程");
    }
    // 等端口释放最多 3 秒（覆盖 admin 优雅路径），超时兜底 TerminateProcess
    for (int i = 0; i < 30; i++) {
        if (!FindPortListener(s.port).found) return true;
        Sleep(100);
    }
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, l.pid);
    if (!h) {
        err = WideFormat(L"无法结束进程 PID %lu（%lu）", l.pid, GetLastError());
        return false;
    }
    BOOL ok = TerminateProcess(h, 0);
    if (ok) WaitForSingleObject(h, 2000);
    CloseHandle(h);
    for (int i = 0; i < 20; i++) {
        if (!FindPortListener(s.port).found) return true;
        Sleep(100);
    }
    err = L"进程未能退出";
    return false;
}

bool WaitHealthzReady(int timeout_ms)
{
    Settings s = SettingsStore::Instance().Get();
    std::wstring url = WideFormat(L"http://127.0.0.1:%d/healthz", s.port);
    ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < static_cast<ULONGLONG>(timeout_ms)) {
        HttpResponse r = HttpJson(L"GET", url, "", "", 800);
        // 200/503 都算"活着"：503 只代表暂无可用账号；身份必须再对一次，
        // 防"端口被别的 HTTP 服务占着"被当成启动成功。
        if (r.status == 200 || r.status == 503) {
            if (r.x_service == L"workbuddy2api" || r.body.find("\"workbuddy2api\"") != std::string::npos)
                return true;
        }
        Sleep(400);
    }
    return false;
}

} // namespace wb2::proc
