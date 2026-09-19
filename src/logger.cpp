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
        // 起点按行对齐 + 尾部整体转码：起点若切在多字节 UTF-8 字符中间、或按块转码把
        // 多字节序列从块边界切开，宽容转码只会以丢弃坏序列收场，旧日志被无谓搞乱。
        std::wstring tmp = g_path + L".trunc";
        _wremove(tmp.c_str());
        if (MoveFileExW(g_path.c_str(), tmp.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            HANDLE h = CreateFileW(tmp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            std::string tail_bytes;
            if (h != INVALID_HANDLE_VALUE) {
                DWORD hi = 0, lo = GetFileSize(h, &hi);
                ULONGLONG size = (static_cast<ULONGLONG>(hi) << 32) | lo;
                ULONGLONG keep = size > 128 * 1024 ? 128 * 1024 : 0;
                if (keep) {
                    // 起点按行对齐：从 128KB 处向前小步扫到下一个 '\n'（最多扫 8KB），
                    // 从行首开始保留；找不到行首（如超长单行）就退回原位置兜底。
                    const ULONGLONG start = keep;
                    LARGE_INTEGER off;
                    off.QuadPart = static_cast<LONGLONG>(start);
                    SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
                    char probe[512];
                    ULONGLONG scanned = 0;
                    bool aligned = false;
                    while (scanned < 8 * 1024) {
                        DWORD got = 0;
                        if (!ReadFile(h, probe, sizeof probe, &got, nullptr) || got == 0) break;
                        for (DWORD i = 0; i < got && !aligned; ++i) {
                            if (probe[i] == '\n') {
                                keep = start + scanned + i + 1;
                                aligned = true;
                            }
                        }
                        if (aligned) break;
                        scanned += got;
                    }
                    off.QuadPart = static_cast<LONGLONG>(keep);
                    SetFilePointerEx(h, off, nullptr, FILE_BEGIN); // 已对齐→行首；未找到→回到原起点
                }
                // 尾部一次性转码：先把字节攒齐，读完整体 Utf8ToWide，块边界不再切开字符。
                char buf[8192];
                DWORD got = 0;
                while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
                    tail_bytes.append(buf, got);
                CloseHandle(h);
            }
            std::wstring tail = Utf8ToWide(tail_bytes);
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
