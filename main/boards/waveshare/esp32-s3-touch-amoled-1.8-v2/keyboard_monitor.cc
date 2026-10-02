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
static void InitBle() {
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(&KeyboardMonitor::GapEventHandler));
    ESP_LOGI(TAG, "BLE initialized");
}

// ---------- 扫描任务 ----------
void KeyboardMonitor::ScanTaskThunk(void* arg) {
    static_cast<KeyboardMonitor*>(arg)->ScanTask();
}

void KeyboardMonitor::ScanTask() {
    vTaskDelay(pdMS_TO_TICKS(1500));  // 等系统/音频就绪
    InitBle();

    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&kScanParams));
    esp_err_t err = esp_ble_gap_start_scanning(0);  // 0 = 持续扫描
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start scanning failed: %s", esp_err_to_name(err));
        return;
    }
    scanning_ = true;
    ESP_LOGI(TAG, "BLE scanning started");

    while (scanning_) {
        vTaskDelay(pdMS_TO_TICKS(1000));
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
            // 广播日志：打印 MAC 与关键数据（需要绑定 TARGET_MAC 时从中取地址）
            ESP_LOGI(TAG,
                     "adv %02X:%02X:%02X:%02X:%02X:%02X ch=%d L=%d%% R=%d%% "
                     "wpm=%d layer='%s' keys='%s' rssi=%d",
                     rst.bda[0], rst.bda[1], rst.bda[2], rst.bda[3], rst.bda[4],
                     rst.bda[5], p[25], p[5], p[12], p[24], status_.layer_name,
                     status_.typed_keys, rst.rssi);
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
    MonitorScreen::GetInstance().StartGesturePolling();
}

void KeyboardMonitor::Stop() {
    if (!started_) {
        return;
    }
    scanning_ = false;
    esp_ble_gap_stop_scanning();
    if (active_) {
        MonitorScreen::GetInstance().Hide();
        SetActive(false);
    }
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
    } else {
        MonitorScreen::GetInstance().Hide();
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
//（注：内容由AI生成）
