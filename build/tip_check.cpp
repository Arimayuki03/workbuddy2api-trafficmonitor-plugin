// tip_check.cpp — 验证 tooltip 预算收敛：加载 DLL、真轮询、打印全文与长度。
#include "common.h"
#include "PluginInterface.h"
#include <cstdio>
#include <thread>
#include <chrono>

int main()
{
    HMODULE h = LoadLibraryExA("WorkBuddy2ApiPlugin.dll", nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { printf("LoadLibrary fail %lu\n", GetLastError()); return 2; }
    auto gi = (ITMPlugin * (*)())GetProcAddress(h, "TMPluginGetInstance");
    if (!gi) { printf("GetProcAddress fail %lu\n", GetLastError()); return 2; }
    ITMPlugin* p = gi();
    if (!p) { printf("TMPluginGetInstance returned null\n"); return 2; }
    CreateDirectoryW(L"tip_cfg", nullptr);
    p->OnExtenedInfo(ITMPlugin::EI_CONFIG_DIR, L"tip_cfg");
    p->OnInitialize(nullptr);
    p->DataRequired();
    for (int i = 0; i < 8; i++) std::this_thread::sleep_for(std::chrono::seconds(1));
    const wchar_t* tip = p->GetTooltipInfo();
    size_t len = wcslen(tip);
    wprintf(L"=== len=%zu ===\n%ls\n=== END ===\n", len, tip);
    // 阈值 = 生产 kTipBudget（worker.cpp），改动预算时同步这里
    if (len <= 700) printf("PASS (<=700)\n");
    else printf("CHECK: over budget, inspect content above\n");
    return 0;
}
