// trace.cpp — 排查辅助：环境变量门控的接口调用追踪 + 向量化异常记录。
// WB2API_TRACE=1 时启用；生产路径零开销（一个原子读）。
#include "trace.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <memory>

namespace wb2::trace {

namespace {
std::mutex g_mu;
HANDLE g_file = nullptr;
unsigned long long g_seq = 0;

void OpenLocked()
{
    wchar_t path[MAX_PATH * 2]{};
    // 写到 TM 目录旁的固定位置，避免依赖配置目录初始化时序
    const wchar_t* tmp = _wgetenv(L"WB2API_TRACE_PATH");
    if (tmp && *tmp) {
        g_file = CreateFileW(tmp, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return;
    }
    const wchar_t* dir = _wgetenv(L"WB2API_TRACE_DIR");
    if (!dir || !*dir) dir = L".";
    swprintf(path, MAX_PATH * 2, L"%ls\\wb2api_trace.log", dir);
    g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool EnabledUnlocked()
{
    return g_file != nullptr && g_file != INVALID_HANDLE_VALUE;
}
} // namespace

bool Active()
{
    std::lock_guard<std::mutex> lk(g_mu);
    return EnabledUnlocked();
}

void Init()
{
    wchar_t flag[8]{};
    size_t n = 0;
    _wgetenv_s(&n, flag, 8, L"WB2API_TRACE");
    if (n == 0 || flag[0] != L'1') return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) return;
    OpenLocked();
    if (EnabledUnlocked()) {
        // 直接写首行（Write 会再加锁，这里已持锁，手动展开）
        unsigned long long t = GetTickCount64();
        char line[128];
        int m = snprintf(line, sizeof line, "[%llu.%03llu] #0 trace init\r\n", t / 1000, t % 1000);
        if (m > 0) {
            DWORD written = 0;
            WriteFile(g_file, line, static_cast<DWORD>(m), &written, nullptr);
        }
    }
}

void Write(const char* what)
{
    // 不在锁外裸读 g_file（与 Init 的写构成数据竞争）；持锁后用 EnabledUnlocked() 判断，
    // 与 ExceptRecord/StackRecord 的写法一致。
    std::lock_guard<std::mutex> lk(g_mu);
    if (!EnabledUnlocked()) return;
    unsigned long long t = GetTickCount64();
    char line[256];
    int n = snprintf(line, sizeof line, "[%llu.%03llu] #%llu %s\r\n",
        t / 1000, t % 1000, ++g_seq, what);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
    }
}

bool TryWrite(const char* what)
{
    // VEH 上下文专用：异常可能落在另一线程持有 g_mu 的临界区内，阻塞 lock() 会自死锁。
    std::unique_lock<std::mutex> lk(g_mu, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    if (!EnabledUnlocked()) return false;
    unsigned long long t = GetTickCount64();
    char line[256];
    int n = snprintf(line, sizeof line, "[%llu.%03llu] #%llu %s\r\n",
        t / 1000, t % 1000, ++g_seq, what);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
    }
    return true;
}

void ExceptRecord(const char* where, unsigned long code, void* addr)
{
    std::lock_guard<std::mutex> lk(g_mu);
    if (!EnabledUnlocked()) return;
    char line[256];
    int n = snprintf(line, sizeof line, "!!! EXCEPTION code=0x%08lX at %p in %s\r\n",
        code, addr, where ? where : "?");
    if (n > 0) {
        DWORD written = 0;
        WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
        FlushFileBuffers(g_file);
    }
}

void StackRecord(void* const* frames, unsigned count)
{
    std::lock_guard<std::mutex> lk(g_mu);
    if (!EnabledUnlocked()) return;
    {
        char head[64];
        int hn = snprintf(head, sizeof head, "  stack frames=%u\r\n", count);
        if (hn > 0) {
            DWORD hw = 0;
            WriteFile(g_file, head, static_cast<DWORD>(hn), &hw, nullptr);
        }
    }
    for (unsigned i = 0; i < count; i++) {
        HMODULE hm = nullptr;
        char mod[MAX_PATH] = "?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(frames[i]), &hm) && hm) {
            GetModuleFileNameA(hm, mod, MAX_PATH);
            const char* slash = strrchr(mod, '\\');
            char name[MAX_PATH];
            _snprintf_s(name, sizeof name, _TRUNCATE, "%s", slash ? slash + 1 : mod);
            uintptr_t base = reinterpret_cast<uintptr_t>(hm);
            uintptr_t pc = reinterpret_cast<uintptr_t>(frames[i]);
            char line[320];
            int n = snprintf(line, sizeof line, "  frame[%u] %s+0x%llX (%p)\r\n",
                i, name, static_cast<unsigned long long>(pc - base), frames[i]);
            if (n > 0) {
                DWORD written = 0;
                WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
            }
        }
    }
    FlushFileBuffers(g_file);
}

} // namespace wb2::trace
