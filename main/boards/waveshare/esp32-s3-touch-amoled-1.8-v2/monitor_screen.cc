#include "monitor_screen.h"

#include "keyboard_monitor.h"

#include "board.h"
#include "display.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static const char* TAG = "MonitorScreen";

#define SCREEN_W 368
#define SCREEN_H 448
#define MARGIN 20

// ---- 顶部信息区 v6 坐标（368×448 原始像素）----
// 连接芯片（右缘）：宽 60、高 30，gap 8，总高 68，top=MARGIN
#define CONN_X (SCREEN_W - MARGIN - 60)
#define CONN_Y MARGIN
#define CONN_W 60
#define CONN_H 30
#define CONN_GAP 8
// WPM 块与连接区之间的空隙
#define TOP_GAP 14
// WPM 折线：高 45 ≈ 连接区总高 68 的 2/3，与连接区垂直水平居中
#define CHART_W 154
#define CHART_H 45
#define CHART_X MARGIN
#define CHART_Y (CONN_Y + (CONN_H * 2 + CONN_GAP - CHART_H) / 2)  // 32
// WPM 标签 / 数字（水平排列，间距 5）
#define WPM_LABEL_X (CHART_X + CHART_W + 5)
#define WPM_LABEL_Y (CHART_Y + CHART_H / 2 - 12)  // 24px 字，垂直居中
#define WPM_NUM_X (WPM_LABEL_X + 40 + 5)
#define WPM_NUM_Y (CHART_Y + CHART_H / 2 - 18)    // 36px 字，垂直居中
#define WPM_NUM_W 48

// 折线纵向映射：y = H - v*H/200（纵坐标上限 200，无刻度）
#define WPM_CHART_TOP 200

// 中央三行
#define LAYER_Y 168
#define TYPED_Y 248
#define MODS_Y 322
// 底部电量条
#define BATT_Y 392

// 电量分色（>30 绿 / 10-30 黄 / ≤10 红）
#define BATT_LOW 10
#define BATT_MID 30

static const lv_color_t kFg = lv_color_hex(0xFFFFFF);
static const lv_color_t kDim = lv_color_hex(0x9AA0A6);
static const lv_color_t kAccent = lv_color_hex(0x4FC3F7);
static const lv_color_t kBarTrack = lv_color_hex(0x2A2A2A);
static const lv_color_t kGreen = lv_color_hex(0x81C784);
static const lv_color_t kYellow = lv_color_hex(0xFFD54F);
static const lv_color_t kRed = lv_color_hex(0xE57373);
static const lv_color_t kBlue = lv_color_hex(0x64B5F6);
static const lv_color_t kOrange = lv_color_hex(0xFFB74D);
static const lv_color_t kChipBg = lv_color_hex(0x141414);
static const lv_color_t kChipOnBg = lv_color_hex(0x14242E);
static const lv_color_t kChipBorder = lv_color_hex(0x555B66);
static const lv_color_t kChipOnBorder = lv_color_hex(0x4FC3F7);

MonitorScreen& MonitorScreen::GetInstance() {
    static MonitorScreen instance;
    return instance;
}

// ---------- 显示 / 隐藏 ----------
void MonitorScreen::Show() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shown_) {
        return;
    }
    auto display = Board::GetInstance().GetDisplay();
    if (!display) {
        return;
    }
    DisplayLockGuard guard(display);

    prev_screen_ = lv_screen_active();
    screen_ = lv_obj_create(nullptr);
    lv_obj_set_size(screen_, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x000000), 0);
    BuildWidgets();
    lv_screen_load(screen_);
    shown_ = true;

    refresh_timer_ = lv_timer_create(RefreshTimerCb, 500, this);
    return_timer_ = lv_timer_create(ReturnTimerCb,
                                    CONFIG_KEYBOARD_MONITOR_TIMEOUT_SEC * 1000, this);
    lv_timer_set_repeat_count(return_timer_, 1);
    ESP_LOGI(TAG, "monitor screen shown");
}

void MonitorScreen::Hide() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!shown_) {
        return;
    }
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        DisplayLockGuard guard(display);
        if (refresh_timer_) {
            lv_timer_del(refresh_timer_);
            refresh_timer_ = nullptr;
        }
        if (return_timer_) {
            lv_timer_del(return_timer_);
            return_timer_ = nullptr;
        }
        if (prev_screen_) {
            lv_screen_load(prev_screen_);
        }
        if (screen_) {
            lv_obj_del(screen_);
            screen_ = nullptr;
        }
    }
    shown_ = false;
    ESP_LOGI(TAG, "monitor screen hidden");
}

