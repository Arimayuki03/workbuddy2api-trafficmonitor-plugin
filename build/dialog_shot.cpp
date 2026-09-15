// dialog_shot.cpp — 打开设置对话框，逐 tab 截图存 BMP（外部转 PNG 目测）。
#include "common.h"
#include "PluginInterface.h"
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

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
    Sleep(2500);

    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        HWND dlg = nullptr;
        for (int i = 0; i < 200 && !(dlg = FindWindowW(nullptr, L"WorkBuddy2API 插件设置")); i++) Sleep(50);
        if (!dlg) { puts("no dialog"); return 1; }
        Sleep(1200);
        HWND tab = FindWindowExW(dlg, nullptr, L"SysTabControl32", nullptr);
        int n = tab ? TabCtrl_GetItemCount(tab) : 0;
        printf("tabs=%d", n);
        for (int i = 0; i < n; i++) {
            TabCtrl_SetCurSel(tab, i);
            NMHDR nh{}; nh.hwndFrom = tab; nh.idFrom = GetDlgCtrlID(tab); nh.code = TCN_SELCHANGE;
            SendMessageW(dlg, WM_NOTIFY, nh.idFrom, (LPARAM)&nh);
            Sleep(700);
            wchar_t fn[64]; wsprintf(fn, L"shot_tab%d.bmp", i);
            SaveBmp(dlg, fn);
            printf("shot %d", i);
        }
        Sleep(500);
        PostMessageW(dlg, WM_CLOSE, 0, 0);
        return 0;
    }, nullptr, 0, nullptr);

    g_p->ShowOptionsDialog(nullptr);
    Sleep(500);
    ExitProcess(0);
    return 0;
}
