// logger.h — 极简文件日志（config 目录 WorkBuddy2ApiPlugin.log，256KB 滚动截断）。
// 只在 worker/动作线程里写；DrawItem 等 UI 路径禁止调用。
#pragma once
#include <string>

namespace wb2 {

void LogInit(const std::wstring& path, bool enabled);
void Log(const std::wstring& level, const std::wstring& msg);

inline void LogI(const std::wstring& m) { Log(L"INFO", m); }
inline void LogW(const std::wstring& m) { Log(L"WARN", m); }
inline void LogE(const std::wstring& m) { Log(L"ERR ", m); }

} // namespace wb2
