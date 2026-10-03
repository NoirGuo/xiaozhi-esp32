#pragma once

#include <cstdint>
#include <mutex>

#include <lvgl.h>

// ============================================================
// MonitorScreen —— LVGL 键盘监控界面（368×448，微雪 1.8 AMOLED）
// 布局 v6（已按用户确认的效果图定稿）：
//   顶部一行：左 = WPM 折线图（上限 200、无坐标轴、渐变面积、当前值圆点）
//             中 = "WPM" 标签 + 数字（3 位上限，字号 130% 放大）
//             右 = USB / BLE 两个独立窄芯片（生效点亮，BLE 显示 "BLE n"）
//   中央三行：层名 → 最近输入字符 → 修饰键芯片（按下点亮）
//   底部三行电量条：L(左手)/M(本机)/R(右手) 分色
// 数据来自 KeyboardMonitor::GetStatus()（互斥拷贝）。
// ============================================================

class MonitorScreen {
public:
    static MonitorScreen& GetInstance();

    void Show();                // 保存当前屏 -> 载入监控屏 + 刷新/返回定时器
    void Hide();                // 恢复原屏，删除定时器
    bool IsShown() const;

    void StartGesturePolling(); // 创建 100ms 左滑轮询定时器（KeyboardMonitor::Start 调用）

private:
    MonitorScreen() = default;
    ~MonitorScreen() = default;
    MonitorScreen(const MonitorScreen&) = delete;
    MonitorScreen& operator=(const MonitorScreen&) = delete;

    static void RefreshTimerCb(lv_timer_t* t);
    static void ReturnTimerCb(lv_timer_t* t);
    static void SwipeTimerCb(lv_timer_t* t);

    void BuildWidgets();
    void UpdateWidgets();

    void PushWpm(uint8_t wpm);   // 推入 WPM 环形缓冲
    void RebuildWpmChart();      // 用缓冲重建折线（面积粗线 + 主线 + 当前值圆点）
    void ClearWpmChart();
    void SetConnChip(lv_obj_t* chip, lv_obj_t* label, bool on,
                     const char* text);

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* prev_screen_ = nullptr;
    lv_timer_t* refresh_timer_ = nullptr;
    lv_timer_t* return_timer_ = nullptr;
    lv_timer_t* gesture_timer_ = nullptr;

    // 顶部 WPM 区（折线图 + 标签 + 数字，水平排列）
    lv_obj_t* wpm_chart_area_ = nullptr;  // 底层粗线（模拟渐变面积，低透明）
    lv_obj_t* wpm_chart_line_ = nullptr;  // 上层主线
    lv_obj_t* wpm_dot_ = nullptr;         // 当前值圆点
    lv_obj_t* wpm_label_ = nullptr;       // "WPM"
    lv_obj_t* wpm_num_ = nullptr;         // 数字（3 位上限）

    // 顶部连接芯片（右缘）
    lv_obj_t* conn_usb_ = nullptr;
    lv_obj_t* conn_usb_label_ = nullptr;
    lv_obj_t* conn_ble_ = nullptr;
    lv_obj_t* conn_ble_label_ = nullptr;

    // 中央三行
    lv_obj_t* layer_label_ = nullptr;
    lv_obj_t* typed_label_ = nullptr;
    lv_obj_t* mod_chip_[4] = {nullptr, nullptr, nullptr, nullptr};
    // 底部三电量
    lv_obj_t* batt_bar_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* batt_pct_[3] = {nullptr, nullptr, nullptr};

    // WPM 历史环形缓冲（60 点 ≈ 30s @500ms）
    static constexpr uint8_t kHistMax = 60;
    uint8_t wpm_hist_[kHistMax] = {0};
    uint8_t wpm_hist_cnt_ = 0;
    uint8_t wpm_hist_head_ = 0;

    mutable std::mutex mutex_;
    bool shown_ = false;
};
//（注：内容由AI生成）
