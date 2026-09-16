// item.h — 任务栏自绘单项：彩色状态点 + 短文本（宽度按最长样例预留，防任务栏抖动）。
#pragma once
#include "common.h"       // windows.h（HFONT/HDC）
#include "PluginInterface.h"
#include <mutex>
#include <string>

namespace wb2 {

class StatusItem : public IPluginItem {
public:
    static StatusItem& Instance();

    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;          // 非自绘宿主的兜底路径
    const wchar_t* GetItemValueSampleText() const override;
    bool IsCustomDraw() const override { return true; }
    int GetItemWidth() const override;                         // 96dpi 兜底宽
    int GetItemWidthEx(void* hDC) const override;              // 实际宽（含点+间距）
    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override;
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;

private:
    StatusItem() = default;
    // 字体克隆宿主 DC 当前字体（缓存句柄，(高,字重,字面) 变了才重建；随 TM 生命周期释放）
    HFONT FontFor(HDC dc) const;
    mutable std::mutex font_mu_;
    mutable HFONT font_{};
    mutable int font_height_{};      // 克隆的 lfHeight
    mutable LONG font_weight_{};     // 克隆的 lfWeight
    mutable std::wstring font_face_; // 克隆的 lfFaceName

    mutable std::mutex val_mu_;
    mutable std::wstring val_cache_; // GetItemValueText 返回值的宿主缓冲
};

} // namespace wb2
