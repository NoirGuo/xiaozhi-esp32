#include "wifi_board.h"
#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"

#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"

#include <esp_lcd_touch_cst816s.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include "review_engine.h"

#define TAG "WaveshareEsp32s3TouchAMOLED1inch8"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);
    
        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V
        
        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x08); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }

    void EnablePowerButtonShortPressIrq() {
        ClearIrqStatus();
        WriteReg(0x41, ReadReg(0x41) | 0x08); // Enable AXP2101 PWRON short press IRQ
    }

    bool ConsumePowerButtonShortPressIrq() {
        uint8_t status = ReadReg(0x49);
        if (status != 0) {
            WriteReg(0x49, status);
        }
        return (status & 0x08) != 0;
    }

    void ClearIrqStatus() {
        WriteReg(0x48, 0xff);
        WriteReg(0x49, 0xff);
        WriteReg(0x4a, 0xff);
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600}, // Sleep out

    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x29, (uint8_t[]){0x00}, 0, 600},
};

// 在waveshare_amoled_1_8类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                    esp_lcd_panel_handle_t panel_handle,
                    int width,
                    int height,
                    int offset_x,
                    int offset_y,
                    bool mirror_x,
                    bool mirror_y,
                    bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle,
                    width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.1, 0);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255 * brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED1inch8 : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;
    TaskHandle_t power_button_task_handle_ = nullptr;
    bool returning_to_brookesia_ = false;

    static void PowerButtonTask(void* arg) {
        auto* self = static_cast<WaveshareEsp32s3TouchAMOLED1inch8*>(arg);
        while (true) {
            if (self->pmic_ != nullptr && self->pmic_->ConsumePowerButtonShortPressIrq()) {
                self->ReturnToBrookesia();
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        self->power_button_task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }

    void ReturnToBrookesia() {
        if (returning_to_brookesia_) {
            return;
        }
        returning_to_brookesia_ = true;

        const esp_partition_t *factory = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP,
            ESP_PARTITION_SUBTYPE_APP_FACTORY,
            nullptr
        );
        if (factory == nullptr) {
            ESP_LOGE(TAG, "Factory partition not found");
            returning_to_brookesia_ = false;
            return;
        }

        esp_err_t err = esp_ota_set_boot_partition(factory);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
            returning_to_brookesia_ = false;
            return;
        }

        ESP_LOGI(TAG, "PWR short press: return to Brookesia factory partition");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            pmic_->PowerOff();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(codec_i2c_bus_, I2C_ADDRESS, &io_expander);
        if(ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 |IO_EXPANDER_PIN_NUM_2, IO_EXPANDER_OUTPUT);
        ret |= esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
        ESP_ERROR_CHECK(ret);
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1|IO_EXPANDER_PIN_NUM_2, 1);
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(100));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1|IO_EXPANDER_PIN_NUM_2, 0);
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1|IO_EXPANDER_PIN_NUM_2, 1);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(codec_i2c_bus_, 0x34);
        pmic_->EnablePowerButtonShortPressIrq();
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = GPIO_NUM_11;
        buscfg.data0_io_num = GPIO_NUM_4;
        buscfg.data1_io_num = GPIO_NUM_5;
        buscfg.data2_io_num = GPIO_NUM_6;
        buscfg.data3_io_num = GPIO_NUM_7;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        xTaskCreate(PowerButtonTask, "pwr_button", 4096, this, 5, &power_button_task_handle_);
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = GPIO_NUM_39,
            .int_gpio_num = GPIO_NUM_13,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {};
        tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
        tp_io_config.scl_speed_hz = 400 * 1000;
        tp_io_config.control_phase_bytes = 1;
        tp_io_config.dc_bit_offset = 0;
        tp_io_config.lcd_cmd_bits = 8;
        tp_io_config.lcd_param_bits = 0;
        tp_io_config.flags.disable_control_phase = 1;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(codec_i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // 初始化工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });

        InitializeReviewTools(mcp_server);
    }

    // ===== 本地复习模块：注册 MCP 工具，由云端 LLM 在唤醒后调用 =====
    void InitializeReviewTools(McpServer& mcp_server) {

        // ===== 法规选择入口 =====

        // 列出全部法规（含分类、卡片数、掌握情况），供用户点名选择。
        mcp_server.AddTool("review.list_docs",
            "List all available laws/regulations (法规) with their category, card count and mastery "
            "level. Call this when the user wants to pick a specific regulation to review, or asks "
            "'有哪些法规' / '列出法规' / '我想指定一部法规复习'. After listing, wait for the user to "
            "choose, then call review.select_scope.",
            PropertyList(), [this](const PropertyList& properties) -> ToolResult {
                auto& engine = ReviewEngine::GetInstance();
                if (!engine.IsLoaded()) {
                    return std::unexpected(std::string("复习卡片尚未加载完成，请稍后再试"));
                }
                const auto& docs = engine.Docs();
                if (docs.empty()) {
                    return std::unexpected(std::string("目前没有任何法规数据"));
                }
                std::string out = "可选法规共 " + std::to_string(docs.size()) + " 部：";
                for (size_t i = 0; i < docs.size(); ++i) {
                    out += "\n" + std::to_string(i + 1) + ". " + engine.FormatDocBrief(docs[i]);
                }
                out += "\n可以直接说“复习安全生产法”，或说“搜索 危大工程”按关键词查找。";
                display_->ShowNotification(("可选法规 " + std::to_string(docs.size()) + " 部").c_str(), 2000);
                return out;
            });

        // 按名称关键字 / 分类 / 标签搜索筛选。
        mcp_server.AddTool("review.search_docs",
            "Search laws/regulations by name keyword, category or tag. All three are optional and "
            "combined with AND. Use when the user names a topic ('临时用电', '危大工程') or a "
            "category ('法律', '行政法规', '部门规范性文件', '行业标准').",
            PropertyList({
                Property("keyword", kPropertyTypeString, std::string("")),
                Property("category", kPropertyTypeString, std::string("")),
                Property("tag", kPropertyTypeString, std::string(""))
            }),
            [this](const PropertyList& properties) -> ToolResult {
                std::string keyword = properties["keyword"].value<std::string>();
                std::string category = properties["category"].value<std::string>();
                std::string tag = properties["tag"].value<std::string>();
                auto& engine = ReviewEngine::GetInstance();
                auto result = engine.SearchDocs(keyword, category, tag);
                if (result.empty()) {
                    // 空状态：回给可用的分类与标签，方便用户换条件重试。
                    std::string hint = "没有找到匹配的法规";
                    auto cats = engine.Categories();
                    if (!cats.empty()) {
                        hint += "。可用分类：";
                        for (size_t i = 0; i < cats.size(); ++i) {
                            if (i > 0) { hint += "、"; }
                            hint += cats[i];
                        }
                    }
                    return std::unexpected(hint);
                }
                std::string out = "找到 " + std::to_string(result.size()) + " 部法规：";
                for (size_t i = 0; i < result.size(); ++i) {
                    out += "\n" + std::to_string(i + 1) + ". " + engine.FormatDocBrief(result[i]);
                }
                return out;
            });

        // 列出可用的分类与标签（语音交互下的“筛选条件菜单”）。
        mcp_server.AddTool("review.list_filters",
            "List the available categories and tags that can be used to filter laws/regulations. "
            "Call this before searching when the user is not sure what to say.",
            PropertyList(), [this](const PropertyList& properties) -> ToolResult {
                auto& engine = ReviewEngine::GetInstance();
                std::string out = "可用分类：";
                auto cats = engine.Categories();
                if (cats.empty()) {
                    out += "（无）";
                } else {
                    for (size_t i = 0; i < cats.size(); ++i) {
                        if (i > 0) { out += "、"; }
                        out += cats[i];
                    }
                }
                out += "\n可用标签：";
                auto tags = engine.Tags();
                if (tags.empty()) {
                    out += "（无）";
                } else {
                    for (size_t i = 0; i < tags.size(); ++i) {
                        if (i > 0) { out += "、"; }
                        out += tags[i];
                    }
                }
                return out;
            });

        // 列出某法规的章节，支持细化到章节范围。
        mcp_server.AddTool("review.list_chapters",
            "List the chapters of a given law/regulation with their card counts, so the user can "
            "narrow the review down to a chapter. 'doc' can be a doc id ('AQF') or a name keyword "
            "('安全生产法').",
            PropertyList({Property("doc", kPropertyTypeString, std::string(""))}),
            [this](const PropertyList& properties) -> ToolResult {
                std::string doc = properties["doc"].value<std::string>();
                auto& engine = ReviewEngine::GetInstance();
                auto ids = engine.ResolveDocIds(doc);
                if (ids.empty()) {
                    return std::unexpected(std::string("没有找到这部法规，可以说“列出全部法规”查看可选范围"));
                }
                std::string out;
                for (size_t i = 0; i < ids.size() && i < 3; ++i) {
                    auto chapters = engine.ChaptersOf(ids[i]);
                    if (chapters.empty()) {
                        out += ids[i] + " 暂无章节划分。";
                        continue;
                    }
                    const auto& docs = engine.Docs();
                    std::string name = ids[i];
                    for (const auto& d : docs) {
                        if (d.id == ids[i]) { name = d.name; break; }
                    }
                    out += "《" + name + "》共 " + std::to_string(chapters.size()) + " 章：";
                    for (size_t j = 0; j < chapters.size(); ++j) {
                        out += "\n· " + chapters[j].first + "（" + std::to_string(chapters[j].second) + " 张）";
                    }
                    out += "\n";
                }
                return out;
            });

        // 确认选择并进入定向复习（支持多选与章节/条款关键字）。
        mcp_server.AddTool("review.select_scope",
            "Select a specific law/regulation (or several) as the review scope, optionally narrowed "
            "to a chapter or a keyword. 'docs' accepts one or more names/ids separated by comma or "
            "、 (multi-select); 'chapter' is a chapter keyword; 'keyword' matches card title, key "
            "points or recite text (use it for 条款 level narrowing). After this call, "
            "review.draw_card will only draw cards inside the selected scope. If 'docs' is empty, "
            "the scope is cleared and reviewing returns to the full deck.",
            PropertyList({
                Property("docs", kPropertyTypeString, std::string("")),
                Property("chapter", kPropertyTypeString, std::string("")),
                Property("keyword", kPropertyTypeString, std::string(""))
            }),
            [this](const PropertyList& properties) -> ToolResult {
                std::string docs_text = properties["docs"].value<std::string>();
                std::string chapter = properties["chapter"].value<std::string>();
                std::string keyword = properties["keyword"].value<std::string>();
                auto& engine = ReviewEngine::GetInstance();

                if (docs_text.empty() && chapter.empty() && keyword.empty()) {
                    engine.ClearScope();
                    return std::string("已取消定向复习，恢复全部法规。" +
                                       engine.FormatStats(engine.StatsForScope(), "全部法规"));
                }

                // 多选：用逗号/顿号分隔，逐项解析为 doc id 后取并集。
                std::vector<std::string> doc_ids;
                std::string current;
                auto flush = [&]() {
                    if (current.empty()) { return; }
                    auto resolved = engine.ResolveDocIds(current);
                    for (const auto& id : resolved) {
                        bool exists = false;
                        for (const auto& e : doc_ids) {
                            if (e == id) { exists = true; break; }
                        }
                        if (!exists) { doc_ids.push_back(id); }
                    }
                    current.clear();
                };
                for (size_t i = 0; i < docs_text.size(); ++i) {
                    unsigned char c = (unsigned char)docs_text[i];
                    // 中文顿号是 3 字节 UTF-8（E3 80 81），按字节序列判断。
                    if (c == ',' || c == ';' || c == ' ' || c == '/' ||
                        (i + 2 < docs_text.size() && c == 0xE3 &&
                         (unsigned char)docs_text[i + 1] == 0x80 &&
                         (unsigned char)docs_text[i + 2] == 0x81)) {
                        if (c == 0xE3) { i += 2; }
                        flush();
                    } else {
                        current += docs_text[i];
                    }
                }
                flush();

                if (!docs_text.empty() && doc_ids.empty()) {
                    return std::unexpected(std::string("没有找到这部法规，可以说“列出全部法规”查看可选范围"));
                }

                std::string error;
                if (!engine.SelectScope(doc_ids, chapter, keyword, &error)) {
                    // 空状态提示（含可用章节建议）。
                    return std::unexpected(error);
                }

                std::string scope_desc = engine.ScopeDescription();
                display_->ShowNotification(scope_desc.c_str(), 2500);
                return std::string("已选定复习范围：" + scope_desc + "。" +
                                   engine.FormatStats(engine.StatsForScope(), "该范围") +
                                   "。说“开始”即可抽卡。");
            });

        // 当前范围的进度与掌握情况。
        mcp_server.AddTool("review.scope_progress",
            "Report the review progress and mastery of the current scope: reviewed / unreviewed "
            "counts, wrong-answer count, accuracy and the last review time. Use when the user asks "
            "'我复习得怎么样了' or '这部法规的进度'.",
            PropertyList(), [this](const PropertyList& properties) -> ToolResult {
                auto& engine = ReviewEngine::GetInstance();
                std::string label = "全部法规";
                if (engine.HasScope()) {
                    label = engine.ScopeDescription();
                } else if (engine.InWrongMode()) {
                    label = "错题回顾";
                }
                return engine.FormatStats(engine.StatsForScope(), label);
            });

        // 退出定向复习。
        mcp_server.AddTool("review.exit_scope",
            "Exit the targeted review scope and go back to the full deck (or exit wrong-card review "
            "mode). Use when the user says '换一部法规' / '退出定向复习' / '回到全部'.",
            PropertyList(), [this](const PropertyList& properties) -> ToolResult {
                auto& engine = ReviewEngine::GetInstance();
                engine.ClearScope();
                engine.ExitWrongReview();
                return std::string("已回到全部法规。" +
                                   engine.FormatStats(engine.StatsForScope(), "全部法规"));
            });

        // 抽卡（顺序/随机）
        mcp_server.AddTool("review.draw_card",
            "Draw a review flashcard. When the user asks to start reviewing, draw a card, "
            "or continue with the next card, call this tool. Parameter 'random' (default false) "
            "controls whether to draw a random card (true) or the next card in order (false). "
            "Return the card's recite text (口播稿) so it can be read aloud to the user.",
            PropertyList({Property("random", kPropertyTypeBoolean, false)}),
            [this](const PropertyList& properties) -> ToolResult {
                bool random = properties["random"].value<bool>();
                auto& engine = ReviewEngine::GetInstance();
                const ReviewCard* card = nullptr;
                int index = 0, total = 0;
                if (!engine.NextCard(random, &card, &index, &total) || card == nullptr) {
                    return std::unexpected(std::string("当前范围内没有可复习的卡片，可以说“列出全部法规”重新选择"));
                }
                // 屏幕显示标题 + 要点
                ShowCardOnScreen(*card, index, total);
                // 返回 recite 文本，云端 LLM 会朗读。
                // 前置一行范围上下文，帮助 LLM 知道当前进度（不会原样朗读）。
                std::string prefix;
                if (engine.HasScope()) {
                    prefix = "[范围：" + engine.ScopeDescription() + " 第 " +
                             std::to_string(index + 1) + "/" + std::to_string(total) + " 张] ";
                } else if (engine.InWrongMode()) {
                    prefix = "[错题回顾 第 " + std::to_string(index + 1) + "/" +
                             std::to_string(total) + " 张] ";
                }
                return std::string(prefix + card->recite);
            });

        // 朗读当前卡口播稿
        mcp_server.AddTool("review.read_aloud",
            "Read aloud the current flashcard's recite text (口播稿). Use this when the user "
            "says '朗读' or '再来一遍'.",
            PropertyList(),
            [this](const PropertyList& properties) -> ToolResult {
                const ReviewCard* card = ReviewEngine::GetInstance().CurrentCard();
                if (card == nullptr) {
                    return std::unexpected(std::string("请先抽一张卡片"));
                }
                return std::string(card->recite);
            });

        // 显示要点/口诀/考点
        mcp_server.AddTool("review.show_details",
            "Show the current flashcard's details on screen: key points (要点), mnemonic (口诀), "
            "and exam point (考点). Returns the key points as text.",
            PropertyList(),
            [this](const PropertyList& properties) -> ToolResult {
                const ReviewCard* card = ReviewEngine::GetInstance().CurrentCard();
                if (card == nullptr) {
                    return std::unexpected(std::string("请先抽一张卡片"));
                }
                std::string result;
                result += card->title;
                if (!card->mnemonic.empty()) {
                    result += "\n【口诀】" + card->mnemonic;
                }
                if (!card->exam_point.empty()) {
                    result += "\n【考点】" + card->exam_point;
                }
                for (const auto& p : card->points) {
                    result += "\n· " + p;
                }
                display_->SetChatMessage("assistant", card->title.c_str());
                return result;
            });

        // 标记错题 / 我会了
        mcp_server.AddTool("review.mark_wrong",
            "Mark the current flashcard as a wrong-answer card (错题) or clear the mark. "
            "Use when the user says '标记错题' or '我不会' (mark=true), or '我会了' (mark=false).",
            PropertyList({Property("mark", kPropertyTypeBoolean, true)}),
            [this](const PropertyList& properties) -> ToolResult {
                const ReviewCard* card = ReviewEngine::GetInstance().CurrentCard();
                if (card == nullptr) {
                    return std::unexpected(std::string("请先抽一张卡片"));
                }
                bool mark = properties["mark"].value<bool>();
                ReviewEngine::GetInstance().MarkWrong(card->id, mark);
                return std::string(mark ? "已标记为错题" : "已标记为掌握");
            });

        // 错题回顾
        mcp_server.AddTool("review.review_wrong",
            "Start reviewing only the wrong-answer cards (错题回顾). Use when the user asks to "
            "review their mistakes.",
            PropertyList(),
            [this](const PropertyList& properties) -> ToolResult {
                auto& engine = ReviewEngine::GetInstance();
                if (!engine.StartWrongReview()) {
                    return std::unexpected(std::string("当前没有错题，继续保持"));
                }
                const ReviewCard* card = nullptr;
                int index = 0, total = 0;
                if (!engine.NextCard(false, &card, &index, &total) || card == nullptr) {
                    return std::unexpected(std::string("当前没有错题"));
                }
                ShowCardOnScreen(*card, index, total);
                return std::string("错题回顾开始，共 " + std::to_string(total) + " 题：" + card->recite);
            });

        // 查询进度（沿用旧工具名，统计已升级为“当前范围”的完整进度）
        mcp_server.AddTool("review.get_progress",
            "Get the review progress of the current scope. Same as review.scope_progress.",
            PropertyList(),
            [this](const PropertyList& properties) -> ToolResult {
                auto& engine = ReviewEngine::GetInstance();
                std::string label = "全部法规";
                if (engine.HasScope()) {
                    label = engine.ScopeDescription();
                } else if (engine.InWrongMode()) {
                    label = "错题回顾";
                }
                return engine.FormatStats(engine.StatsForScope(), label);
            });
    }

    // 把卡片标题与要点显示到屏幕（底部字幕栏 + 通知）
    void ShowCardOnScreen(const ReviewCard& card, int index, int total) {
        std::string title_line = card.id + " " + card.title;
        display_->SetChatMessage("assistant", title_line.c_str());
        // 用通知栏展示进度
        std::string progress = "卡片 " + std::to_string(index + 1) + "/" + std::to_string(total);
        display_->ShowNotification(progress.c_str(), 2000);
    }

public:
    WaveshareEsp32s3TouchAMOLED1inch8() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeTca9554();
        InitializeAxp2101();
        InitializeSpi();
        InitializeDisplay();
         InitializeTouch();
        InitializeButtons();
        InitializeTools();

        // 初始化本地复习引擎（解析嵌入的 cards.json）
        if (!ReviewEngine::GetInstance().Initialize(nullptr, 0)) {
            ESP_LOGE(TAG, "Failed to initialize review engine");
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED1inch8);