bool MonitorScreen::IsShown() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shown_;
}

// ---------- 手势轮询（左滑切换） ----------
void MonitorScreen::StartGesturePolling() {
    // 【启动期错峰】本函数由扫描任务在 BLE 初始化后调用。启动早期（约 3.9~4.5s）
    // WiFi/BLE 的 phy 校准会写一次 NVS（flash 写），此时 CPU1 若正在执行
    // LVGL 的 flash 代码（如本 timer 回调），双核共享 flash cache 会取指失败
    // → IllegalInstruction panic（PC 落在 0x4202xxxx flash 映射区）→ 重启循环。
    // 这里在拿 LVGL 锁之前先让出 5 秒，等校准保存窗口过去再建 timer，
    // 校准数据保存成功之后后续启动不再重校准，问题自愈。
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "gesture: enter StartGesturePolling");
    auto& self = MonitorScreen::GetInstance();
    ESP_LOGI(TAG, "gesture: GetInstance OK");
    auto display = Board::GetInstance().GetDisplay();
    ESP_LOGI(TAG, "gesture: display=%s", display ? "ok" : "NULL");
    if (!display) {
        return;
    }
    ESP_LOGI(TAG, "gesture: taking lock");
    DisplayLockGuard guard(display);
    ESP_LOGI(TAG, "gesture: lock acquired");
    if (!gesture_timer_) {
        ESP_LOGI(TAG, "gesture polling: creating 100ms timer");
        gesture_timer_ = lv_timer_create(SwipeTimerCb, 100, &self);
        if (gesture_timer_) {
            ESP_LOGI(TAG, "gesture polling started");
        } else {
            ESP_LOGE(TAG, "gesture polling: lv_timer_create FAILED");
        }
    } else {
        ESP_LOGI(TAG, "gesture polling: timer already exists");
    }
}

void MonitorScreen::SwipeTimerCb(lv_timer_t* t) {
    (void)t;
    // 【探针】LVGL 任务存活打点：每 10 秒一条（若日志无此条，说明 LVGL 任务未运行）
    static uint32_t tick = 0;
    if (++tick % 100 == 0) {
        ESP_LOGI(TAG, "lvgl alive tick=%u", (unsigned)(tick / 100));
    }
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            break;
        }
        indev = lv_indev_get_next(indev);
    }
    if (!indev) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGE(TAG, "gesture: no POINTER indev found");
        }
        return;
    }
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_LEFT) {
        ESP_LOGI(TAG, "gesture: LEFT detected, toggling");
        KeyboardMonitor::GetInstance().Toggle();
    }
}

// ---------- 定时刷新 ----------
// LVGL 9.5：lv_timer_t 结构体为私有（lv_timer_private.h），必须用公开访问器取 user_data
void MonitorScreen::RefreshTimerCb(lv_timer_t* t) {
    static_cast<MonitorScreen*>(lv_timer_get_user_data(t))->UpdateWidgets();
}

void MonitorScreen::ReturnTimerCb(lv_timer_t* t) {
    auto* self = static_cast<MonitorScreen*>(lv_timer_get_user_data(t));
    self->Hide();
    KeyboardMonitor::GetInstance().SetActive(false);
    KeyboardMonitor::GetInstance().StopScanning();  // 30s 自动返回：停止扫描
}

// ---------- WPM 历史缓冲 ----------
void MonitorScreen::PushWpm(uint8_t wpm) {
    wpm_hist_[wpm_hist_head_] = wpm;
    wpm_hist_head_ = (wpm_hist_head_ + 1) % kHistMax;
    if (wpm_hist_cnt_ < kHistMax) {
        wpm_hist_cnt_++;
    }
}

void MonitorScreen::RebuildWpmChart() {
    if (!wpm_chart_line_ || wpm_hist_cnt_ == 0) {
        return;
    }
    static lv_point_precise_t pts[kHistMax];
    uint8_t n = wpm_hist_cnt_;
    uint8_t start = (wpm_hist_head_ + kHistMax - n) % kHistMax;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t v = wpm_hist_[(start + i) % kHistMax];
        int32_t y = CHART_H - (int32_t)v * CHART_H / WPM_CHART_TOP;
        if (y < 0) {
            y = 0;
        }
        pts[i] = (lv_point_precise_t){CHART_W * i / (n - 1), y};
    }
    lv_line_set_points(wpm_chart_line_, pts, n);
    lv_line_set_points(wpm_chart_area_, pts, n);

    // 当前值圆点：贴主线末点
    if (lv_obj_is_valid(wpm_dot_)) {
        lv_obj_set_pos(wpm_dot_, pts[n - 1].x - 3, pts[n - 1].y - 3);
    }
}

