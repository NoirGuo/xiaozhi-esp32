#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_gap_ble_api.h"
#include "esp_timer.h"

// ---- 可调参数默认值（无 Kconfig 时生效；若在 Kconfig.projbuild 定义了
// CONFIG_KEYBOARD_MONITOR_*，ESP-IDF 生成的 CONFIG_ 宏会覆盖下面的默认值） ----
// 【诊断开关】构建烧录后仍黑白屏时，把下面改成 0（关闭全部监控代码，
// 系统回到纯小智）重新构建：若正常进入系统 → 问题在监控代码；若仍黑白屏
// → 问题在 fork 分支 IDF6 构建基线/配置，与监控无关。
#ifndef CONFIG_KEYBOARD_MONITOR_ENABLED
#define CONFIG_KEYBOARD_MONITOR_ENABLED 1
#endif
#ifndef CONFIG_KEYBOARD_MONITOR_CHANNEL
#define CONFIG_KEYBOARD_MONITOR_CHANNEL 1
#endif
#ifndef CONFIG_KEYBOARD_MONITOR_TARGET_MAC
#define CONFIG_KEYBOARD_MONITOR_TARGET_MAC ""
#endif
// 可选项：键盘硬件唯一 ID 绑定（prospector v2.2 载荷 byte 19-22，HWINFO）。
// 十六进制 8 位，如 "A1B2C3D4"；留空 = 不绑定（仅靠 MAC/频道过滤）。
#ifndef CONFIG_KEYBOARD_MONITOR_TARGET_ID
#define CONFIG_KEYBOARD_MONITOR_TARGET_ID ""
#endif
#ifndef CONFIG_KEYBOARD_MONITOR_TIMEOUT_SEC
#define CONFIG_KEYBOARD_MONITOR_TIMEOUT_SEC 30
#endif

// ============================================================
// KeyboardMonitor —— BLE 键盘状态监听（Prospector 协议，zmk-config-Noirix44 monitor 分支 v2.2 布局）
// 板无关模块：ESP32-S3 以 observer 模式扫描键盘广播，解析 26 字节
// 厂商载荷并缓存状态；不建立连接，不占键盘连接槽。
// 一对一监听：可选「MAC 绑定」+「keyboard_id(HWINFO) 绑定」+「频道过滤」。
// ============================================================

// 26 字节广播载荷（含厂商 ID，offset 0-25）——prospector v2.2 权威布局
//（见 zmk-config-Noirix44/monitor 分支 include/zmk/status_advertisement.h）：
// 0-1  = 0xFF 0xFF (Manufacturer ID)
// 2-3  = 0xAB 0xCD (Prospector Protocol ID)
// 4    = version (0x22 = v2.2)
// 5    = 中央（左手）电量 %    ← central=左
// 6    = 层索引
// 7    = profile slot（[2:0]=profile 编号）
// 8    = 已连接设备数
// 9    = 状态标志 (bit0 caps / bit1 charging / bit2 usb / bit3 hid / bit4 ble / bit5 bonded)
// 10   = 角色 (0=standalone, 1=central, 2=peripheral)
// 11   = 分体设备索引 (device_index)
// 12-14= 外设电量 [0]=左, [1]=右/aux, [2]=第三设备 (0=N/A)
// 15-18= 层名 ASCII (4字节, 不保证 NUL 结尾)
// 19-22= keyboard_id（HWINFO 硬件唯一 ID，4 字节）
// 23   = 修饰键位图 (bit0-7: LCTL LSFT LALT LGUI RCTL RSFT RALT RGUI)
// 24   = WPM
// 25   = 频道 (1)

struct KeyboardStatus {
    bool valid = false;
    int64_t last_seen_us = 0;   // esp_timer_get_time()
    int rssi = 0;

    uint8_t battery_left = 0;   // byte 5 中央（左手）
    uint8_t layer = 0;          // byte 6
    uint8_t profile = 0;        // byte 7 & 0x07
    uint8_t conn_count = 0;     // byte 8
    uint8_t status_flags = 0;   // byte 9
    uint8_t role = 0;           // byte 10
    uint8_t device_index = 0;   // byte 11 分体设备索引
    uint8_t battery_right = 0;  // byte 12 外设[0]（左手）
    uint8_t battery_aux = 0;    // byte 13 外设[1]（右手/aux）
    uint8_t battery_per2 = 0;   // byte 14 外设[2]（第三设备）
    char layer_name[5] = {0};   // bytes 15-18
    uint8_t keyboard_id[4] = {0}; // bytes 19-22 HWINFO 硬件唯一 ID
    uint8_t mods = 0;           // byte 23
    uint8_t wpm = 0;            // byte 24
    uint8_t channel = 0;        // byte 25
};

class KeyboardMonitor {
public:
    static KeyboardMonitor& GetInstance();

    // 启动：BLE 初始化 + 扫描任务 + 手势轮询定时器（板构造末尾调用）
    void Start();
    // 停止：停扫描并隐藏监控界面（PowerSaveTimer 休眠回调调用）
    void Stop();

    // 切换监控界面（语音 MCP 工具 / 左滑手势共用入口）
    void Toggle();
    void SetActive(bool active);   // 自动返回后同步状态
    bool IsActive() const;
    // 请求停止扫描（30s 自动返回 / 退出界面时调用；ScanTask 500ms 内执行）
    void RequestScanStop();

    KeyboardStatus GetStatus() const;  // 互斥拷贝

    // 一对一监听（v2.2 载荷含 keyboard_id HWINFO 唯一 ID）：
    // 频道过滤（Kconfig，默认 1）+ MAC 绑定（可选）+ keyboard_id 绑定（可选）
    void SetTargetMac(const uint8_t* mac);   // nullptr = 不绑定
    void SetTargetId(uint32_t id);           // 0 = 不绑定（v2.2 keyboard_id 大端 4 字节）

    // 以下为 BLE 栈 / FreeRTOS 回调入口，需从自由函数调用，故为 public：
    static void ScanTaskThunk(void* arg);
    static void GapEventHandler(esp_gap_ble_cb_event_t event,
                                esp_ble_gap_cb_param_t* param);

private:
    KeyboardMonitor() = default;
    ~KeyboardMonitor() = default;
    KeyboardMonitor(const KeyboardMonitor&) = delete;
    KeyboardMonitor& operator=(const KeyboardMonitor&) = delete;

    void ScanTask();
    void HandleAdv(const esp_ble_gap_cb_param_t::ble_scan_result_evt_param& rst);
    void ParseTargetMac(const char* str);
    void ParseTargetId(const char* str);

    mutable std::mutex mutex_;
    KeyboardStatus status_;

    volatile bool started_ = false;
    volatile bool active_ = false;
    volatile bool scanning_ = false;
    volatile bool ble_ready_ = false;  // BLE 栈已初始化（按需扫描前提）
    volatile bool scan_wanted_ = false;  // 界面切换请求标志：true=开扫 false=停扫（ScanTask 轮询执行）
    TaskHandle_t scan_task_handle_ = nullptr;

    uint8_t target_mac_[6] = {0};
    bool has_target_mac_ = false;
    uint32_t target_id_ = 0;       // v2.2 keyboard_id（大端）
    bool has_target_id_ = false;

    int64_t last_toggle_us_ = 0;
};
//（注：内容由AI生成）
