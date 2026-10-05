（中文 | [English](README_en.md)）

# xiaozhi-esp32 定制版 · 小智 AI + Noirix44 键盘监听（1.54TFT）

基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的定制固件，在保留小智 AI 全部功能的基础上，额外提供三项能力：

- **Noirix44 键盘状态实时监听**：BLE 广播 → 屏幕显示 WPM、层名、修饰键、电量
- **首次开机自选语言**：6 种语言，Boot 键确认
- **双唤醒词常驻**：中文「你好小智」+ 英文「Hi ESP」

---

## 硬件

![ESP32-S3 开发板](./docs/board-esp32s3.jpg)

| 部件 | 规格 |
|---|---|
| 主控 | ESP32-S3（8MB Flash / 8MB PSRAM） |
| 屏幕 | 1.54 英寸 TFT 直角屏（ST7789，240×240，无触摸） |
| 音频 | I2S 数字麦克风 + 喇叭 |
| 按键 | Boot（GPIO0）、音量+（GPIO10）、音量-（GPIO39） |

---

## 功能说明

### 1. 小智 AI（原版全部能力）

语音对话、配网、OTA 升级、MCP 工具等，与上游原版一致，未做删减。

### 2. 键盘状态监听

不建立 BLE 连接、不占用键盘连接槽，以 observer 模式接收键盘广播。

**屏幕布局（240×240）：**

| 区域 | 内容 |
|---|---|
| 顶部 | USB / BLE 连接状态（哪个生效哪个点亮） |
| 上部 | WPM 实时折线图（最近约 30 秒，纵轴上限 150，不显示坐标刻度） |
| 中部 | 层名大字（60px 粗体，超宽自动缩小到 48/36px） |
| 中部偏下 | 4 个修饰键芯片：CTRL / SHIFT / ALT / GUI（按下高亮变色） |
| 底部 | L / M / R 三设备电量条（>30% 绿 / 10–30% 黄 / ≤10% 红） |

**协议**：Prospector v2.2——键盘侧需刷 `zmk-config-Noirix44` 的 `monitor` 分支，广播 26 字节厂商载荷（FF FF 头 + AB CD 协议标识 + 频道 + 层名 + 修饰键 + WPM + 三电量等）。

**一对一监听（可选项）**：

- `CONFIG_KEYBOARD_MONITOR_TARGET_MAC`：绑定键盘 MAC 地址，只显示该键盘
- `CONFIG_KEYBOARD_MONITOR_TARGET_ID`：绑定键盘硬件 ID（HWINFO，8 位十六进制），更强的身份校验
- `CONFIG_KEYBOARD_MONITOR_CHANNEL`：频道过滤（默认启用）
- 三者均留空时按频道过滤工作，多键盘同频道会混显

**扫描策略**：BLE 栈开机即就绪（常开、射频空闲），**仅在监听界面打开时启动扫描**；休眠时自动停扫并隐藏界面，唤醒自动恢复。

### 3. 首次开机语言自选

| 语言 | 显示名 |
|---|---|
| 简体中文 | Chinese |
| 菲律宾语 | Filipino |
| 越南语 | Vietnamese |
| 韩语 | Korean |
| 马来语 | Malay |
| 英语 | English |

- 仅首次开机（存储中无语言记录）自动出现，界面**不超时**
- **音量+ 下移、音量- 上移**循环选择，**Boot 键确认**
- 每次切换用该语言语音播报「按 Boot 键确认」
- 确认后写入存储并立即生效，之后开机直接进入所选语言
- 语言选择界面激活期间，音量键临时接管为切换语言，正常音量调节暂停

> 注意：语言自选只切换**设备界面显示语言**。语音对话仍按原版机制工作：正式使用需在小智控制台配置对应语言的语音包。另外官方唤醒词模型仅提供中/英/日/法/德/西/意，**韩/越/马/菲四语言暂无官方唤醒词模型**（界面翻译不受影响）。

### 4. 双唤醒词

开机即常驻两个唤醒词模型：

