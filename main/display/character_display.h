#pragma once

#include "lcd_display.h"

// 角色屏显示：在官方面包板 LCD 板型上叠加一只「会说话的兔子」。
// 原理：继承 SpiLcdDisplay，复用其全部 SPI/LVGL 初始化；
// SetupUI() 先建好标准 UI（顶栏/状态栏/字幕栏），再把默认机器人表情图换成兔子，
// 并用一个 LVGL 定时器在「闭嘴」和「张嘴」两帧之间切换，实现说话时嘴巴动。
//
// 素材：character_assets.h 里的 6 张兔子帧（每情绪含「闭嘴底图」+「张嘴说话帧」），
// 由本目录外的 convert.py 从 AI 生成的 PNG 转成 LVGL RGB565 数组：
//   rabbit_neutral / rabbit_talk      （中性）
//   rabbit_happy  / rabbit_happy_talk （开心）
//   rabbit_think  / rabbit_think_talk （思考）
class CharacterLcdDisplay : public SpiLcdDisplay {
public:
    CharacterLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                        int width, int height, int offset_x, int offset_y,
                        bool mirror_x, bool mirror_y, bool swap_xy);
    ~CharacterLcdDisplay() override;

    void SetupUI() override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;

    // 由 Application 在 TTS 音频真正起播/播完时调用，实现「精准嘴动」
    void OnAudioStart() override;
    void OnAudioStop() override;

private:
    lv_obj_t* character_img_ = nullptr;   // 角色主图（全屏）
    lv_timer_t* mouth_timer_ = nullptr;   // 嘴部动画定时器
    bool talking_ = false;                // 是否正在说话
    bool mouth_open_ = false;             // 当前帧是否张嘴

    // 当前情绪对应的底图与说话帧：SetEmotion 据情绪字符串切换，
    // 嘴部动画在两者间翻转，从而实现「开心时也跟着嘴动」等效果。
    const lv_image_dsc_t* emotion_base_ = &rabbit_neutral;
    const lv_image_dsc_t* emotion_talk_ = &rabbit_talk;

    void StartTalking();
    void StopTalking();
    static void MouthTimerCb(lv_timer_t* t);
};
