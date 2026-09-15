// dialogs.h — 设置对话框（5 个 tab 页）。模态；返回是否发生设置变更。
#pragma once
#include "PluginInterface.h"
#include <windows.h>

namespace wb2::dlg {

ITMPlugin::OptionReturn ShowSettingsDialog(HWND parent);

} // namespace wb2::dlg
