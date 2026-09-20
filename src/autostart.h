// autostart.h — "随开机启动"实现：注册 Windows 计划任务（登录触发），
// 动作是插件生成的隐藏启动器 VBS（自带 10 秒延迟 + 工作目录 + 隐藏窗口）。
// 全部走 schtasks.exe 命令行（当前用户任务，无需管理员）。
#pragma once
#include <string>
#include <atomic>
#include <windows.h>

namespace wb2::autostart {

const wchar_t* TaskName(); // L"WorkBuddy2API-Service"

// 是否已安装本插件注册的任务（schtasks /Query 退出码）。
bool IsInstalled(const std::atomic<bool>* cancel = nullptr);
// 生成/更新 VBS 并 /Create /F 覆盖注册。VBS 里写死当前 service_dir/port 对应路径。
// cancel（可空）：schtasks 等待期间轮询，置位后放弃等待尽快返回。
bool Install(std::wstring& err, const std::atomic<bool>* cancel = nullptr);
// /Delete /F（容忍"任务不存在"）。
bool Uninstall(std::wstring& err, const std::atomic<bool>* cancel = nullptr);

} // namespace wb2::autostart
