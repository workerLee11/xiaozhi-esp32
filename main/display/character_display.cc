#include "character_display.h"
#include "character_assets.h"

#include <esp_timer.h>
#include <string>

#include "lvgl.h"

#define CHARACTER_TAG "CharacterLcd"

// 嘴部动画帧率：每 140ms 在闭嘴/张嘴之间翻一次，约等于自然说话节奏
#define MOUTH_INTERVAL_MS 140

CharacterLcdDisplay::CharacterLcdDisplay(
    esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy) {
    // 注意：SetupUI() 由 application.cc 在对象构造完成后以虚函数调用，
    // 届时本类的 SetupUI 覆盖会被正确执行，无需在此手动调用。
}

CharacterLcdDisplay::~CharacterLcdDisplay() {
    if (mouth_timer_ != nullptr) {
        lv_timer_del(mouth_timer_);
        mouth_timer_ = nullptr;
    }
}

void CharacterLcdDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(CHARACTER_TAG, "SetupUI called multiple times, skip");
        return;
    }

    // 1) 先构建官方标准 UI（顶栏/状态栏/字幕栏/默认表情图等）
    LcdDisplay::SetupUI();

    // 2) 隐藏默认的机器人表情图，避免和角色图重叠
    if (emoji_box_ != nullptr)   lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_image_ != nullptr) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_label_ != nullptr) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    // 3) 创建角色主图（全屏），作为背景层 container_ 的子对象，
    //    这样它一定位于顶栏/状态栏/字幕栏之下。
    character_img_ = lv_img_create(container_);
    lv_image_set_src(character_img_, &rabbit_neutral);
    lv_obj_set_size(character_img_, width_, height_);
    lv_obj_align(character_img_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(character_img_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);

    // 4) 嘴部动画定时器：默认暂停，说话时才跑
    mouth_timer_ = lv_timer_create(MouthTimerCb, MOUTH_INTERVAL_MS, this);
    lv_timer_pause(mouth_timer_);

    ESP_LOGI(CHARACTER_TAG, "Character UI ready (%dx%d rabbit)", width_, height_);
}

void CharacterLcdDisplay::StartTalking() {
    if (character_img_ == nullptr) return;
    talking_ = true;
    mouth_open_ = false;
    // 从当前情绪底图开始嘴动
    lv_image_set_src(character_img_, emotion_base_);
    if (mouth_timer_ != nullptr) lv_timer_resume(mouth_timer_);
}

void CharacterLcdDisplay::StopTalking() {
    talking_ = false;
    mouth_open_ = false;
    // 停嘴后回到当前情绪底图（可能是开心/思考等）
    if (character_img_ != nullptr) lv_image_set_src(character_img_, emotion_base_);
    if (mouth_timer_ != nullptr) lv_timer_pause(mouth_timer_);
}

void CharacterLcdDisplay::MouthTimerCb(lv_timer_t* t) {
    auto* self = static_cast<CharacterLcdDisplay*>(lv_timer_get_user_data(t));
    if (self == nullptr) return;

    if (!self->talking_) {
        lv_timer_pause(t);
        return;
    }
    // 仅在「正在说话」时翻转嘴型；停止由 OnAudioStop 触发，不靠计时
    self->mouth_open_ = !self->mouth_open_;
    lv_image_set_src(self->character_img_,
                   self->mouth_open_ ? self->emotion_talk_ : self->emotion_base_);
}

void CharacterLcdDisplay::OnAudioStart() {
    // TTS 真正开始播放：开启嘴动（时长由 OnAudioStop 决定）
    ESP_LOGI(CHARACTER_TAG, "audio start -> talk on");
    StartTalking();
}

void CharacterLcdDisplay::OnAudioStop() {
    // TTS 播放结束：关闭嘴动并回到当前情绪底图
    ESP_LOGI(CHARACTER_TAG, "audio stop -> talk off");
    StopTalking();
}

void CharacterLcdDisplay::SetEmotion(const char* emotion) {
    // 角色模式下，表情由角色图本身表现（不再去设置已隐藏的 emoji_image_）。
    // 根据服务端下发的情绪字符串，切换当前「底图 + 张嘴帧」；
    // 说话中只更新映射、不动当前显示，停下后自动回到对应情绪底图。
    if (emotion == nullptr) return;

    const lv_image_dsc_t* base = &rabbit_neutral;
    const lv_image_dsc_t* talk = &rabbit_talk;

    std::string e(emotion);
    // 同时兼容英文与中文情绪名（xiaozhi 服务端下发的名称取决于控制台配置）
    if (e == "happy" || e == "joy" || e == "excited" || e == "laugh" ||
        e == "开心" || e == "高兴" || e == "喜悦") {
        base = &rabbit_happy;      talk = &rabbit_happy_talk;
    } else if (e == "thinking" || e == "think" || e == "thinking1" || e == "think1" ||
               e == "思考" || e == "想一想" || e == "想一下" || e == "想了想") {
        base = &rabbit_think;      talk = &rabbit_think_talk;
    }
    // 其它（neutral / listening / 未知）一律回落到中性帧

    emotion_base_ = base;
    emotion_talk_ = talk;

    if (!talking_ && character_img_ != nullptr) {
        lv_image_set_src(character_img_, base);
    }
    ESP_LOGI(CHARACTER_TAG, "emotion=%s -> base set", emotion);
}

void CharacterLcdDisplay::SetChatMessage(const char* role, const char* content) {
    // 保留官方字幕（底部栏）逻辑，让 AI 的回复文字照常显示
    LcdDisplay::SetChatMessage(role, content);
    // 嘴动不再按字数估算，改由 OnAudioStart/OnAudioStop（TTS 真实起止）驱动，
    // 这样「开口」与声音完全同步，且不会在句子间隙误停。
}
