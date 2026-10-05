#include "language_select.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "settings.h"

#include "esp_log.h"

#include <string_view>

static const char* TAG = "LanguageSelect";

#define SCREEN_W 240
#define SCREEN_H 240

LanguageSelect& LanguageSelect::GetInstance() {
    static LanguageSelect instance;
    return instance;
}

// 板构造时调用（早于 Application::Initialize 的 SetupUI，NVS 已就绪）：
// 有持久化语言则立即 SetLanguage，保证小智 UI 首帧即正确语言
void LanguageSelect::LoadPersisted() {
    Settings settings("user");
    std::string lang = settings.GetString("language", "");
    if (!lang.empty()) {
        if (Lang::SetLanguage(lang.c_str())) {
            ESP_LOGI(TAG, "restore language: %s", lang.c_str());
        } else {
            ESP_LOGW(TAG, "unknown persisted language: %s", lang.c_str());
        }
    }
}

// audio/display 就绪后调用：无语言记录（首次开机）则弹出选择界面
// 返回 true 表示已显示选择界面（调用方可据此临时禁用省电休眠）
bool LanguageSelect::CheckAndShow() {
    Settings settings("user");
    std::string lang = settings.GetString("language", "");
    if (!lang.empty()) {
        return false;  // 已有语言记录，不弹窗
    }
    Show();
    if (active_ && power_save_hook_) {
        power_save_hook_(false);  // 禁用省电休眠，选择界面不超时、不黑屏
    }
    return active_;
}

void LanguageSelect::Show() {
    if (active_) {
        return;
    }
    auto display = Board::GetInstance().GetDisplay();
    DisplayLockGuard lock(display);
    if (!lock) {
        return;
    }

    prev_screen_ = lv_screen_active();
    screen_ = lv_obj_create(nullptr);
    lv_obj_set_size(screen_, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x111111), 0);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_clear_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t* title = lv_label_create(screen_);
    lv_label_set_text(title, "Select Language");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    // 6 个语言列表项（英文显示名，兼容内置 montserrat 字体）
    const int item_w = 168;
    const int item_h = 30;
    const int start_y = 62;
    const int gap = 4;
    const int count = Lang::LanguageCount();
    for (int i = 0; i < count && i < 6; i++) {
        lv_obj_t* row = lv_obj_create(screen_);
        lv_obj_set_size(row, item_w, item_h);
        lv_obj_set_pos(row, (SCREEN_W - item_w) / 2, start_y + i * (item_h + gap));
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, Lang::LanguageDisplayName(i));
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_center(lbl);

        item_row_[i] = row;
        item_lbl_[i] = lbl;
    }

    // 底部操作提示
    lv_obj_t* hint = lv_label_create(screen_);
    lv_label_set_text(hint, "Volume: switch   Boot: confirm");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9AA0A6), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    current_ = 0;
    active_ = true;
    lv_screen_load(screen_);
    UpdateHighlight();
    PlayPrompt();
    ESP_LOGI(TAG, "language select shown (first boot)");
}

void LanguageSelect::Hide() {
    if (!active_) {
        return;
    }
    auto display = Board::GetInstance().GetDisplay();
    DisplayLockGuard lock(display);
    if (!lock) {
        return;
    }
    if (prev_screen_) {
        lv_screen_load(prev_screen_);
    }
    if (screen_) {
        lv_obj_del(screen_);
        screen_ = nullptr;
    }
    active_ = false;
    ESP_LOGI(TAG, "language select hidden");
}

void LanguageSelect::Move(int dir) {
    if (!active_) {
        return;
    }
    int n = Lang::LanguageCount();
    current_ = (current_ + dir + n) % n;
    {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        if (!lock) {
            return;
        }
        UpdateHighlight();
    }
    PlayPrompt();
}

void LanguageSelect::Confirm() {
    if (!active_) {
        return;
    }
    const char* code = Lang::LanguageCode(current_);
    {
        Settings settings("user", true);
        settings.SetString("language", code);
    }
    Lang::SetLanguage(code);
    ESP_LOGI(TAG, "language confirmed: %s", code);
    if (power_save_hook_) {
        power_save_hook_(true);  // 恢复省电休眠
    }
    Hide();
}

void LanguageSelect::UpdateHighlight() {
    if (!active_ || !screen_) {
        return;
    }
    const int count = Lang::LanguageCount();
    for (int i = 0; i < count && i < 6; i++) {
        if (!item_row_[i]) {
            continue;
        }
        bool sel = (i == current_);
        lv_obj_set_style_bg_color(item_row_[i],
                                  sel ? lv_color_hex(0x4FC3F7) : lv_color_hex(0x222222), 0);
        if (item_lbl_[i]) {
            lv_obj_set_style_text_color(item_lbl_[i],
                                        sel ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF), 0);
        }
    }
}

void LanguageSelect::PlayPrompt() {
    // 与 gen_lang.py LANGUAGE_ORDER 顺序一致：zh-CN, fil-PH, vi-VN, ko-KR, ms-MY, en-US
    static const std::string_view kPrompts[6] = {
        Lang::Sounds::OGG_CONFIRM_PRESS_ZH_CN,
        Lang::Sounds::OGG_CONFIRM_PRESS_FIL_PH,
        Lang::Sounds::OGG_CONFIRM_PRESS_VI_VN,
        Lang::Sounds::OGG_CONFIRM_PRESS_KO_KR,
        Lang::Sounds::OGG_CONFIRM_PRESS_MS_MY,
        Lang::Sounds::OGG_CONFIRM_PRESS_EN_US,
    };
    if (current_ < 0 || current_ >= 6) {
        return;
    }
    Application::GetInstance().PlaySound(kPrompts[current_]);
    ESP_LOGI(TAG, "play prompt for lang idx=%d", current_);
}
