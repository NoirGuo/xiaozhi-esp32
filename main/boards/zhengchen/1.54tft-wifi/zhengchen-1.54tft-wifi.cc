#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "zhengchen_lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "power_save_timer.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "power_manager.h"
#include "mcp_server.h"
#include "review_engine.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#include <atomic>

#define TAG "ZHENGCHEN_1_54TFT_WIFI"

// 微信对话样式下，正文由 LVGL 按气泡宽度自动折行（LV_LABEL_LONG_WRAP），
// 超出一屏的部分用 VOL± 翻页查看，因此这里不再做"按字符数截断"。
// 原先的 10 字截断只适用于三段式单行横向滚动字幕，会把卡片标题切残。

// 长按判定阈值：轻点仍是音量 ±10；按住超过该阈值才开始翻页，
// 同时让 button 组件把这次按压归类为 long press，松手时不会再补发 click
// （否则"按住翻页"会顺带把音量 +10）。
static constexpr uint16_t kVolumeLongPressMs = 600;
// 按住后多久开始翻页、之后每页间隔
static constexpr int64_t kScrollStartDelayUs = 600 * 1000;
static constexpr int64_t kScrollRepeatIntervalUs = 300 * 1000;

class ZHENGCHEN_1_54TFT_WIFI : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    ZHENGCHEN_LcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    // 按住 VOL± 连续翻页：0 = 停止，-1 = 上翻（看更早），+1 = 下翻（看更新）
    esp_timer_handle_t chat_scroll_timer_ = nullptr;
    std::atomic<int> chat_scroll_direction_{0};
    // 本次按压期间是否真的翻过页：翻过则松手时吞掉 click，避免"翻页顺手改音量"
    std::atomic<bool> chat_scroll_moved_{false};

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_9);
        power_manager_->OnTemperatureChanged([this](float chip_temp) {
            display_->UpdateHighTempWarning(chip_temp);
        });

        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
                ESP_LOGI("PowerManager", "Charging started");
            } else {
                power_save_timer_->SetEnabled(true);
                ESP_LOGI("PowerManager", "Charging stopped");
            }
        });
    
    }

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_2);
        rtc_gpio_set_direction(GPIO_NUM_2, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_2, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // ---------------- 聊天区翻页（按住 VOL±） ----------------
    static void ChatScrollTimerCallback(void* arg) {
        static_cast<ZHENGCHEN_1_54TFT_WIFI*>(arg)->OnChatScrollTick();
    }

    void InitializeChatScrollTimer() {
        esp_timer_create_args_t args = {};
        args.callback = &ZHENGCHEN_1_54TFT_WIFI::ChatScrollTimerCallback;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "chat_scroll";
        ESP_ERROR_CHECK(esp_timer_create(&args, &chat_scroll_timer_));
    }

    // 定时器任务上下文：翻一页；翻不动（到顶/到底/无内容）就停并给一次提示
    void OnChatScrollTick() {
        int direction = chat_scroll_direction_.load();
        if (direction == 0) {
            return;
        }
        if (!display_->ScrollChatPage(direction)) {
            chat_scroll_direction_.store(0);
            if (!display_->IsChatScrollable()) {
                GetDisplay()->ShowNotification("当前没有可滚动内容", 1200);
            } else {
                GetDisplay()->ShowNotification(direction < 0 ? "已到最早一条" : "已到最新一条", 1200);
            }
            return;
        }
        chat_scroll_moved_.store(true);
        // 仍按住 → 继续下一屏（连翻），松手时由 StopChatScroll 取消
        esp_timer_start_once(chat_scroll_timer_, kScrollRepeatIntervalUs);
    }

    void StartChatScroll(int direction) {
        power_save_timer_->WakeUp();
        chat_scroll_moved_.store(false);
        chat_scroll_direction_.store(direction);
        esp_timer_stop(chat_scroll_timer_);
        esp_timer_start_once(chat_scroll_timer_, kScrollStartDelayUs);
    }

    // 把"本次按压"标记为已消费：翻过页就返回 true，调用方据此跳过音量调节
    bool ConsumeScrollClick() {
        return chat_scroll_moved_.exchange(false);
    }

    void StopChatScroll() {
        chat_scroll_direction_.store(0);
        esp_timer_stop(chat_scroll_timer_);
    }

    void InitializeButtons() {
        
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // 设置开机按钮的长按事件（直接进入配网模式）
        boot_button_.OnLongPress([this]() {
            // 唤醒电源保存定时器
            power_save_timer_->WakeUp();
            // 获取应用程序实例
            auto& app = Application::GetInstance();
            
            // 进入配网模式
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            
            // 重置WiFi配置以确保进入配网模式
            EnterWifiConfigMode();
        });

        // 轻点：音量 ±10（原行为不变）
        volume_up_button_.OnClick([this]() {
            if (ConsumeScrollClick()) {
                return;  // 这次按压已经用于翻页，别再改音量
            }
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
        });

        volume_down_button_.OnClick([this]() {
            if (ConsumeScrollClick()) {
                return;
            }
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
        });

        // 双击：最大音量 / 静音（原"长按"功能迁到双击，避免按住翻页时丢功能）
        volume_up_button_.OnDoubleClick([this]() {
            chat_scroll_moved_.store(false);
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnDoubleClick([this]() {
            chat_scroll_moved_.store(false);
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        // 按住 ≥600ms：上/下翻聊天记录（按住连翻，松手即停）
        volume_up_button_.OnPressDown([this]() { StartChatScroll(-1); });
        volume_up_button_.OnPressUp([this]() { StopChatScroll(); });
        volume_down_button_.OnPressDown([this]() { StartChatScroll(1); });
        volume_down_button_.OnPressUp([this]() { StopChatScroll(); });
    }

    void InitializeSt7789Display() {
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));

        display_ = new ZHENGCHEN_LcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        display_->SetupHighTempWarningPopup();
    }

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
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
                // 微信对话样式按气泡宽度自动折行，标题完整显示（返回给 LLM 的文本不变）
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

    // 把卡片标题显示到屏幕（微信对话样式的聊天气泡 + 进度通知）
    void ShowCardOnScreen(const ReviewCard& card, int index, int total) {
        // 标题完整入气泡，由 LVGL 折行；超出部分用 VOL± 翻页查看
        std::string title_line = card.id + " " + card.title;
        display_->SetChatMessage("assistant", title_line.c_str());
        // 用通知栏展示进度
        std::string progress = "卡片 " + std::to_string(index + 1) + "/" + std::to_string(total);
        display_->ShowNotification(progress.c_str(), 2000);
    }

public:
    ZHENGCHEN_1_54TFT_WIFI() :
        boot_button_(BOOT_BUTTON_GPIO),
        // 音量键显式给 600ms 长按阈值：按住 ≥600ms 归为 long press，
        // 松手不会再补发 click，避免"翻页时顺手改了音量"
        volume_up_button_(VOLUME_UP_BUTTON_GPIO, false, kVolumeLongPressMs),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO, false, kVolumeLongPressMs) {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeChatScrollTimer();
        InitializeButtons();
        InitializeSt7789Display();  
        InitializeTools();
        GetBacklight()->RestoreBrightness();

        // 初始化本地复习引擎（解析嵌入的 cards.json）
        if (!ReviewEngine::GetInstance().Initialize(nullptr, 0)) {
            ESP_LOGE(TAG, "Failed to initialize review engine");
        }
    }

    // 获取音频编解码器
    virtual AudioCodec* GetAudioCodec() override {
        // 静态实例化NoAudioCodecSimplex类
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        // 返回音频编解码器
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = std::max<uint32_t>(power_manager_->GetBatteryLevel(), 20);
        return true;
    }

    virtual bool GetTemperature(float& esp32temp)  override {
        esp32temp = power_manager_->GetTemperature();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(ZHENGCHEN_1_54TFT_WIFI);
