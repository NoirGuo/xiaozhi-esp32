#include "keyboard_monitor.h"

#include "monitor_screen.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static const char* TAG = "KeyboardMonitor";

// ---- 协议常量 ----
#define KM_ADV_TYPE_MANUFACTURER 0xFF
#define KM_AD_LEN_MIN 27  // [len][type(0xFF)][26B 载荷]，len 含 type
#define KM_COMPANY0 0xFF
#define KM_COMPANY1 0xFF
#define KM_PROTO0   0xAB
#define KM_PROTO1   0xCD
// 失联判定 15s：键盘空闲广播间隔 10s（st7789v 分支 noirix44_left.conf），
// 与 st7789v 监视器 WAITING 阈值一致
#define KM_STALE_US (15LL * 1000LL * 1000LL)
#define KM_DEBOUNCE_US (500LL * 1000LL)

// 低占空扫描：80ms 周期 / 30ms 窗口（如功耗敏感可改 BLE_SCAN_DUPLICATE）
// 注意：ESP-IDF 6.x 中 esp_ble_gap_set_scan_params 参数为非 const 指针
static esp_ble_scan_params_t kScanParams = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,  // 主动扫描，可收到 Scan Response
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x80,   // 128 * 0.625ms = 80ms
    .scan_window = 0x30,     // 48 * 0.625ms = 30ms
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,  // 持续收 200ms/10s 间隔更新
};

KeyboardMonitor& KeyboardMonitor::GetInstance() {
    static KeyboardMonitor instance;
    return instance;
}

// ---------- BLE 初始化 ----------
// 键盘监控是附加功能：BLE 任何一步失败都只降级（记录日志、停用监控），
// 绝不用 ESP_ERROR_CHECK abort——否则会拖垮整个小智系统（黑白屏重启循环）。
// 特别注意：S3 无经典蓝牙，esp_bt_controller_mem_release(CLASSIC_BT) 会返回
// 非 ESP_OK，必须容忍。
// 幂等设计：休眠唤醒会再次调用，按 controller/bluedroid 当前状态跳过已完成步骤。
static bool InitBle() {
    esp_err_t err;
    // 【探针】确认蓝牙核绑定配置是否真正编入本固件（config.json sdkconfig_append 生效检查）
#ifdef CONFIG_BT_CTRL_PINNED_TO_CORE_CHOICE_0
    ESP_LOGI(TAG, "BT_CTRL core=0 (config applied)");
#elif defined(CONFIG_BT_CTRL_PINNED_TO_CORE_CHOICE_1)
    ESP_LOGI(TAG, "BT_CTRL core=1 (config NOT applied!)");
#else
    ESP_LOGI(TAG, "BT_CTRL core config missing");
#endif
#ifdef CONFIG_BT_BLUEDROID_PINNED_TO_CORE_0
    ESP_LOGI(TAG, "BLUEDROID core=0 (config applied)");
#elif defined(CONFIG_BT_BLUEDROID_PINNED_TO_CORE_1)
    ESP_LOGI(TAG, "BLUEDROID core=1 (config NOT applied!)");
#else
    ESP_LOGI(TAG, "BLUEDROID core config missing");
#endif
    esp_bt_controller_status_t ctl_st = esp_bt_controller_get_status();
    if (ctl_st == ESP_BT_CONTROLLER_STATUS_IDLE) {
        err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (err != ESP_OK && err != ESP_ERR_INVALID_ARG && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "bt mem_release(CLASSIC): %s", esp_err_to_name(err));
        }
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&bt_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bt controller init failed: %s", esp_err_to_name(err));
            return false;
        }
        ctl_st = esp_bt_controller_get_status();
    }
    if (ctl_st == ESP_BT_CONTROLLER_STATUS_INITED || ctl_st == ESP_BT_CONTROLLER_STATUS_IDLE) {
        // ESP32-S3 无经典蓝牙（BR/EDR），控制器只支持 BLE 模式（mode 1）。
        // 用 ESP_BT_MODE_BTDM(3) 会报 "invalid mode 3, controller support mode is 1"，
        // enable 失败后控制器处于半初始化状态，80ms 后触发 IllegalInstruction panic。
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bt controller enable failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        err = esp_bluedroid_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        err = esp_bluedroid_enable();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    err = esp_ble_gap_register_callback(&KeyboardMonitor::GapEventHandler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gap register callback failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "BLE initialized");
    return true;
}

// ---------- 扫描任务 ----------
void KeyboardMonitor::ScanTaskThunk(void* arg) {
    static_cast<KeyboardMonitor*>(arg)->ScanTask();
}

