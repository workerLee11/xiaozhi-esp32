#include "character_display.h"
#include "character_assets.h"
#include "display.h"

#include <esp_log.h>

#include <string>

#include "lvgl.h"

#define CHARACTER_TAG "CharacterLcd"

// 状态轮询周期。绝大多数 tick 完全不重绘；只有状态真正变化时才动一下嘴部贴片。
#define UI_TICK_MS 100

// 嘴部贴片位置：底图 240x320 上实测（鼻子约 (120,155)，微笑嘴线约 y=168）。
// 整块只有 26x15 像素，切换时的脏区远小于一屏，不会触发可见的整屏刷新。
#define MOUTH_W 26
#define MOUTH_OPEN_H 15
#define MOUTH_CX 120
#define MOUTH_CY 169

namespace {

lv_color_t MouthColor(int emotion) {
    switch (emotion) {
        case 1:  return lv_color_hex(0x8A3B44);  // 开心：偏暖
        case 2:  return lv_color_hex(0x4A4668);  // 思考：偏冷
        default: return lv_color_hex(0x5A3232);  // 中性：深棕
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

    lv_obj_t* screen = lv_screen_active();

    // 官方浮动表情 / 图片预览都建在 screen 上层，会盖住兔子，全部隐藏
    if (emoji_box_ != nullptr)     lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_image_ != nullptr)   lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_label_ != nullptr)   lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    if (preview_image_ != nullptr) lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    // 让底层的背景图透出来（官方主题切换可能重设颜色，但不会动 screen 的背景图）
    if (container_ != nullptr)  lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
    if (content_ != nullptr)    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    if (top_bar_ != nullptr)    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    if (bottom_bar_ != nullptr) lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);

    // 角色底图 = screen 的样式背景图。
    // 与官方 SetTheme() 里给 container_ 设背景图是同一套 API；这里特意设在 screen 上，
    // 因为 SetTheme() 只会清 container_ 的 bg_image_src，不会动 screen 的。
    // 它不是对象，不受 flex 布局 / 对齐 / move_to_index 影响，物理上不可能位移。
    // 只设置这一次，运行期永不更换 => 永不产生全屏重绘。
    lv_obj_set_style_bg_image_src(screen, &rabbit_neutral, 0);

    // 嘴部小贴片：静止时隐藏，说话时显示一个深色张嘴形状
    mouth_ = lv_obj_create(screen);
    lv_obj_set_size(mouth_, MOUTH_W, MOUTH_OPEN_H);
    lv_obj_set_pos(mouth_, MOUTH_CX - MOUTH_W / 2, MOUTH_CY - MOUTH_OPEN_H / 2);
    lv_obj_set_style_bg_color(mouth_, MouthColor(0), 0);
    lv_obj_set_style_bg_opa(mouth_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mouth_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(mouth_, 0, 0);
    lv_obj_set_style_pad_all(mouth_, 0, 0);
    lv_obj_clear_flag(mouth_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mouth_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);

    // 常驻定时器：只在 LVGL 线程里读标志、改嘴部贴片
    ui_timer_ = lv_timer_create(UiTimerCb, UI_TICK_MS, this);

    ESP_LOGI(CHARACTER_TAG,
             "ready: screen %dx%d, bg %dx%d (static, never re-set), mouth patch %dx%d",
             (int)lv_obj_get_width(screen), (int)lv_obj_get_height(screen),
             (int)rabbit_neutral.header.w, (int)rabbit_neutral.header.h,
             MOUTH_W, MOUTH_OPEN_H);
}

void CharacterLcdDisplay::UiTimerCb(lv_timer_t* t) {
    auto* self = static_cast<CharacterLcdDisplay*>(lv_timer_get_user_data(t));
    if (self == nullptr || self->mouth_ == nullptr) return;

    const int emo = self->pending_emotion_.exchange(-1);
    if (emo >= 0) self->applied_emotion_ = emo;

    const bool talking = self->talking_.load();

    // 状态未变：什么都不做，静止时零重绘
    if (emo < 0 && talking == self->applied_talking_) return;

    self->applied_talking_ = talking;
    if (talking) {
        lv_obj_set_style_bg_color(self->mouth_, MouthColor(self->applied_emotion_), 0);
        lv_obj_set_size(self->mouth_, MOUTH_W, MOUTH_OPEN_H);
        lv_obj_set_pos(self->mouth_, MOUTH_CX - MOUTH_W / 2, MOUTH_CY - MOUTH_OPEN_H / 2);
        lv_obj_remove_flag(self->mouth_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(self->mouth_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(CHARACTER_TAG, "mouth -> %s (emotion=%d), dirty area %dx%d only",
             talking ? "open" : "closed", self->applied_emotion_, MOUTH_W, MOUTH_OPEN_H);
}

void CharacterLcdDisplay::OnAudioStart() {
    // 只写标志。若在这里直接调 LVGL API，会与渲染任务并发修改对象树。
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
    // 只登记待应用的表情，由 UiTimerCb 在 LVGL 线程里改嘴部贴片颜色
    pending_emotion_.store(code);
    ESP_LOGI(CHARACTER_TAG, "emotion=%s -> %d (deferred, no full refresh)", emotion, code);
}

void CharacterLcdDisplay::SetChatMessage(const char* role, const char* content) {
    // 官方字幕逻辑内部自带 DisplayLockGuard，照常保留
    LcdDisplay::SetChatMessage(role, content);
}
