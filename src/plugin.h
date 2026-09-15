// plugin.h — 插件主类（ITMPlugin 实现，进程内唯一实例，TM 退出前不释放——符合接口约定）。
#pragma once
#include "common.h"       // windows.h（HWND/COLORREF/HMODULE）
#include "PluginInterface.h"
#include <mutex>
#include <string>

namespace wb2 {

class CPluginApp : public ITMPlugin {
public:
    static CPluginApp& Instance();

    // —— ITMPlugin ——
    IPluginItem* GetItem(int index) override;
    void DataRequired() override;                       // worker 自带线程；这里只保证已启动
    OptionReturn ShowOptionsDialog(void* hParent) override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    const wchar_t* GetTooltipInfo() override;
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;
    void OnInitialize(ITrafficMonitor* pApp) override;
    // 命令菜单（插件管理里出现的快捷动作 + 任务勾选）
    int GetCommandCount() override;
    const wchar_t* GetCommandName(int command_index) override;
    void OnPluginCommand(int command_index, void* hWnd, void* para) override;
    int IsCommandChecked(int command_index) override;

    // —— 供 item/dialog 使用 ——
    void OpenSettings(HWND parent);                     // 任务栏单击入口
    COLORREF ValueTextColor(bool dark_mode) const;      // EI_VALUE_TEXT_COLOR 或深浅色默认
    ITrafficMonitor* App() const { return app_; }

private:
    CPluginApp() = default;
    void EnsureInited();                                // 配置目录就绪 + worker 启动 + TM 启动跟随

    ITrafficMonitor* app_ = nullptr;
    COLORREF label_color_ = RGB(0, 0, 0), value_color_ = RGB(0, 0, 0);
    bool colors_set_ = false;
    std::once_flag init_once_;
    std::mutex tt_mu_;
    std::wstring tt_cache_;
};

} // namespace wb2
