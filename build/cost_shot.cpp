// cost_shot.cpp — 临时验证工具：打开设置窗 → 页② 选中账户行 → 发 LVN_ITEMACTIVATE
// （等价双击）→ 截"成本台账"弹窗 → 点确定关闭。验证 model_costs 解析与弹窗路径。
#include "common.h"
#include "PluginInterface.h"
#include <commctrl.h>
#include <cstdio>
#include <vector>

static ITMPlugin* g_p;

static void SaveBmp(HWND dlg, const wchar_t* out) {
    RECT rc{}; GetWindowRect(dlg, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) { puts("bad size"); return; }
    HDC sdc = GetDC(dlg);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, w, h, sdc, 0, 0, SRCCOPY);
    ReleaseDC(dlg, sdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
    int stride = (w * 3 + 3) & ~3;
    std::vector<BYTE> pix((size_t)stride * h);
    int got = GetDIBits(mdc, bmp, 0, h, pix.data(), &bi, DIB_RGB_COLORS);
    SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc);
    if (!got) { puts("GetDIBits fail"); return; }
    BITMAPFILEHEADER bf{};
    DWORD img = (DWORD)stride * h;
    bf.bfType = 0x4D42; bf.bfOffBits = (DWORD)(sizeof bf + sizeof(BITMAPINFOHEADER)); bf.bfSize = bf.bfOffBits + img;
    BITMAPINFOHEADER bh{};
    bh.biSize = sizeof bh; bh.biWidth = w; bh.biHeight = -h; bh.biPlanes = 1; bh.biBitCount = 24; bh.biCompression = BI_RGB;
    FILE* fp = _wfopen(out, L"wb");
    if (!fp) { puts("open fail"); return; }
    fwrite(&bf, sizeof bf, 1, fp); fwrite(&bh, sizeof bh, 1, fp); fwrite(pix.data(), 1, img, fp);
    fclose(fp);
    puts("saved");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* dll = argc > 1 ? argv[1] : "WorkBuddy2ApiPlugin.dll";
    HMODULE h = LoadLibraryExA(dll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { printf("LoadLibrary failed %lu", GetLastError()); return 2; }
    g_p = ((ITMPlugin * (*)())GetProcAddress(h, "TMPluginGetInstance"))();
    CreateDirectoryW(L"shot_cfg", nullptr);
    g_p->OnExtenedInfo(ITMPlugin::EI_CONFIG_DIR, L"shot_cfg");
    g_p->OnInitialize(nullptr);
    Sleep(3000);

    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        HWND dlg = nullptr;
        for (int i = 0; i < 200 && !(dlg = FindWindowW(nullptr, L"WorkBuddy2API 插件设置")); i++) Sleep(50);
        if (!dlg) { puts("no dialog"); return 1; }
        Sleep(800);
        HWND tab = FindWindowExW(dlg, nullptr, L"SysTabControl32", nullptr);
        TabCtrl_SetCurSel(tab, 1);
        NMHDR nh{}; nh.hwndFrom = tab; nh.idFrom = GetDlgCtrlID(tab); nh.code = TCN_SELCHANGE;
        SendMessageW(dlg, WM_NOTIFY, nh.idFrom, (LPARAM)&nh);
        Sleep(800);
        HWND list = FindWindowExW(dlg, nullptr, L"SysListView32", nullptr);
        if (!list) { puts("no list"); return 1; }
        // 选中第 0 行（Ashley——实测有 model_costs）
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        NMHDR act{}; act.hwndFrom = list; act.idFrom = GetDlgCtrlID(list); act.code = LVN_ITEMACTIVATE;
        PostMessageW(dlg, WM_NOTIFY, act.idFrom, (LPARAM)&act);
        HWND box = nullptr;
        for (int i = 0; i < 100 && !(box = FindWindowW(L"#32770", L"成本台账")); i++) Sleep(50);
        if (!box) { puts("no cost box"); return 1; }
        Sleep(300);
        SaveBmp(box, L"cost_box.bmp");
        PostMessageW(box, WM_COMMAND, IDOK, 0);
        Sleep(300);
        PostMessageW(dlg, WM_CLOSE, 0, 0);
        return 0;
    }, nullptr, 0, nullptr);

    g_p->ShowOptionsDialog(nullptr);
    Sleep(500);
    ExitProcess(0);
    return 0;
}
