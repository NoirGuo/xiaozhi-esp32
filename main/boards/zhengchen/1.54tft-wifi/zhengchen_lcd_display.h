#ifndef ZHENGCHEN_LCD_DISPLAY_H
#define ZHENGCHEN_LCD_DISPLAY_H

#include "display/lcd_display.h"
#include "lvgl_theme.h"
#include <esp_lvgl_port.h>

class ZHENGCHEN_LcdDisplay : public SpiLcdDisplay {
protected:
    lv_obj_t* high_temp_popup_ = nullptr;  // 高温警告弹窗
    lv_obj_t* high_temp_label_ = nullptr;  // 高温警告标签

public:
    // 继承构造函数
    using SpiLcdDisplay::SpiLcdDisplay;

    // 240×240 上聊天气泡已占满可视区，64px 的表情头像会压住第一行文字。
    // 需要保留表情头像时把这里改成 false（其余逻辑不变）。
    static constexpr bool kHideEmojiAvatarInChat = true;

    // 微信对话样式下，给聊天区打开"按需显示"的细滚动条：
    // 只在翻页那一刻出现，停手即隐藏，用来提示"上面/下面还有内容"。
    void SetupUI() override {
        SpiLcdDisplay::SetupUI();  // 内部有 setup_ui_called_ 判重，可安全重复调用
        DisplayLockGuard lock(this);
        if (content_ != nullptr) {
            lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_AUTO);
        }
    }

    // 聊天区当前是否还能上下滚动（内容超出可视区）。
    // 默认三段式下 content_ 为空，恒返回 false。
    bool IsChatScrollable() {
        DisplayLockGuard lock(this);
        if (content_ == nullptr) {
            return false;
        }
        if (lv_obj_get_scroll_dir(content_) == LV_DIR_NONE) {
            return false;
        }
        return lv_obj_get_scroll_top(content_) > 0 || lv_obj_get_scroll_bottom(content_) > 0;
    }

    // 翻一页：direction < 0 向上（看更早的消息），> 0 向下（看更新的消息）。
    // 已在顶/底时返回 false，调用方可据此停止连续翻页。
    bool ScrollChatPage(int direction) {
        if (direction == 0) {
            return false;
        }
        DisplayLockGuard lock(this);
        if (content_ == nullptr) {
            return false;
        }

        int32_t top = lv_obj_get_scroll_top(content_);
        int32_t bottom = lv_obj_get_scroll_bottom(content_);
        if (direction < 0 && top <= 0) {
            return false;
        }
        if (direction > 0 && bottom <= 0) {
            return false;
        }

        // 翻 80% 视口，保留一行重叠，避免读完一行还要回翻
        lv_coord_t view_h = lv_obj_get_content_height(content_);
        int32_t step = view_h - view_h / 5;
        if (step < 24) {
            step = 24;  // 极小屏幕兜底，避免 0 步长原地不动
        }

        // LVGL 的 scroll_y 向下为负值
        int32_t y = lv_obj_get_scroll_y(content_) + (direction > 0 ? -step : step);
        lv_obj_scroll_to_y(content_, y, LV_ANIM_ON);
        return true;
    }

    // 微信对话样式下不显示表情头像（避免遮挡 240×240 上的正文）。
    // 注意：返回值仍表示"上游是否成功处理了该表情"，只是不显示出来。
    bool SetEmotion(const char* emotion) override {
        bool shown = SpiLcdDisplay::SetEmotion(emotion);
        if (kHideEmojiAvatarInChat) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();  // 头像已隐藏，别让 GIF 继续解码吃 CPU
            }
            if (emoji_image_ != nullptr) {
                lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            }
            if (emoji_label_ != nullptr) {
                lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return shown;
    }

    void SetupHighTempWarningPopup() {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        auto text_font = lvgl_theme->text_font()->font();
        // 创建高温警告弹窗
        high_temp_popup_ = lv_obj_create(lv_screen_active());  // 使用当前屏幕
        lv_obj_set_scrollbar_mode(high_temp_popup_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(high_temp_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
        lv_obj_align(high_temp_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(high_temp_popup_, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_set_style_radius(high_temp_popup_, 10, 0);
        
        // 创建警告标签
        high_temp_label_ = lv_label_create(high_temp_popup_);
        lv_label_set_text(high_temp_label_, "警告：温度过高");
        lv_obj_set_style_text_color(high_temp_label_, lv_color_white(), 0);
        lv_obj_center(high_temp_label_);
        
        // 默认隐藏
        lv_obj_add_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
    }

    void UpdateHighTempWarning(float chip_temp, float threshold = 75.0f) {
        if (high_temp_popup_ == nullptr) {
            ESP_LOGW("ZHENGCHEN_LcdDisplay", "High temp popup not initialized!");
            return;
        }

        if (chip_temp >= threshold) {
            ShowHighTempWarning();
        } else {
            HideHighTempWarning();
        }
    }

    void ShowHighTempWarning() {
        if (high_temp_popup_ && lv_obj_has_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void HideHighTempWarning() {
        if (high_temp_popup_ && !lv_obj_has_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
        }
    }
};



#endif // ZHENGCHEN_LCD_DISPLAY_H
