// item.cpp — 自绘实现。要点：
//  * DrawItem 由 TM 高频调用：零网络、零分配大对象，只取预生成字符串 + 轻量状态查询；
//  * 字体跟随宿主 DC 当前字体（任务栏/主窗口字号由 TM 决定），缓存句柄只重建一次；
//  * 宽度按最长样例（"88/88 · 8.8k"）预留 → 数值变化不引起任务栏宽度抖动。
#include "item.h"
#include "common.h"
#include "plugin.h"
#include "worker.h"
#include <algorithm>

namespace wb2 {

StatusItem& StatusItem::Instance()
{
    static StatusItem inst;
    return inst;
}

const wchar_t* StatusItem::GetItemName() const { return L"WB2API 服务状态"; }
const wchar_t* StatusItem::GetItemId() const { return L"WB2API_STATUS"; }
const wchar_t* StatusItem::GetItemLableText() const { return L"WB2API"; }
const wchar_t* StatusItem::GetItemValueSampleText() const { return L"88/88 · 8.8k"; }

const wchar_t* StatusItem::GetItemValueText() const
{
    std::wstring v = Worker::Instance().DisplayValue();
    std::lock_guard<std::mutex> lk(val_mu_);
    val_cache_ = std::move(v);
    return val_cache_.c_str();
}

namespace {

// 状态点颜色：浅/深色模式各一套（在黑色任务栏文字旁可辨识为基准）。
COLORREF DotColor(SvcState s, bool dark)
{
    switch (s) {
    case SvcState::Running:     return dark ? RGB(70, 220, 100) : RGB(16, 130, 40);
    case SvcState::Unservable:  return dark ? RGB(255, 180, 60) : RGB(215, 130, 0);
    case SvcState::Starting:    return dark ? RGB(90, 170, 255) : RGB(30, 110, 220);
    case SvcState::WrongService:
    case SvcState::Dead:        return dark ? RGB(255, 95, 95) : RGB(200, 30, 30);
    case SvcState::Stopped:     return dark ? RGB(130, 130, 130) : RGB(160, 160, 160);
    default:                    return dark ? RGB(150, 150, 150) : RGB(140, 140, 140);
    }
}

} // namespace

HFONT StatusItem::FontFor(HDC dc) const
{
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    int h = -(int)tm.tmHeight;
    // TEXTMETRIC 新 SDK 已无 tmFaceName：字体名改走 GetTextFaceW。
    wchar_t facebuf[LF_FACESIZE]{};
    std::wstring face;
    if (GetTextFaceW(dc, LF_FACESIZE, facebuf) > 0) face = facebuf;
    if (face.empty()) face = L"Microsoft YaHei UI";
    std::lock_guard<std::mutex> lk(font_mu_);
    if (!font_ || font_height_ != h || font_face_ != face) {
        if (font_) DeleteObject(font_);
        LOGFONTW lf{};
        lf.lfHeight = h;
        lf.lfWeight = tm.tmWeight ? tm.tmWeight : FW_REGULAR;
        lf.lfCharSet = GB2312_CHARSET; // 中文任务栏（微软雅黑系）
        wcsncpy_s(lf.lfFaceName, face.c_str(), _TRUNCATE);
        font_ = CreateFontIndirectW(&lf);
        font_height_ = h;
        font_face_ = face;
    }
    return font_;
}

int StatusItem::GetItemWidth() const
{
    // 96dpi 兜底路径：样例 9 字符 × ~8px + 点 13 + 边距 ≈ 88
    return 88;
}

int StatusItem::GetItemWidthEx(void* hDC) const
{
    HDC dc = static_cast<HDC>(hDC);
    HFONT old = static_cast<HFONT>(SelectObject(dc, FontFor(dc)));
    SIZE sz{};
    LPCWSTR sample = GetItemValueSampleText();
    GetTextExtentPoint32W(dc, sample, lstrlenW(sample), &sz);
    SelectObject(dc, old);
    return sz.cx + 13 /*点8+间隙5*/ + 4;
}

void StatusItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
{
    HDC dc = static_cast<HDC>(hDC);
    std::wstring text = Worker::Instance().DisplayValue();
    SvcState st = Worker::Instance().State();

    SaveDC(dc);
    HFONT old_font = static_cast<HFONT>(SelectObject(dc, FontFor(dc)));
    SetBkMode(dc, TRANSPARENT);

    int d = std::max(5, std::min(9, h * 3 / 8));
    int cx = x + 1 + d / 2, cy = y + h / 2;
    COLORREF c = DotColor(st, dark_mode);
    HPEN pen = static_cast<HPEN>(GetStockObject(NULL_PEN));
    HBRUSH br = CreateSolidBrush(c);
    HPEN op = static_cast<HPEN>(SelectObject(dc, pen));
    HBRUSH ob = static_cast<HBRUSH>(SelectObject(dc, br));
    Ellipse(dc, cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(br);

    COLORREF txt = CPluginApp::Instance().ValueTextColor(dark_mode);
    SetTextColor(dc, txt);
    RECT rc{ x + d + 5, y, x + w - 1, y + h };
    DrawTextW(dc, text.c_str(), -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(dc, old_font);
    RestoreDC(dc, -1);
}

int StatusItem::OnMouseEvent(MouseEventType type, int, int, void* hWnd, int)
{
    switch (type) {
    case MT_LCLICKED:
        CPluginApp::Instance().OpenSettings(static_cast<HWND>(hWnd));
        return 1;
    case MT_DBCLICKED:
        return 1; // 单击已处理，吞掉双击避免弹两个窗
    default:
        return 0; // 右键/滚轮交还主程序（TM 菜单）
    }
}

} // namespace wb2
