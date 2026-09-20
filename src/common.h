// common.h — 全局小工具：编码转换、字符串处理、数值格式化。
// 约定：进程内边界一律 std::wstring（UTF-16），网络/文件边界一律 UTF-8。
// workbuddy2api 的 nickname、TrafficMonitor 的中文路径（如 E:\软件\）都在
// 各自边界处显式转换，绝不经过 ANSI 代码页。
#pragma once

// 构建脚本已全局定义同名宏，加 ifndef 防 C4005 双定义告警
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace wb2 {

// UTF-8 → UTF-16。非法字节以 U+FFFD 代替（不抛异常）。
std::wstring Utf8ToWide(const std::string& s);
// UTF-16 → UTF-8。
std::string WideToUtf8(const std::wstring& s);

inline std::wstring TrimW(std::wstring s)
{
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 大数压缩显示：>=100000 → "120k"；>=10000 → "56.3k"（一位小数、去尾 .0）；其余原值。
// 任务栏空间有限，积分统一走这里；tooltip 用原始千分位。
std::wstring FormatCreditsCompact(int64_t v);
// 千分位（tooltip 用）。
std::wstring FormatThousands(int64_t v);
// 浮点积分（/v1/stats credit）：整值千分位不带小数，小值 2 位有效去尾零。
std::wstring FormatCreditNum(double v);

// 时间戳（unix 秒）→ 本地 HH:mm / MM-dd HH:mm；0 → L"-"。
std::wstring FormatTimeShort(int64_t unix_sec);
// RFC3339 字符串（如 "2026-09-16T09:00:00+08:00"）→ 本地 HH:mm；失败返回原串。
std::wstring FormatRfc3339Short(const std::string& rfc3339);

// 宽字符 snprintf 封装。
std::wstring WideFormat(LPCWSTR fmt, ...);

} // namespace wb2
