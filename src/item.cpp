// item.cpp — 自绘实现。要点：
//  * DrawItem 由 TM 高频调用：零网络、零分配大对象，只取预生成字符串 + 轻量状态查询；
//  * 字体直接克隆宿主 DC 当前字体（CDrawCommon::SetFont 已把任务栏字体选进 DC），
//    保证与相邻栏位字号/字重一致；TM 换字体时按 (高,字重,字面) 缓存键重建；
//  * 宽度按"当前显示模式下实际会出现的最长值"预留 → 数值变化不引起任务栏宽度抖动。
#include "item.h"
#include "common.h"
#include "plugin.h"
#include "settings.h"
#include "trace.h"
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

// 宽度预留样例：长度必须 >= 对应显示模式下 BuildDisplayLocked 实际产出的任何值
// （Running 态 "h/t" 最多 "88/88"，积分态最长 "88.8k"/"-88.8k"，其余为短状态词）。
// 不再混入旧的 "· 8.8k" 合并格式——那会让任务栏栏位常年多出一段空白。
const wchar_t* StatusItem::GetItemValueSampleText() const
{
    switch (SettingsStore::Instance().Get().show_mode) {
    case SM_STATE_CREDITS: return L"-88.8k"; // 积分态最长样例（含负号）
    case SM_STATE_ONLY:    return L"无响应";  // 最长状态词（4 个全角）
    default:               return L"88/88";  // 健康/总数
    }
}

const wchar_t* StatusItem::GetItemValueText() const
{
    WB2API_TRACE_LOG("item.GetValueText");
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
    // TM 绘制插件项前已把任务栏字体选进 DC（CDrawCommon::SetFont → SelectObject），
    // 直接克隆它的 LOGFONT：字号/字重/字面与相邻栏位逐位一致。
    // （旧实现按 -tmHeight 重建：tmHeight 含 internal leading，字号恒比宿主大一号。）
    LOGFONTW lf{};
    if (HFONT host = static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT)); host &&
        GetObjectW(host, sizeof lf, &lf) && lf.lfFaceName[0] != L'\0') {
        ; // 克隆成功
    } else {
        // DC 上没有可用字体（异常路径兜底）：按雅黑 + 当前字号重建。
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        lf = {};
        lf.lfHeight = -tm.tmHeight;
        lf.lfWeight = FW_REGULAR;
        lf.lfCharSet = GB2312_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    }
    std::lock_guard<std::mutex> lk(font_mu_);
    if (!font_ || font_height_ != lf.lfHeight || font_weight_ != lf.lfWeight ||
        font_face_ != std::wstring(lf.lfFaceName)) {
        if (font_) DeleteObject(font_);
        font_ = CreateFontIndirectW(&lf);
        font_height_ = lf.lfHeight;
        font_weight_ = lf.lfWeight;
        font_face_ = lf.lfFaceName;
    }
    return font_;
}

int StatusItem::GetItemWidth() const
{
    // 96dpi 兜底路径（仅当宿主 API<3 或 WidthEx 返回 0 时被用到）：
    // 样例 5 字符 × ~8px + 点 13 + 边距 ≈ 57
    return 57;
}

int StatusItem::GetItemWidthEx(void* hDC) const
{
    WB2API_TRACE_LOG("item.WidthEx");
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
    WB2API_TRACE_LOG("item.DrawItem");
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
    WB2API_TRACE_LOG("item.OnMouseEvent");
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
