// tip_meas.cpp — 对比新旧 DLL 在同一真实数据下的 tooltip 长度。
#include "common.h"
#include "PluginInterface.h"
#include <cstdio>
#include <thread>
#include <chrono>

static void Measure(const char* dll_path, const char* tag)
{
    HMODULE h = LoadLibraryExA(dll_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { printf("%s: LoadLibrary fail %lu\n", tag, GetLastError()); return; }
    auto gi = (ITMPlugin * (*)())GetProcAddress(h, "TMPluginGetInstance");
    if (!gi) { printf("%s: GetProcAddress fail %lu\n", tag, GetLastError()); return; }
    ITMPlugin* p = gi();
    if (!p) { printf("%s: TMPluginGetInstance returned null\n", tag); return; }
    char cfg[MAX_PATH];
    sprintf(cfg, "meas_cfg_%s", tag);
    wchar_t wcfg[MAX_PATH];
    mbstowcs(wcfg, cfg, MAX_PATH);
    CreateDirectoryW(wcfg, nullptr);
    p->OnExtenedInfo(ITMPlugin::EI_CONFIG_DIR, wcfg);
    p->OnInitialize(nullptr);
    p->DataRequired();
    for (int i = 0; i < 8; i++) std::this_thread::sleep_for(std::chrono::seconds(1));
    const wchar_t* tip = p->GetTooltipInfo();
    size_t len = wcslen(tip);
    size_t lines = 1;
    for (const wchar_t* q = tip; *q; q++) if (*q == L'\n') lines++;
    wprintf(L"=== %hs: len=%zu lines=%zu ===\n", tag, len, lines);
    size_t lno = 1, lstart = 0;
    for (size_t i = 0; i <= len; i++) {
        if (tip[i] == L'\n' || tip[i] == 0) {
            size_t end = i;
            if (end > lstart && tip[end - 1] == L'\r') end--; // 行长不含 \r\n 分隔符
            if (end > lstart)
                wprintf(L"  [%2zu] %3zu %.*ls\n", lno, end - lstart, (int)(end - lstart), tip + lstart);
            lno++; lstart = i + 1;
        }
    }
    printf("(free not called; process exits)\n");
}

int main()
{
    Measure("old150.dll", "v1.5.0");
    Measure("new151.dll", "v1.5.1");
    printf("done\n");
    return 0;
}
