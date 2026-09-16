// common.cpp — 编码/格式化助手实现。
#include "common.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cwchar>

namespace wb2 {

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
        static_cast<int>(s.size()), nullptr, 0);
    if (wlen <= 0) {
        // 严格模式失败（上游给了坏字节）：退宽容模式，坏序列被丢弃而不是整串变空。
        wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (wlen <= 0) return std::wstring();
    }
    std::wstring out(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), wlen);
    return out;
}

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring FormatCreditsCompact(int64_t v)
{
    uint64_t a = v < 0 ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);
    if (a < 10000) return std::to_wstring(v);
    // k 表示法：一位小数，整倍去尾（56.0k→56k）。不带空格——
    // 与 GetItemValueSampleText("-88.8k") 一致，保证宽度预留准确。
    long double k = static_cast<long double>(v) / 1000.0L;
    wchar_t buf[32];
    if (a >= 100000)
        swprintf(buf, 32, L"%.0Lfk", k);
    else {
        swprintf(buf, 32, L"%.1Lfk", k);
    }
    std::wstring s(buf);
    // "56.0k" → "56k"
    if (s.size() > 4 && s.compare(s.size() - 3, 3, L".0k") == 0)
        s.replace(s.size() - 3, 2, L"");
    return s;
}

std::wstring FormatThousands(int64_t v)
{
    bool neg = v < 0;
    uint64_t a = neg ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);
    wchar_t raw[32];
    swprintf(raw, 32, L"%llu", static_cast<unsigned long long>(a));
    std::wstring digits(raw), out;
    int cnt = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        out.push_back(*it);
        if (++cnt % 3 == 0 && it + 1 != digits.rend()) out.push_back(L',');
    }
    if (neg) out.push_back(L'-');
    std::reverse(out.begin(), out.end());
    return out;
}

std::wstring FormatTimeShort(int64_t unix_sec)
{
    if (unix_sec <= 0) return L"-";
    time_t t = static_cast<time_t>(unix_sec);
    struct tm lt{};
    localtime_s(&lt, &t);
    wchar_t buf[32];
    swprintf(buf, 32, L"%02d:%02d", lt.tm_hour, lt.tm_min);
    return buf;
}

std::wstring FormatRfc3339Short(const std::string& rfc)
{
    // "2026-09-16T09:00:00+08:00" → 提取 HH:mm（本时区一致性由上游保证：wb2api 用
    // 本地整点排程，输出带本地偏移，直接取 T 后 5 字符即用户墙上时间）。
    size_t tpos = rfc.find('T');
    if (tpos == std::string::npos || rfc.size() < tpos + 6) return Utf8ToWide(rfc);
    return Utf8ToWide(rfc.substr(tpos + 1, 5));
}

std::wstring WideFormat(LPCWSTR fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    wchar_t buf[512];
    int n = _vsnwprintf_s(buf, 512, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return L"";
    // _TRUNCATE 截断时 _vsnwprintf_s 返回"本应写入"的长度（可能 >= 512），
    // 直接用它会越界读栈缓冲区（/GS 快速失败）。夹到实际可容纳的 511。
    if (n > 511) n = 511;
    return std::wstring(buf, n);
}

} // namespace wb2
