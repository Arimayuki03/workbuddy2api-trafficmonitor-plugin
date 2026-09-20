// autostart.cpp — 实现。注意两处编码雷：
//  1. VBS 含中文路径 → 必须存成 UTF-16LE+BOM（WSH 按 Unicode 读取），ANSI/UTF-8 会乱码；
//  2. schtasks.exe 控制台输出是 OEM 码页 → 按 CP_OEMCP 转宽字符。
#include "autostart.h"
#include "common.h"
#include "logger.h"
#include "settings.h"
#include <vector>

namespace wb2::autostart {

const wchar_t* TaskName() { return L"WorkBuddy2API-Service"; }

namespace {

// 跑隐藏控制台命令并收集输出。返回退出码（-1=没跑起来；-2=cancel 置位中途放弃）。
// cancel（可空）：等待期间轮询，置位即杀掉子进程返回——宿主卸载时 detached 线程
// 不能继续在已解映射的模块上逗留（审查 High 项）。
LONG RunHidden(const std::wstring& cmd_line, std::wstring& output,
    const std::atomic<bool>* cancel = nullptr)
{
    output.clear();
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) return -1;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.hStdOutput = w;
    si.hStdError = w;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd_line.begin(), cmd_line.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
    CloseHandle(w);
    if (!ok) {
        CloseHandle(r);
        output = WideFormat(L"命令启动失败 %lu", GetLastError());
        return -1;
    }
    // 读管道 + 等进程：管道读阻塞时由"子进程已结束 → 读到 EOF"自然退出，
    // 取消则 TerminateProcess 强杀后 EOF 到来。等待分 200ms 切片轮询 cancel。
    bool cancelled = false;
    for (;;) {
        char chunk[512];
        DWORD got = 0;
        if (!ReadFile(r, chunk, sizeof chunk, &got, nullptr) || got == 0) break;
        int wl = MultiByteToWideChar(CP_OEMCP, 0, chunk, got, nullptr, 0);
        if (wl > 0) {
            std::wstring piece(wl, L'\0');
            MultiByteToWideChar(CP_OEMCP, 0, chunk, got, piece.data(), wl);
            output += piece;
        }
        if (cancel && cancel->load()) {
            cancelled = true;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    CloseHandle(r);
    if (cancelled) {
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return -2;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<LONG>(code);
}

std::wstring VbsPath()
{
    Settings s = SettingsStore::Instance().Get();
    return s.config_dir.empty() ? L"wb2api-autostart.vbs" : s.config_dir + L"\\wb2api-autostart.vbs";
}

// 生成 UTF-16LE+BOM 的隐藏启动器（幂等：路径写死当前 service_dir；改目录后重新勾选即更新）。
bool WriteLauncher(std::wstring& err)
{
    Settings s = SettingsStore::Instance().Get();
    std::wstring exe = s.service_dir + L"\\wb2api.exe";
    std::wstring vbs;
    vbs += L"' WorkBuddy2API 开机自启启动器 —— 由 TrafficMonitor 插件生成，换目录后重新勾选会自动重写\r\n";
    vbs += L"Dim sh, fso\r\n";
    vbs += L"Set sh = CreateObject(\"WScript.Shell\")\r\n";
    vbs += L"Set fso = CreateObject(\"Scripting.FileSystemObject\")\r\n";
    vbs += L"WScript.Sleep 10000\r\n"; // 等登录会话网络/磁盘就绪，减少一次保活失败
    vbs += L"If fso.FileExists(\"" + exe + L"\") Then\r\n";
    vbs += L"    sh.CurrentDirectory = \"" + s.service_dir + L"\"\r\n";
    vbs += L"    sh.Run Chr(34) & \"" + exe + L"\" & Chr(34) & \" -config config.json\", 0, False\r\n";
    vbs += L"End If\r\n";

    // UTF-16LE + BOM
    std::string bytes;
    bytes.push_back(static_cast<char>(0xFF));
    bytes.push_back(static_cast<char>(0xFE));
    for (wchar_t c : vbs) {
        bytes.push_back(static_cast<char>(c & 0xFF));
        bytes.push_back(static_cast<char>((c >> 8) & 0xFF));
    }
    std::wstring path = VbsPath();
    _wremove(path.c_str());
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = WideFormat(L"写入启动器失败 %lu", GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != bytes.size()) {
        err = L"写入启动器不完整";
        return false;
    }
    return true;
}

} // namespace

bool IsInstalled(const std::atomic<bool>* cancel)
{
    std::wstring out;
    LONG code = RunHidden(L"schtasks /Query /TN \"" + std::wstring(TaskName()) + L"\" /FO LIST", out, cancel);
    return code == 0;
}

bool Install(std::wstring& err, const std::atomic<bool>* cancel)
{
    if (!WriteLauncher(err)) return false;
    std::wstring cmd =
        L"schtasks /Create /F /TN \"" + std::wstring(TaskName()) +
        L"\" /SC ONLOGON /RL LIMITED "
        L"/TR \"wscript.exe \\\"" + VbsPath() + L"\\\"\"";
    std::wstring out;
    LONG code = RunHidden(cmd, out, cancel);
    if (code == -2) return false; // 取消：静默放弃（宿主正在卸载，无需报告）
    if (code != 0) {
        err = L"schtasks 注册失败：" + TrimW(out);
        LogE(L"autostart: " + err);
        return false;
    }
    LogI(L"autostart: 计划任务已注册（登录触发 + 10s 延迟 + 隐藏）");
    return true;
}

bool Uninstall(std::wstring& err, const std::atomic<bool>* cancel)
{
    std::wstring out;
    LONG code = RunHidden(L"schtasks /Delete /F /TN \"" + std::wstring(TaskName()) + L"\"", out, cancel);
    if (code == -2) return false; // 取消：静默放弃
    if (code != 0 && IsInstalled()) {
        // 退出码非 0 且复查任务仍在才算失败；"任务本就不存在"在任意 locale 下都不再靠文案猜
        err = L"schtasks 删除失败：" + TrimW(out);
        return false;
    }
    _wremove(VbsPath().c_str());
    LogI(L"autostart: 计划任务已移除");
    return true;
}

} // namespace wb2::autostart
