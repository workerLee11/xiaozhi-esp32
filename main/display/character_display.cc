#include "character_display.h"
#include "character_assets.h"
#include "display.h"
#include "board.h"

#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "lvgl.h"

#define CHARACTER_TAG "CharacterLcd"

// 顶部栏电量数字用文本字体（图标字体里没有 0-9 和 %）
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

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

// 自检画面用的小色块 / 线条（纯 LVGL 对象，无字体依赖）
void AddRect(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

}  // namespace

CharacterLcdDisplay::CharacterLcdDisplay(
    esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy) {
    offset_x_ = offset_x;
    offset_y_ = offset_y;
    mirror_x_ = mirror_x;
    mirror_y_ = mirror_y;
    swap_xy_ = swap_xy;
    // SetupUI() 由 Application 在对象构造完成后以虚函数调用。
}

CharacterLcdDisplay::~CharacterLcdDisplay() {
    if (ui_timer_ != nullptr) {
        lv_timer_del(ui_timer_);
        ui_timer_ = nullptr;
    }
    if (battery_timer_ != nullptr) {
        lv_timer_del(battery_timer_);
        battery_timer_ = nullptr;
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

    // ---- 开机自检画面（CHARACTER_SELFTEST_MS 后自动删除）----
    // 判断方法：
    //   四条白边都完整          -> 分辨率 / offset 正确
    //   右边或下边缺失 / 图被切 -> 面板实际分辨率与编译配置不一致
    //   四角颜色为 红/绿/蓝/白   -> RGB 顺序正确（红蓝互换 = BGR 配错）
    //   中心十字正好在正中      -> 无偏移
    //   出现横向撕裂 / 斜纹     -> SPI 信号完整性问题
    {
        const int W = width_;
        const int H = height_;
        const lv_color_t white = lv_color_hex(0xFFFFFF);
        const lv_color_t gray = lv_color_hex(0x808080);

        selftest_ = lv_obj_create(screen);
        lv_obj_set_size(selftest_, W, H);
        lv_obj_set_pos(selftest_, 0, 0);
        lv_obj_set_style_bg_color(selftest_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(selftest_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(selftest_, 0, 0);
        lv_obj_set_style_radius(selftest_, 0, 0);
        lv_obj_set_style_pad_all(selftest_, 0, 0);
        lv_obj_clear_flag(selftest_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(selftest_, LV_OBJ_FLAG_CLICKABLE);

        // 2px 外框（四条边分别画，便于判断哪条缺失）
        AddRect(selftest_, 0, 0, W, 2, white);
        AddRect(selftest_, 0, H - 2, W, 2, white);
        AddRect(selftest_, 0, 0, 2, H, white);
        AddRect(selftest_, W - 2, 0, 2, H, white);

        // 四角 16x16 色块：红 / 绿 / 蓝 / 白
        AddRect(selftest_, 4, 4, 16, 16, lv_color_hex(0xFF0000));
        AddRect(selftest_, W - 20, 4, 16, 16, lv_color_hex(0x00FF00));
        AddRect(selftest_, 4, H - 20, 16, 16, lv_color_hex(0x0000FF));
        AddRect(selftest_, W - 20, H - 20, 16, 16, white);

        // 中心十字
        AddRect(selftest_, 0, H / 2 - 1, W, 2, gray);
        AddRect(selftest_, W / 2 - 1, 0, 2, H, gray);

        // 顶部 / 左侧刻度，每 20px 一格，方便数像素判断是否被裁切
        for (int x = 20; x < W - 20; x += 20) AddRect(selftest_, x, 2, 1, 8, gray);
        for (int y = 20; y < H - 20; y += 20) AddRect(selftest_, 2, y, 8, 1, gray);

        lv_timer_t* st = lv_timer_create(SelfTestTimerCb, CHARACTER_SELFTEST_MS, this);
        if (st != nullptr) lv_timer_set_repeat_count(st, 1);
    }

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

    // ---- 顶栏电量百分比（官方电池图标 + 数字）----
    // 官方 battery_label_ 只放图标字符（图标字体里没有 0-9 和 %），
    // 这里在它右侧补一个文本 label。两者同在 right_icons 这个 flex 容器里，
    // 会自动从左到右排布，因此不需要手动定位，也不会影响左侧的网络图标。
    if (battery_label_ != nullptr) {
        lv_obj_t* icons = lv_obj_get_parent(battery_label_);
        battery_pct_ = lv_label_create(icons);
        lv_obj_set_style_text_font(battery_pct_, &BUILTIN_TEXT_FONT, 0);
        // 初始颜色跟随官方图标，保证和电池图标同色
        lv_obj_set_style_text_color(battery_pct_,
                                    lv_obj_get_style_text_color(battery_label_, 0), 0);
        lv_obj_set_style_margin_left(battery_pct_, 3, 0);
        // 固定宽度 + 右对齐：百分比从 9% 变 10% 时宽度不变，
        // 避免 flex 容器重新排布导致顶栏更大面积重绘。
        lv_obj_set_width(battery_pct_, 34);
        lv_obj_set_style_text_align(battery_pct_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(battery_pct_, "");
        lv_obj_add_flag(battery_pct_, LV_OBJ_FLAG_HIDDEN);  // 读到真实电量后再显示

        battery_timer_ = lv_timer_create(BatteryTimerCb, CHARACTER_BATTERY_REFRESH_MS, this);
        if (battery_timer_ != nullptr) {
            lv_timer_ready(battery_timer_);  // 不等 30 秒，下个 tick 先读一次
        }
    }

    ESP_LOGI(CHARACTER_TAG,
             "ready: screen %dx%d, bg %dx%d cf=%d stride=%d, offset(%d,%d) mirror(%d,%d) swap=%d, "
             "spi=10MHz selftest=%dms mouth=%dx%d",
             (int)lv_obj_get_width(screen), (int)lv_obj_get_height(screen),
             (int)rabbit_neutral.header.w, (int)rabbit_neutral.header.h,
             (int)rabbit_neutral.header.cf, (int)rabbit_neutral.header.stride,
             offset_x_, offset_y_, (int)mirror_x_, (int)mirror_y_, (int)swap_xy_,
             CHARACTER_SELFTEST_MS, MOUTH_W, MOUTH_OPEN_H);
}

void CharacterLcdDisplay::SelfTestTimerCb(lv_timer_t* t) {
    auto* self = static_cast<CharacterLcdDisplay*>(lv_timer_get_user_data(t));
    if (self == nullptr || self->selftest_ == nullptr) return;
    lv_obj_del(self->selftest_);
    self->selftest_ = nullptr;
    ESP_LOGI(CHARACTER_TAG, "selftest removed, rabbit background visible");
}

void CharacterLcdDisplay::BatteryTimerCb(lv_timer_t* t) {
    auto* self = static_cast<CharacterLcdDisplay*>(lv_timer_get_user_data(t));
    if (self == nullptr || self->battery_pct_ == nullptr) return;

    int level = 0;
    bool charging = false;
    bool discharging = false;

    if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        // 板型没有实现电量采集：保持隐藏，绝不显示编造的数字
        if (!lv_obj_has_flag(self->battery_pct_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(self->battery_pct_, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGW(CHARACTER_TAG, "battery: not available on this board, gauge hidden");
        }
        return;
    }

    if (level < 0) level = 0;
    if (level > 100) level = 100;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", level);

    // 数字和充电状态都没变：直接返回，静止时零重绘
    if (strcmp(buf, self->last_battery_text_) == 0 && charging == self->last_charging_) {
        return;
    }

    strncpy(self->last_battery_text_, buf, sizeof(self->last_battery_text_) - 1);
    self->last_battery_text_[sizeof(self->last_battery_text_) - 1] = '\0';
    self->last_charging_ = charging;

    // 配色：充电 -> 绿；低电量（<=20%）-> 橙红；其余跟随官方电池图标色
    lv_color_t color = lv_obj_get_style_text_color(self->battery_pct_, 0);
    if (charging) {
        color = lv_color_hex(0x1D9E75);
    } else if (level <= 20) {
        color = lv_color_hex(0xD85A30);
    } else if (self->battery_label_ != nullptr) {
        color = lv_obj_get_style_text_color(self->battery_label_, 0);
    }

    lv_obj_set_style_text_color(self->battery_pct_, color, 0);
    lv_label_set_text(self->battery_pct_, buf);
    lv_obj_remove_flag(self->battery_pct_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(CHARACTER_TAG, "battery -> %d%%%s, dirty area ~28x18 only (top bar)", level,
             charging ? " charging" : (discharging ? " discharging" : ""));
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