void KeyboardMonitor::ScanTask() {
    vTaskDelay(pdMS_TO_TICKS(1500));  // 等系统/音频就绪
    // 【关键时序修复】手势轮询不能在 Board 构造（main 线程）创建：
    // 那时 LVGL 任务刚启动可能持锁初始化，而 lv_lock() 无超时，
    // main 线程拿锁会永久卡死 → Application::Initialize() 不执行 → 白屏。
    // 改在扫描任务内（此处约 4s，系统已完全启动、LVGL 正常循环）创建。
    MonitorScreen::GetInstance().StartGesturePolling();
    if (!InitBle()) {
        ESP_LOGE(TAG, "BLE init failed, keyboard monitor disabled (AI still works)");
        // 不退出任务：规避"任务退出/空闲路径"崩溃（此前按需版 panic 即发生在此类路径）
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    ble_ready_ = true;
    ESP_LOGI(TAG, "BLE ready (on-demand scanning: start/stop follows monitor UI)");
    // 【严格按需版】任务常驻不退出；平时不扫描（controller 空闲），
    // 切入监听界面（scan_wanted_=true）才开扫，退出即停。
    // 开/停扫全部在本任务内串行执行，避免跨任务直接调 BLE API 的竞态。
    while (true) {
        if (scan_wanted_ && !scanning_) {
            if (esp_ble_gap_set_scan_params(&kScanParams) == ESP_OK &&
                esp_ble_gap_start_scanning(0) == ESP_OK) {
                scanning_ = true;
                ESP_LOGI(TAG, "BLE scanning started (monitor UI active)");
            }
        } else if (!scan_wanted_ && scanning_) {
            esp_ble_gap_stop_scanning();
            scanning_ = false;
            ESP_LOGI(TAG, "BLE scanning stopped (monitor UI closed)");
        }
        vTaskDelay(pdMS_TO_TICKS(500));  // 500ms 轮询标志位
    }
}

// ---------- GAP 事件 ----------
void KeyboardMonitor::GapEventHandler(esp_gap_ble_cb_event_t event,
                                      esp_ble_gap_cb_param_t* param) {
    if (event == ESP_GAP_BLE_SCAN_RESULT_EVT &&
        param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
        GetInstance().HandleAdv(param->scan_rst);
    }
}

// ---------- 广播解析 + 过滤 ----------
void KeyboardMonitor::HandleAdv(
    const esp_ble_gap_cb_param_t::ble_scan_result_evt_param& rst) {
    // 过滤 1：MAC 绑定（一对一监听的主要手段，本协议载荷无键盘 ID 字段）
    if (has_target_mac_ && memcmp(rst.bda, target_mac_, 6) != 0) {
        return;
    }

    // ESP-IDF 6.x：扫描结果为单缓冲区 ble_adv（adv 数据 + scan rsp 拼接），
    // 长度由 adv_data_len / scan_rsp_len 给出
    const uint8_t* d = rst.ble_adv;
    uint8_t len = rst.adv_data_len;
    uint8_t i = 0;
    while (i < len) {
        uint8_t ad_len = d[i];
        if (ad_len == 0 || ad_len > len - i - 1) {
            break;
        }
        uint8_t ad_type = d[i + 1];
        if (ad_type == KM_ADV_TYPE_MANUFACTURER && ad_len >= KM_AD_LEN_MIN) {
            const uint8_t* p = d + i + 2;  // p[0..1]=company, p[2..25]=协议
            // 过滤 2：协议魔数
            if (p[0] != KM_COMPANY0 || p[1] != KM_COMPANY1 ||
                p[2] != KM_PROTO0 || p[3] != KM_PROTO1) {
                i += ad_len + 1;
                continue;
            }
            // 过滤 3：频道（Noirix44 st7789v 固定频道 1）
            if (p[25] != CONFIG_KEYBOARD_MONITOR_CHANNEL) {
                i += ad_len + 1;
                continue;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            status_.valid = true;
            status_.last_seen_us = esp_timer_get_time();
            status_.rssi = rst.rssi;
            status_.battery_left = p[5];
            status_.layer = p[6];
            status_.profile = p[7] & 0x07;
            status_.conn_count = p[8];
            status_.status_flags = p[9];
            status_.role = p[10];
            status_.side = p[11];
            status_.battery_right = p[12];
            status_.battery_aux = p[13];
            memcpy(status_.layer_name, p + 14, 4);
            status_.layer_name[4] = '\0';
            memcpy(status_.typed_keys, p + 18, 5);
            status_.typed_keys[5] = '\0';
            status_.mods = p[23];
            status_.wpm = p[24];
            status_.channel = p[25];
            // 广播日志（节流）：状态变化立即打印，无变化每 3 秒一条——
            // 键盘 active 时广播 200ms 一条，若不节流会刷屏淹没系统日志
            {
                static KeyboardStatus last_log = {};
                static int64_t last_log_us = 0;
                int64_t now_us = esp_timer_get_time();
                bool changed =
                    status_.battery_left != last_log.battery_left ||
                    status_.battery_right != last_log.battery_right ||
                    status_.layer != last_log.layer ||
                    status_.profile != last_log.profile ||
                    status_.wpm != last_log.wpm ||
                    status_.mods != last_log.mods ||
                    memcmp(status_.layer_name, last_log.layer_name, 5) != 0 ||
                    memcmp(status_.typed_keys, last_log.typed_keys, 6) != 0;
                if (changed || now_us - last_log_us > 3 * 1000 * 1000) {
                    ESP_LOGI(TAG,
                             "adv %02X:%02X:%02X:%02X:%02X:%02X ch=%d L=%d%% R=%d%% "
                             "wpm=%d layer='%s' keys='%s' rssi=%d",
                             rst.bda[0], rst.bda[1], rst.bda[2], rst.bda[3], rst.bda[4],
                             rst.bda[5], p[25], p[5], p[12], p[24], status_.layer_name,
                             status_.typed_keys, rst.rssi);
                    last_log = status_;
                    last_log_us = now_us;
                }
            }
            return;
        }
        i += ad_len + 1;
    }
}

// ---------- 启停 / 切换 ----------
void KeyboardMonitor::Start() {
    if (started_) {
        return;
    }
    started_ = true;
    if (!CONFIG_KEYBOARD_MONITOR_ENABLED) {
        ESP_LOGI(TAG, "keyboard monitor disabled by Kconfig");
        return;
    }
    if (strlen(CONFIG_KEYBOARD_MONITOR_TARGET_MAC) > 0) {
        ParseTargetMac(CONFIG_KEYBOARD_MONITOR_TARGET_MAC);
    }
    ESP_LOGI(TAG, "start: channel=%d mac=%s", CONFIG_KEYBOARD_MONITOR_CHANNEL,
             has_target_mac_ ? "bound" : "any");
    xTaskCreate(ScanTaskThunk, "km_scan", 4096, this, 5, &scan_task_handle_);
    // StartGesturePolling 移入 ScanTask（InitBle 前调用）——不在 main 线程碰 LVGL 锁
}

void KeyboardMonitor::RequestScanStop() {
    if (!started_) {
        return;
    }
    scan_wanted_ = false;  // 请求停扫（ScanTask 500ms 内执行）
}

void KeyboardMonitor::Stop() {
    if (!started_) {
        return;
    }
    scan_wanted_ = false;      // 请求停扫（ScanTask 500ms 内执行）
    if (scanning_) {
        esp_ble_gap_stop_scanning();
        scanning_ = false;
    }
    if (active_) {
        MonitorScreen::GetInstance().Hide();
        SetActive(false);
    }
    started_ = false;  // 允许休眠唤醒后 Start() 重新启动扫描任务
    ESP_LOGI(TAG, "stopped");
}

void KeyboardMonitor::Toggle() {
    if (!started_ || !CONFIG_KEYBOARD_MONITOR_ENABLED) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now - last_toggle_us_ < KM_DEBOUNCE_US) {
        return;  // 手势/语音防抖
    }
    last_toggle_us_ = now;
    SetActive(!active_);
    if (active_) {
        MonitorScreen::GetInstance().Show();
        scan_wanted_ = true;   // 切入监听界面 → 请求开扫（ScanTask 执行）
    } else {
        MonitorScreen::GetInstance().Hide();
        scan_wanted_ = false;  // 退出监听界面 → 请求停扫（ScanTask 执行）
    }
}

void KeyboardMonitor::SetActive(bool active) {
    active_ = active;
}

bool KeyboardMonitor::IsActive() const {
    return active_;
}

KeyboardStatus KeyboardMonitor::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void KeyboardMonitor::SetTargetMac(const uint8_t* mac) {
    if (mac) {
        memcpy(target_mac_, mac, 6);
        has_target_mac_ = true;
    } else {
        has_target_mac_ = false;
    }
}

// 解析 "AA:BB:CC:DD:EE:FF"
void KeyboardMonitor::ParseTargetMac(const char* str) {
    unsigned int b[6];
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3],
               &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            target_mac_[i] = uint8_t(b[i]);
        }
        has_target_mac_ = true;
        ESP_LOGI(TAG, "MAC filter bound");
    } else {
        ESP_LOGW(TAG, "invalid MAC filter: %s", str);
    }
}