void MonitorScreen::ClearWpmChart() {
    wpm_hist_cnt_ = 0;
    wpm_hist_head_ = 0;
    lv_line_set_points(wpm_chart_line_, nullptr, 0);
    lv_line_set_points(wpm_chart_area_, nullptr, 0);
    if (lv_obj_is_valid(wpm_dot_)) {
        lv_obj_add_flag(wpm_dot_, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------- 连接芯片样式 ----------
void MonitorScreen::SetConnChip(lv_obj_t* chip, lv_obj_t* label, bool on,
                                const char* text) {
    lv_label_set_text(label, text);
    lv_obj_set_style_bg_color(chip, on ? kChipOnBg : kChipBg, 0);
    lv_obj_set_style_border_color(chip, on ? kChipOnBorder : kChipBorder, 0);
    lv_obj_set_style_text_color(label, on ? kFg : kDim, 0);
}

// ---------- 控件 ----------
void MonitorScreen::BuildWidgets() {
    lv_obj_set_style_pad_all(screen_, 0, 0);

    // --- 顶部 WPM 折线图（两层线模拟渐变面积 + 当前值圆点） ---
    wpm_chart_area_ = lv_line_create(screen_);
    lv_obj_set_size(wpm_chart_area_, CHART_W, CHART_H);
    lv_obj_set_pos(wpm_chart_area_, CHART_X, CHART_Y);
    lv_obj_set_style_line_color(wpm_chart_area_, kAccent, 0);
    lv_obj_set_style_line_width(wpm_chart_area_, 10, 0);
    lv_obj_set_style_opa(wpm_chart_area_, 36, 0);
    lv_obj_set_style_line_rounded(wpm_chart_area_, true, 0);

    wpm_chart_line_ = lv_line_create(screen_);
    lv_obj_set_size(wpm_chart_line_, CHART_W, CHART_H);
    lv_obj_set_pos(wpm_chart_line_, CHART_X, CHART_Y);
    lv_obj_set_style_line_color(wpm_chart_line_, kAccent, 0);
    lv_obj_set_style_line_width(wpm_chart_line_, 2, 0);
    lv_obj_set_style_line_rounded(wpm_chart_line_, true, 0);

    wpm_dot_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(wpm_dot_);
    lv_obj_set_size(wpm_dot_, 6, 6);
    lv_obj_set_style_bg_color(wpm_dot_, kAccent, 0);
    lv_obj_set_style_bg_opa(wpm_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wpm_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(wpm_dot_, CHART_X, CHART_Y);
    lv_obj_add_flag(wpm_dot_, LV_OBJ_FLAG_HIDDEN);

    // --- 顶部 WPM 标签 + 数字（字号 130%：标签 24px、数字 36px） ---
    wpm_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(wpm_label_, kDim, 0);
    lv_obj_set_style_text_font(wpm_label_, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_transform_scale(wpm_label_, 205, 0);  // 30px→≈24px（256=100%）
    lv_obj_set_pos(wpm_label_, WPM_LABEL_X, WPM_LABEL_Y);
    lv_label_set_text(wpm_label_, "WPM");

    wpm_num_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(wpm_num_, kFg, 0);
    lv_obj_set_style_text_font(wpm_num_, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_transform_scale(wpm_num_, 307, 0);  // 30px→≈36px（256=100%）
    lv_obj_set_pos(wpm_num_, WPM_NUM_X, WPM_NUM_Y);
    lv_obj_set_width(wpm_num_, WPM_NUM_W);  // 3 位数字上限
    lv_obj_set_style_text_align(wpm_num_, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(wpm_num_, "--");

    // --- 顶部右缘：USB / BLE 独立窄芯片 ---
    conn_usb_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(conn_usb_);
    lv_obj_set_size(conn_usb_, CONN_W, CONN_H);
    lv_obj_set_pos(conn_usb_, CONN_X, CONN_Y);
    lv_obj_set_style_bg_color(conn_usb_, kChipBg, 0);
    lv_obj_set_style_bg_opa(conn_usb_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(conn_usb_, kChipBorder, 0);
    lv_obj_set_style_border_width(conn_usb_, 1, 0);
    lv_obj_set_style_radius(conn_usb_, 6, 0);
    conn_usb_label_ = lv_label_create(conn_usb_);
    lv_obj_center(conn_usb_label_);
    lv_obj_set_style_text_font(conn_usb_label_, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_transform_scale(conn_usb_label_, 150, 0);  // ~24px
    lv_obj_set_style_text_color(conn_usb_label_, kDim, 0);
    lv_label_set_text(conn_usb_label_, "USB");

    conn_ble_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(conn_ble_);
    lv_obj_set_size(conn_ble_, CONN_W, CONN_H);
    lv_obj_set_pos(conn_ble_, CONN_X, CONN_Y + CONN_H + CONN_GAP);
    lv_obj_set_style_bg_color(conn_ble_, kChipBg, 0);
    lv_obj_set_style_bg_opa(conn_ble_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(conn_ble_, kChipBorder, 0);
    lv_obj_set_style_border_width(conn_ble_, 1, 0);
    lv_obj_set_style_radius(conn_ble_, 6, 0);
    conn_ble_label_ = lv_label_create(conn_ble_);
    lv_obj_center(conn_ble_label_);
    lv_obj_set_style_text_font(conn_ble_label_, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_transform_scale(conn_ble_label_, 150, 0);
    lv_obj_set_style_text_color(conn_ble_label_, kDim, 0);
    lv_label_set_text(conn_ble_label_, "BLE");

    // --- 中央三行 ---
    layer_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(layer_label_, kAccent, 0);
    lv_obj_set_style_text_font(layer_label_, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_transform_scale(layer_label_, 250, 0);
    lv_obj_align(layer_label_, LV_ALIGN_CENTER, 0, LAYER_Y - SCREEN_H / 2);
    lv_label_set_text(layer_label_, "BASE");

    typed_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(typed_label_, kFg, 0);
    lv_obj_set_style_text_font(typed_label_, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_transform_scale(typed_label_, 200, 0);
    lv_obj_align(typed_label_, LV_ALIGN_CENTER, 0, TYPED_Y - SCREEN_H / 2);
    lv_label_set_text(typed_label_, "");

    static const char* kMods[4] = {"CTRL", "SHIFT", "ALT", "GUI"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t* chip = lv_obj_create(screen_);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, 66, 30);
        lv_obj_set_style_bg_color(chip, kChipBg, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chip, 6, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, kDim, 0);
        lv_obj_align(chip, LV_ALIGN_CENTER, -3 * 74 + i * 74 + 37, MODS_Y - SCREEN_H / 2);

        lv_obj_t* lab = lv_label_create(chip);
        lv_obj_center(lab);
        lv_obj_set_style_text_color(lab, kDim, 0);
        lv_label_set_text(lab, kMods[i]);
        mod_chip_[i] = chip;
        lv_obj_set_user_data(chip, lab);
    }

    // --- 底部三电量（L 蓝 / M 白 / R 橙，标签在条上方） ---
    static const lv_color_t kPref[3] = {kBlue, kFg, kOrange};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* bar = lv_bar_create(screen_);
        lv_obj_set_size(bar, 100, 16);
        lv_obj_align(bar, i == 0 ? LV_ALIGN_BOTTOM_LEFT
                                 : (i == 1 ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_BOTTOM_RIGHT),
                     i == 0 ? MARGIN : (i == 1 ? 0 : -MARGIN), -46);
        lv_obj_set_style_bg_color(bar, kBarTrack, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar, kGreen, LV_PART_INDICATOR);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        batt_bar_[i] = bar;

        lv_obj_t* pct = lv_label_create(screen_);
        lv_obj_set_style_text_color(pct, kPref[i], 0);
        lv_obj_align_to(pct, bar, LV_ALIGN_OUT_TOP_MID, 0, -6);
        batt_pct_[i] = pct;
        lv_label_set_text(pct, "--%");
    }
}

// ---------- 刷新 ----------
void MonitorScreen::UpdateWidgets() {
    auto display = Board::GetInstance().GetDisplay();
    if (!display) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!screen_) {
        return;
    }

    KeyboardStatus st = KeyboardMonitor::GetInstance().GetStatus();
    int64_t now = esp_timer_get_time();
    bool stale = !st.valid || (now - st.last_seen_us) > 15LL * 1000LL * 1000LL;

    char buf[32];
    if (stale) {
        // 失联：数字 --、折线清空、芯片全暗、中央 WAITING
        lv_label_set_text(wpm_num_, "--");
        ClearWpmChart();
        lv_label_set_text(layer_label_, "WAITING");
        lv_label_set_text(typed_label_, "");
        SetConnChip(conn_usb_, conn_usb_label_, false, "USB");
        SetConnChip(conn_ble_, conn_ble_label_, false, "BLE");
        for (int i = 0; i < 4; i++) {
            lv_obj_t* lab = (lv_obj_t*)lv_obj_get_user_data(mod_chip_[i]);
            if (lab) {
                lv_obj_set_style_text_color(lab, kDim, 0);
            }
            lv_obj_set_style_bg_color(mod_chip_[i], kChipBg, 0);
        }
        for (int i = 0; i < 3; i++) {
            lv_bar_set_value(batt_bar_[i], 0, LV_ANIM_OFF);
            lv_label_set_text(batt_pct_[i], "--%");
        }
        return;
    }

    // --- WPM：推入历史并刷新折线 + 数字（3 位上限） ---
    PushWpm(st.wpm);
    RebuildWpmChart();
    if (lv_obj_is_valid(wpm_dot_)) {
        lv_obj_clear_flag(wpm_dot_, LV_OBJ_FLAG_HIDDEN);
    }
    snprintf(buf, sizeof(buf), "%d", st.wpm);
    lv_label_set_text(wpm_num_, buf);

    // --- 连接芯片：USB=flags bit2，BLE=flags bit4（BLE 显示 profile） ---
    bool usb = (st.status_flags & 0x04) != 0;
    bool ble = (st.status_flags & 0x10) != 0;
    SetConnChip(conn_usb_, conn_usb_label_, usb, "USB");
    if (ble) {
        snprintf(buf, sizeof(buf), "BLE %d", st.profile);
    } else {
        snprintf(buf, sizeof(buf), "BLE");
    }
    SetConnChip(conn_ble_, conn_ble_label_, ble, buf);

    // --- 中央行 1：层名 ---
    if (st.layer_name[0] != '\0') {
        lv_label_set_text(layer_label_, st.layer_name);
    } else {
        snprintf(buf, sizeof(buf), "LAYER %d", st.layer);
        lv_label_set_text(layer_label_, buf);
    }

    // --- 中央行 2：最近输入字符 ---
    lv_label_set_text(typed_label_, st.typed_keys[0] ? st.typed_keys : " ");

    // --- 中央行 3：修饰键芯片点亮 ---
    for (int i = 0; i < 4; i++) {
        bool on = (st.mods & (1u << i)) != 0;
        lv_obj_t* lab = (lv_obj_t*)lv_obj_get_user_data(mod_chip_[i]);
        if (lab) {
            lv_obj_set_style_text_color(lab, on ? kFg : kDim, 0);
        }
        lv_obj_set_style_bg_color(mod_chip_[i], on ? kChipOnBg : kChipBg, 0);
        lv_obj_set_style_border_color(mod_chip_[i], on ? kAccent : kDim, 0);
    }

    // --- 底部三电量：L=byte5 / M=本机 / R=byte12 ---
    uint8_t lv = st.battery_left;
    uint8_t rv = st.battery_right;
    int own = -1;
    bool charging = false, discharging = false;
    if (Board::GetInstance().GetBatteryLevel(own, charging, discharging)) {
        if (own < 0) {
            own = 0;
        }
    }
    uint8_t levels[3] = {lv, uint8_t(own < 0 ? 0 : own), rv};
    static const char* kTag[3] = {"L", "M", "R"};
    for (int i = 0; i < 3; i++) {
        uint8_t level = levels[i];
        bool unknown = (i == 1 && own < 0) || (i == 2 && rv == 0);
        lv_bar_set_value(batt_bar_[i], unknown ? 0 : level, LV_ANIM_OFF);
        lv_color_t color = level > BATT_MID ? kGreen
                           : (level > BATT_LOW ? kYellow : kRed);
        lv_obj_set_style_bg_color(batt_bar_[i], unknown ? kBarTrack : color,
                                  LV_PART_INDICATOR);
        if (unknown) {
            snprintf(buf, sizeof(buf), "%s --%%", kTag[i]);
        } else {
            snprintf(buf, sizeof(buf), "%s %d%%", kTag[i], level);
        }
        lv_label_set_text(batt_pct_[i], buf);
    }
}
