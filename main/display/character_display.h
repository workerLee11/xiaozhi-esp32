#pragma once

#include <atomic>

#include "lcd_display.h"

// 角色屏显示：在官方面包板 LCD 板型上叠加一只「会说话的兔子」。
//
// 线程模型：
//   OnAudioStart / OnAudioStop / SetEmotion 由 Application 的状态机/协议线程调用，
//   这些入口只写 std::atomic 标志，绝不直接调用任何 lv_* API。
//   所有 LVGL 操作统一由常驻 ui_timer_ 在 LVGL 线程里执行，避免并发破坏对象树。
//
// 显示模型（v3，稳定性优先）：
//   兔子底图设为 screen 的「样式背景图」(lv_obj_set_style_bg_image_src)，
//   它不是 LVGL 对象，不参与对象树 / flex 布局 / 层级排序，因此不可能位移或被挤歪。
//   该背景图只在启动时设置一次，运行期永不更换 —— 永远不产生全屏重绘，
//   从而消除单缓冲 partial-refresh 下的扫描撕裂（黑白相间线条 / 倒转 / 闪烁）。
//   说话与表情改由一块 26x15 的嘴部贴片表达，每次只重绘这一小块区域。
class CharacterLcdDisplay : public SpiLcdDisplay {
public:
    CharacterLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                        int width, int height, int offset_x, int offset_y,
                        bool mirror_x, bool mirror_y, bool swap_xy);
    ~CharacterLcdDisplay() override;

    void SetupUI() override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;

    // 由 Application 在 TTS 音频真正起播/播完时调用
    void OnAudioStart() override;
    void OnAudioStop() override;

private:
    lv_obj_t* mouth_ = nullptr;           // 嘴部小贴片（screen 上，仅 26x15 像素）
    lv_timer_t* ui_timer_ = nullptr;      // 常驻 LVGL timer，统一应用状态

    // 跨线程写入，仅此两处；绝不在写入方调用 LVGL
    std::atomic<bool> talking_{false};
    std::atomic<int> pending_emotion_{-1};  // -1 = 无待应用变化；0 中性 / 1 开心 / 2 思考

    // 以下仅由 LVGL 线程（SetupUI / UiTimerCb）访问
    bool applied_talking_ = false;
    int applied_emotion_ = 0;

    static void UiTimerCb(lv_timer_t* t);
};
