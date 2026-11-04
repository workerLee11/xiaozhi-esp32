#pragma once

#include <atomic>

#include "lcd_display.h"
#include "character_assets.h"

// 角色屏显示：在官方面包板 LCD 板型上叠加一只「会说话的兔子」。
//
// 线程模型（很重要）：
//   OnAudioStart/OnAudioStop/SetEmotion 由 Application 的状态机/协议线程调用，
//   这些入口只写原子标志，绝不直接调用任何 lv_* API。
//   所有 LVGL 操作统一由常驻的 ui_timer_ 在 LVGL 线程里执行，避免并发破坏对象树。
//
// 显示模型：角色图建在 screen 最底层当纯背景（不能塞进 container_，那是 flex 布局容器）；
//   容器与聊天区设为透明；一句话只在开始/结束各切一次帧，静止时零重绘。
class CharacterLcdDisplay : public SpiLcdDisplay {
public:
    CharacterLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                        int width, int height, int offset_x, int offset_y,
                        bool mirror_x, bool mirror_y, bool swap_xy);
    ~CharacterLcdDisplay() override;

    void SetupUI() override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;

    // 由 Application 在 TTS 音频真正起播/播完时调用，实现说话时的张嘴表情
    void OnAudioStart() override;
    void OnAudioStop() override;

private:
    lv_obj_t* character_img_ = nullptr;   // 角色背景图（screen 最底层）
    lv_timer_t* ui_timer_ = nullptr;      // 常驻 LVGL timer，统一应用状态

    // 跨线程写入，仅此两处；绝不在写入方调用 LVGL
    std::atomic<bool> talking_{false};
    std::atomic<int> pending_emotion_{-1};  // -1 = 无待应用变化；0 中性 / 1 开心 / 2 思考

    // 以下仅由 LVGL 线程（SetupUI / UiTimerCb）访问
    int applied_emotion_ = 0;
    const lv_image_dsc_t* current_src_ = nullptr;

    static void UiTimerCb(lv_timer_t* t);
};
