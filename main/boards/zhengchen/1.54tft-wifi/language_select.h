#pragma once

#include <functional>

#include <lvgl.h>

// ============================================================
// LanguageSelect —— 首次开机语言自选界面（240×240 直角屏）
// 交互：音量+/- 循环切换高亮语言，BOOT 键单击确认
// 每次切换播放对应语言提示音"按 Boot 键确认"（common/ 内嵌 ogg）
// 确认后写入 NVS（Settings user/language），此后开机不再弹出
// 所有 LVGL 操作均通过 DisplayLockGuard 加锁（可在非 LVGL 线程调用）
// ============================================================
class LanguageSelect {
public:
    static LanguageSelect& GetInstance();

    void LoadPersisted();   // 板构造时调用：读 NVS，有记录则 SetLanguage（UI 创建前）
    bool CheckAndShow();    // audio 就绪后调用：无记录则显示选择界面；返回是否显示
    bool IsActive() const { return active_; }

    // 电源钩子：界面激活时禁用省电休眠（防 60s 黑屏），确认后恢复
    using PowerSaveHook = std::function<void(bool enable)>;
    void SetPowerSaveHook(PowerSaveHook hook) { power_save_hook_ = std::move(hook); }

    void Move(int dir);     // 音量键：+1 下移 / -1 上移（循环）
    void Confirm();         // BOOT 键：写入 NVS 并生效，退出选择界面

private:
    LanguageSelect() = default;
    ~LanguageSelect() = default;
    LanguageSelect(const LanguageSelect&) = delete;
    LanguageSelect& operator=(const LanguageSelect&) = delete;

    void Show();
    void Hide();
    void UpdateHighlight();
    void PlayPrompt();

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* prev_screen_ = nullptr;
    lv_obj_t* item_row_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* item_lbl_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    PowerSaveHook power_save_hook_;
    int current_ = 0;
    bool active_ = false;
};