- 中文：「你好小智」
- 英文：「Hi ESP」

---

## 基础操作

| 操作 | 按键 / 说法 | 说明 |
|---|---|---|
| 音量调节 | 单击 音量+ / 音量- | 每次 ±10% |
| 最大音量 | 长按 音量+ | 直接 100% |
| 静音 | 长按 音量- | 直接 0% |
| 打开键盘监听 | 双击 音量+ | 或对小智说「打开键盘监控」「查看键盘状态」 |
| 退出键盘监听 | 再次双击 音量+ / 语音关闭 | 或等待自动返回（30 秒无键盘数据，可配置） |
| 唤醒对话 | 说「你好小智」/「Hi ESP」 | |
| 配网 | 手机连接设备热点，按小智控制台指引 | 与原版流程一致 |

监听界面打开时，单击/长按音量键的调节功能不受影响。

---

## 键盘侧准备

1. 给键盘刷入 `zmk-config-Noirix44` 的 `monitor` 分支固件（启用 Prospector v2.2 广播）
2. 可选：在 `idf.py menuconfig` → 板相关配置中填写：
   - `CONFIG_KEYBOARD_MONITOR_TARGET_MAC`（键盘 MAC）
   - `CONFIG_KEYBOARD_MONITOR_TARGET_ID`（键盘 HWINFO 8 位十六进制）
   - `CONFIG_KEYBOARD_MONITOR_TIMEOUT_SEC`（自动返回秒数，默认 30）
   - `CONFIG_KEYBOARD_MONITOR_CHANNEL`（频道，需与键盘侧一致）

---

## 编译与烧录

### GitHub Actions（推荐）

- 推送 `ci/st7789-1.54-tft` 分支自动触发构建
- 板文件（`main/boards/zhengchen/1.54tft-wifi/`）变更会构建 zhengchen 板；仅公共文件变更只构建代表板
- 也可在 Actions 页面手动运行 workflow（全量构建）
- 固件产物从构建产物的 Artifacts 下载

### 本地编译

```bash
idf.py set-target esp32s3
idf.py menuconfig
# Xiaozhi Assistant → Board Type → zhengchen-1.54tft-wifi
idf.py build
idf.py -p <串口> flash monitor
```

烧录提示：若升级时出现启动异常，尝试烧录时不擦除设备（保留校准/NVS 数据）。

---

## 目录结构（相关部分）

```
main/boards/zhengchen/1.54tft-wifi/
├── zhengchen-1.54tft-wifi.cc    # 板初始化：按键、屏幕、监听/语言选择挂钩
├── monitor_screen.{h,cc}        # 监听界面布局与刷新
├── keyboard_monitor.{h,cc}      # BLE 扫描、Prospector v2.2 解析、按需扫描
├── language_select.{h,cc}       # 首次开机语言自选
├── config.h / config.json       # 引脚定义 / CI 构建配置
└── lv_font_montserrat_bold_*.c  # 层名用自生成粗体字体
scripts/gen_lang.py              # 运行时多语言头文件生成器
```

---

## 常见问题

**Q：语言选择界面再次出现？**
清除设备 NVS 即可（烧录时勾选擦除 flash，或恢复出厂设置）。

**Q：屏幕一直白屏/启动反复重启？**
先确认串口日志。常见原因：烧录时擦除了校准数据。重新烧录（不擦除）通常可恢复；确保持续日志中无 `Factory partition not found` 相关异常。

**Q：监听界面收不到键盘数据？**
1. 确认键盘已刷 monitor 分支固件（v2.2 广播）
2. 确认频道（CHANNEL）与键盘一致
3. 若配置了 MAC / HWINFO 绑定，确认与键盘实际值一致
4. 监听界面打开时（顶部显示 BLE 亮起）再观察

**Q：多个键盘同时广播会怎样？**
未配置一对一绑定时按频道过滤，同频道键盘会混显；配置 MAC 或 HWINFO 绑定后只显示指定键盘。

---

## 许可

遵循上游 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的开源许可。
