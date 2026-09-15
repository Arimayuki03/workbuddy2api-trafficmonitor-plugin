#include "logger.h"
#include "common.h"
#include <mutex>
#include <sys/stat.h>

namespace wb2 {

namespace {
std::mutex g_log_mu;
std::wstring g_path;
bool g_enabled = false;
} // namespace

void LogInit(const std::wstring& path, bool enabled)
{
    std::lock_guard<std::mutex> lk(g_log_mu);
    g_path = path;
    g_enabled = enabled;
}

void Log(const std::wstring& level, const std::wstring& msg)
{
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (!g_enabled || g_path.empty()) return;
    struct _stat64 st{};
    if (_wstat64(g_path.c_str(), &st) == 0 && st.st_size > 256 * 1024) {
        // 简单截断：保留后半段（新日志在前，旧日志滚掉）。
        std::wstring tmp = g_path + L".trunc";
        _wremove(tmp.c_str());
        if (MoveFileExW(g_path.c_str(), tmp.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            HANDLE h = CreateFileW(tmp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            std::wstring tail;
            if (h != INVALID_HANDLE_VALUE) {
                DWORD hi = 0, lo = GetFileSize(h, &hi);
                ULONGLONG size = (static_cast<ULONGLONG>(hi) << 32) | lo;
                ULONGLONG keep = size > 128 * 1024 ? 128 * 1024 : 0;
                if (keep) {
                    LARGE_INTEGER off;
                    off.QuadPart = static_cast<LONGLONG>(keep);
                    SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
                }
                char buf[8192];
                DWORD got = 0;
                while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
                    tail += Utf8ToWide(std::string(buf, got));
                CloseHandle(h);
            }
            HANDLE w2 = CreateFileW(g_path.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (w2 != INVALID_HANDLE_VALUE) {
                std::string cut = WideToUtf8(L"--- 日志超长截断 ---\n" + tail);
                DWORD written = 0;
                WriteFile(w2, cut.data(), static_cast<DWORD>(cut.size()), &written, nullptr);
                CloseHandle(w2);
            }
            _wremove(tmp.c_str());
        }
    }
    SYSTEMTIME systm{};
    GetLocalTime(&systm);
    wchar_t head[64];
    swprintf(head, 64, L"[%02d:%02d:%02d] %ls ", systm.wHour, systm.wMinute, systm.wSecond, level.c_str());
    std::string line = WideToUtf8(std::wstring(head) + msg + L"\n");
    HANDLE h = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(h);
}

} // namespace wb2
