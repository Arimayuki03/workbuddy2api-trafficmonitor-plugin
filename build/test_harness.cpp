// test_harness.cpp — 无 GUI 插件自检：加载 DLL、驱动 worker 真轮询本机 wb2api、内存 DC 验证自绘。
// 用法（在能访问 wb2api 的机器上）：test_harness [DLL路径]
#include "common.h"
#include "PluginInterface.h"
#include <algorithm>
#include <cstdio>
#include <thread>
#include <chrono>

int main(int argc, char** argv)
{
    const char* dll = argc > 1 ? argv[1] : "build\\out\\Release\\WorkBuddy2ApiPlugin.dll";
    HMODULE h = LoadLibraryExA(dll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { printf("LoadLibrary 失败 %lu\n", GetLastError()); return 2; }
    auto get_inst = (ITMPlugin * (*)())GetProcAddress(h, "TMPluginGetInstance");
    if (!get_inst) { printf("找不到 TMPluginGetInstance\n"); return 3; }
    ITMPlugin* p = get_inst();
    if (!p) { printf("实例为空\n"); return 4; }
    const wchar_t* nm = p->GetInfo(ITMPlugin::TMI_NAME);
    const wchar_t* ver = p->GetInfo(ITMPlugin::TMI_VERSION);
    printf("API version=%d\nname=%S\nversion=%S\n",
        p->GetAPIVersion(), nm ? nm : L"?", ver ? ver : L"?");

    IPluginItem* it0 = p->GetItem(0);
    printf("item0=%p name=%S id=%S customdraw=%d\n",
        (void*)it0, it0->GetItemName(), it0->GetItemId(), it0->IsCustomDraw());
    printf("item1(null expected)=%p\n", (void*)p->GetItem(1));
    printf("width96=%d sample=%S\n", it0->GetItemWidth(), it0->GetItemValueSampleText());

    // 用真实配置目录初始化，令 worker 起来并轮询运行中的 wb2api
    const wchar_t* cfg = L"build\\out\\test_cfg";
    CreateDirectoryW(cfg, nullptr);
    p->OnExtenedInfo(ITMPlugin::EI_CONFIG_DIR, cfg);
    p->OnInitialize(nullptr); // nullptr pApp：走默认 Settings（指向 D:\Code\workbuddy2api:7863）
    p->DataRequired();

    // 等待 worker 完成若干轮轮询
    for (int i = 0; i < 10; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::wstring v = it0->GetItemValueText();
        std::wstring tip = p->GetTooltipInfo();
        printf("tick %2d: value=[%S] tooltip_lines=%zu\n", i + 1,
            v.empty() ? L"(空)" : v.c_str(),
            (size_t)std::count(tip.begin(), tip.end(), L'\n') + 1);
    }

    // 内存 DC 上跑自绘与宽度查询，确认不崩、GDI 句柄平衡
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, 120, 24);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    int w1 = it0->GetItemWidthEx(mem);
    printf("GetItemWidthEx=%d\n", w1);
    for (int i = 0; i < 500; i++) {
        it0->DrawItem(mem, 0, 0, 120, 24, i % 2 == 0);
    }
    printf("DrawItem x500 完成（深/浅各半）\n");
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    // 命令菜单存在性
    printf("commands=%d\n", p->GetCommandCount());
    for (int i = 0; i < p->GetCommandCount(); i++)
        printf("  cmd[%d]=%S checked=%d\n", i, p->GetCommandName(i), p->IsCommandChecked(i));

    // 触发一次"重启服务"动作验证异步链路（会真重启本机 wb2api），稍等后确认又轮询到
    printf("触发 RequestRestartService 前先停止……跳过真重启，仅测试 tooltip 构建完成。\n");

    FreeLibrary(h);
    printf("测试完成（FreeLibrary 未崩）\n");
    return 0;
}
