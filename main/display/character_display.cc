#include "character_display.h"
#include "character_assets.h"
#include "display.h"

#include <esp_log.h>

#include <string>

#include "lvgl.h"

#define CHARACTER_TAG "CharacterLcd"

// 状态轮询周期。绝大多数 tick 不重绘，只有状态真正变化时才切一次帧。
#define UI_TICK_MS 80

namespace {

const lv_image_dsc_t* BaseFrame(int emotion) {
    switch (emotion) {
        case 1:  return &rabbit_happy;
        case 2:  return &rabbit_think;
        default: return &rabbit_neutral;
    }
}

const lv_image_dsc_t* TalkFrame(int emotion) {
    switch (emotion) {
        case 1:  return &rabbit_happy_talk;
        case 2:  return &rabbit_think_talk;
        default: return &rabbit_talk;
    }
}

}  // namespace

CharacterLcdDisplay::CharacterLcdDisplay(
    esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy) {
    // SetupUI() 由 Application 在对象构造完成后以虚函数调用。
}

CharacterLcdDisplay::~CharacterLcdDisplay() {
    if (ui_timer_ != nullptr) {
        lv_timer_del(ui_timer_);
        ui_timer_ = nullptr;
    }
}

void CharacterLcdDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(CHARACTER_TAG, "SetupUI called multiple times, skip");
        return;
    }

    // 官方 UI 内部自带 DisplayLockGuard，返回时已释放
    LcdDisplay::SetupUI();

    // 我们后续的所有 LVGL 操作都必须持锁
    DisplayLockGuard lock(this);

    // 官方浮动表情元素建在 screen 上层，会盖住兔子，全部隐藏
    if (emoji_box_ != nullptr)   lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_image_ != nullptr) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_label_ != nullptr) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    // 主容器与聊天区透明，才能透出底层的角色图
    if (container_ != nullptr) lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
    if (content_ != nullptr)   lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // 角色图建在 screen 上（与官方 emoji_image_ 同层），再移到最底层当纯背景。
    // 绝不能塞进 container_：那是 flex 纵向布局容器，塞入全屏子项会撑爆布局、定位失效。
    character_img_ = lv_img_create(lv_screen_active());
    lv_obj_move_to_index(character_img_, 0);
    lv_obj_set_size(character_img_, width_, height_);
    lv_obj_set_pos(character_img_, 0, 0);
    lv_obj_clear_flag(character_img_, LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_src(character_img_, &rabbit_neutral);
    current_src_ = &rabbit_neutral;

    // 常驻定时器：只在 LVGL 线程里读标志、切帧；跨线程入口绝不碰 LVGL
    ui_timer_ = lv_timer_create(UiTimerCb, UI_TICK_MS, this);

    ESP_LOGI(CHARACTER_TAG, "Character UI ready (background layer %dx%d)", width_, height_);
}

void CharacterLcdDisplay::UiTimerCb(lv_timer_t* t) {
    auto* self = static_cast<CharacterLcdDisplay*>(lv_timer_get_user_data(t));
    if (self == nullptr || self->character_img_ == nullptr) return;

    int emo = self->pending_emotion_.exchange(-1);
    if (emo >= 0) self->applied_emotion_ = emo;

    const bool talking = self->talking_.load();
    const lv_image_dsc_t* want = talking ? TalkFrame(self->applied_emotion_)
                                         : BaseFrame(self->applied_emotion_);

    // 状态没变就什么都不做：静止时零重绘，杜绝整屏反复刷新造成的闪烁
    if (want != self->current_src_) {
        lv_image_set_src(self->character_img_, want);
        self->current_src_ = want;
        ESP_LOGI(CHARACTER_TAG, "frame -> %s (emotion=%d)",
                 talking ? "talk" : "base", self->applied_emotion_);
    }
}

void CharacterLcdDisplay::OnAudioStart() {
    // 只写标志。若在这里直接调 lv_image_set_src / lv_timer_*，
    // 会与 LVGL 渲染任务并发修改对象树，导致花屏甚至死机。
    talking_.store(true);
    ESP_LOGI(CHARACTER_TAG, "audio start");
}

void CharacterLcdDisplay::OnAudioStop() {
    talking_.store(false);
    ESP_LOGI(CHARACTER_TAG, "audio stop");
}

void CharacterLcdDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) return;

    std::string e(emotion);
    int code = 0;
    if (e == "happy" || e == "joy" || e == "excited" || e == "laugh" ||
        e == "开心" || e == "高兴" || e == "喜悦") {
        code = 1;
    } else if (e == "thinking" || e == "think" || e == "thinking1" || e == "think1" ||
               e == "思考" || e == "想一想" || e == "想一下" || e == "想了想") {
        code = 2;
    }
    // 只登记待应用的表情，由 UiTimerCb 在 LVGL 线程里切图
    pending_emotion_.store(code);
    ESP_LOGI(CHARACTER_TAG, "emotion=%s -> %d (deferred)", emotion, code);
}

void CharacterLcdDisplay::SetChatMessage(const char* role, const char* content) {
    // 官方字幕逻辑内部自带 DisplayLockGuard，照常保留
    LcdDisplay::SetChatMessage(role, content);
}
