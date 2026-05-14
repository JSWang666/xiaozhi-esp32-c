# C 语言重构遗留问题清单

**审查范围**：187 个新转换的 C 板子文件 + 核心 C/C++ 桥接代码  
**审查方式**：（1）抽样编译 5 个代表板子；（2）6 个子代理对全部板子做 .cc HEAD vs .c 现状的静态差异分析。  
**结论**：当前只有 **`bread-compact-wifi`** 经过编译+硬件验证。**至少 80% 的其他板子存在不同程度的问题**，从「编译失败」到「编译通过但功能缺失」到「编译通过但运行时崩溃」都有。

---

## 一、严重程度分类

### 🔴 编译失败（无法生成固件，已实测确认）

> **2026-05-13 更新**：下列 5 个板子中，**前 4 个**已在主线中修复并通过 `ninja -C build` 验证；**`waveshare/esp32-s3-lcd-0.85`** 已于后续提交完成 C 头文件拆分、`config.h` codec 宏与 WiFi 配网入口，并由 `release.py` / CI 矩阵覆盖。

| 板子 | 错误类型 | 状态 |
|---|---|---|
| `lichuang-dev` | 缺 `#include "led/led.h"`（`led_t`），codec 宏需 `es8311_codec.h` | **已修** |
| `esp-box-3` | 缺 `esp_lcd_panel_io.h`、panel ops、ES8311/ES7210 头；`device_state.h` 已补 | **已修** |
| `kevin-sp-v3-dev` | 误用 `board_btn_handle_t`，应为 `board_btn_t*` + `board_btn_gpio_cfg_t` | **已修** |
| `xingzhi-metal-1.54-wifi` | `cst816x.h` 为 C++；主 `.c` 缺 SPI panel IO / ES8311 头 | **已修**（`cst816x.h` 改为纯 C 声明） |
| `waveshare/esp32-s3-lcd-0.85` | `led_strip_control.h` / `power_manager.h` 曾为 C++ | **已修**：`power_manager_legacy.hpp` + C 安全 `power_manager.h`；`led_strip_control.h` 纯 C 桩；`config.h` 补 codec 头；主 `.c` 补 `led/led.h` 与 WiFi 配网入口 |

**附加修复（同轮）**：`esp-sparkbot` 缺 LCD/codec 头；`df-k10` / `esp-hi` 的 MCP 工具回调误用不存在的 `mcp_property_*` API，已改为 `cJSON` + `mcp_tool_param_t`。

### 🟠 编译通过但有架构性 ABI Bug（运行时必崩）

#### Bug 1：`Backlight` / `Camera` 强制类型转换错误

**Backlight（已修）**：`c_board_wrapper.cc` 使用非拥有的 `CBacklightAdapter` 包装 C 返回的 `backlight_t*`；各板 `destroy()` 中统一调用 `backlight_destroy(ctx->backlight)`，与 `~CBoardDelegate` 中「先删适配器再调 `desc_->destroy`」的顺序一致，避免双重释放或泄漏。

**Camera**：若仍存在将 `camera_t*` 误当 C++ `Camera*` 的路径，需单独按 Display 同款 unwrap/adapter 处理（当前 `c_board_wrapper.cc` 中已无 `GetCamera` 相关 `static_cast` 片段则以此为准）。

**受影响范围**：约 89 个板子设置了 `.get_backlight`（基本所有带 LCD 屏的板子）；摄像头相关以当前代码为准。

#### Bug 2：`BOARD_KIND_DUAL` 与 ML307 引脚配置

`create_board()` 已对 `BOARD_KIND_DUAL` 走 `CBoardWrapper<DualNetworkBoard>`（见 `c_board_wrapper.cc`），但此前多块双网板在 `board_desc_t` 上**未填写** `modem_tx_pin` / `modem_rx_pin` / `modem_dtr_pin` / `default_net_type`（`calloc` 默认为 0），导致 ML307 使用错误 GPIO 或默认网络类型与 `DualNetworkBoard` 预期不一致。

**已修**：所有 `BOARD_KIND_DUAL` 板在 `create_board_desc()` 中写入与 `config.h` 一致的 ML307（或 4G 模块）UART 引脚，并设 `default_net_type = 1`（与 `DualNetworkBoard` 构造函数默认一致，即缺省设置下优先蜂窝）。

涉及板型包括：`magiclick-2p5`、`minsi-k08-dual`、`xingzhi-abs-2.0`、`nulllab-ai-vox-v3`、`zhengchen-cam-ml307`、`atk-dnesp32s3-box2-4g`、`kevin-box-2`、`xingzhi-cube-1.54tft-ml307`、`xingzhi-cube-0.96oled-ml307`、`yunliao-s3` 等。

#### Bug 3：`CAudioCodecAdapter` 没转发音量/增益/Start

**已修**：`c_board_wrapper.cc` 中 `CAudioCodecAdapter` 重写 `Start` / `SetOutputVolume` / `SetInputGain`，同步 C 结构体字段并调用 `audio_codec_t` 的 ops（或 `audio_codec_base_*` 回退）。

#### Bug 4：`audio_codec_destroy()` 无限递归

**已修**：`audio_codec.c::audio_codec_destroy` 在调用 `ops->destroy` 前将 `codec->ops` 置空，板级 destroy 内再次调用 `audio_codec_destroy` 时变为 no-op，避免与 `sensecap_audio_codec`、`k10_audio_codec`、`tab5`/`cores3`、lilygo PDM 等实现互递归。

#### Bug 5：`led_cpp_bridge.cc` 丢失参数

```c
static void circular_strip_on_state(led_t *led, DeviceState state, bool voice) {
    auto *w = reinterpret_cast<circular_strip_wrapper *>(led);
    if (w && w->impl) w->impl->OnStateChanged();  // 丢了 state, voice 参数
}
```

`CircularStrip`/`GpioLed` 不再能根据设备状态变 LED 颜色。

#### Bug 6：`protocol_cpp_bridge.cc` 指针寿命

`OnIncomingAudio` 把 `unique_ptr` 持有的内存指针传给 C 回调，C 端来不及拷贝就被 free，**音频数据可能损坏**。

#### Bug 7：`audio_service_init` 不安全 cast

**已修**：`audio_cpp_bridge.cc::audio_service_init` 通过 `codec_unwrap_cpp(audio_codec_t*)`（定义于 `codec_cpp_bridge.cc`）识别由 `es8311_codec_create` 等桥接创建的 facade；若 `ops` 非桥接表则拒绝初始化并返回 `ESP_ERR_INVALID_ARG`，不再对裸指针 `static_cast<AudioCodec*>`。

---

### 🟡 编译通过但功能缺失（运行不崩，但行为不对）

#### 摄像头模块丢失 - 19 个板子

原 C++ 用了 `Esp32Camera` / `EspVideo` 并暴露 `GetCamera()`，转 C 时摄像头初始化代码删除或没接入 `board_desc_t->get_camera`：

`bread-compact-wifi-s3cam`、`atoms3r-cam-m12-echo-base`、`atk-dnesp32s3`、`atk-dnesp32s3-box3`、`df-k10`、`df-s3-ai-cam`、`esp32s3-korvo2-v3`（含 rndis 版）、`esp-p4-function-ev-board`、`esp-s3-lcd-ev-board` v1/v2、`esp-sparkbot`、`esp-vocat`、`kevin-sp-v3-dev`、`kevin-sp-v4-dev`、`lichuang-dev`、`lilygo-t-cameraplus-s3`、`m5stack-cardputer-adv`、`m5stack-core-s3`、`m5stack-tab5`、`otto-robot`、`rymcu/bigsmart`、`waveshare/esp32-s3-cam`（讽刺：名字带 cam 但没摄像头）、`zhengchen-cam`、`zhengchen-cam-ml307`

#### 触摸屏丢失 - 30+ 个板子

触摸控制器（CST816/FT5x06/GT911/GT1151）没初始化或没接入 LVGL：

- `esp-sensairshuttle`、`esp-vocat`、`freenove-esp32s3-display-2.8-lcd`
- `lilygo-t-cameraplus-s3`、`m5stack-tab5`（触摸建了不用）
- `sp-esp32-s3-1.28-box`、`taiji-pi-s3`、`wireless-tag-wtp4c5mp07s`
- `waveshare/esp32-c6-touch-amoled-1.32` / 1.43 / 2.06
- `waveshare/esp32-s3-touch-amoled-1.32` / 1.43c / 1.75 / 1.8 / 2.06 / 2.16
- `waveshare/esp32-s3-touch-lcd-1.46` / 1.54 / 1.83 / 1.85 / 1.85c / 3.49 / 3.5
- 共 ~30 个

#### PowerSaveTimer（CPU 降频/屏幕休眠）丢失 - 40+ 个板子

HEAD 里 61 个板子用，现在只 22 个保留。丢失的板子：电池续航大幅下降。

详细名单见各小组报告，主要有：`aipi-lite`、`atk-dnesp32s3-box*`（除 box）、`du-chatx`、`electron-bot`、`esp32s3-korvo2-v3*`、`esp-sparkbot`、`genjutech`、`jiuchuan-s3`、`kevin-sp-v3/v4-dev`、`lilygo-*` 系列、`m5stack-cardputer-adv`、`minsi-k08-dual`、`movecall-moji2`、`sp-esp32-s3-*`、`surfer-c3-1.14tft`、`taiji-pi-s3`、`xingzhi-abs-2.0`、`xingzhi-metal-1.54-wifi`、`xmini-c3*`、`yunliao-s3`、`zhengchen-1.54tft-wifi`、`waveshare/esp32-s3-lcd-0.85`

#### SleepTimer（深度睡眠）丢失 - 2 个板子

- `esp-spot`：IMU 触发深度睡眠的完整逻辑没了
- `xmini-c3-4g`

#### Axp2101 / Sy6970 PMIC 丢失 - 8+ 个板子

电源管理芯片驱动没移植：`atk-dnesp32s3-box0` / `box2-4g` / `box2-wifi` / `box3`、`du-chatx`、`electron-bot`、`tudouzi`、`waveshare/esp32-c6-touch-amoled-2.06`（裸操作寄存器代替）

#### AdcBatteryMonitor 丢失 - 4+ 个板子

`movecall-moji2-esp32c5`、`xmini-c3-4g`、`xmini-c3-v3`

#### 自定义 Display 子类丢失（功能严重退化）

- `electron-bot` - `ElectronEmojiDisplay` → 用通用 SPI LCD 代替
- `esp-hi` - `anim::EmojiWidget` → 没有动画表情显示
- `esp-sensairshuttle` - 条件编译的 `EmoteDisplay`
- `jiuchuan-s3` - `CustomLcdDisplay`
- `xingzhi-abs-2.0` - `CustomLcdDisplay`
- `zhengchen-1.54tft-wifi` - `ZhengchenLcdDisplay`

#### MCP 工具丢失（语音指令缺失）

- `bread-compact-esp32` - lamp 工具
- `esp-hi` - 部分 robot 工具
- `esp-sparkbot` - `light_mode` / `set_camera_flipped`
- `kevin-c3` - LED strip 的 `blink` / `scroll`（变成空函数）
- `lichuang-dev` - `reconfigure_wifi`
- `movecall-moji2-esp32c5` - `PressToTalk`
- `rymcu/bigsmart` - `reconfigure_wifi`
- `sensecap-watcher` - SSCMA 模型参数/启停工具
- `waveshare/esp32-s3-lcd-0.85` - LED strip 工具、`reconfigure_wifi`
- `xmini-c3` - `PressToTalk`
- `yunliao-s3` - `set_aec` / `switch_TFT`

#### WiFi 配网入口丢失 - 几乎全部板子

原板子的 boot 长按/单击在 `kDeviceStateStarting` 时调 `EnterWifiConfigMode()`。新 C 代码统一改成 `if (state == kDeviceStateStarting) return;` —— **大约 60 个板子的 WiFi 配网入口失效**。`bread-compact-wifi` 能用应该是因为系统自动重试机制，但用户体验跟原版不一样。

**根本原因**：C API 里没暴露 `app_enter_wifi_config_mode()` 这个调用，subagent 们绕开了。

---

### 🟢 高概率正常工作的板子

下面这些板子的 .c 实现与 HEAD .cc 行为接近，**很可能**编译并运行正常（**但仍未实测**）：

- `bread-compact-wifi` ✅ **已实测**
- `bread-compact-ml307` ✅ 已编译验证
- `bread-compact-wifi-lcd`
- `bread-compact-nt26`
- `esp-box`、`esp-box-lite`（如果 Backlight 强转碰巧不崩）
- `esp32-cgc`
- `kevin-box-2`
- `mixgo-nova`
- `movecall-cuican-esp32s3`、`movecall-moji-esp32s3`
- `magiclick-c3`、`magiclick-c3-v2`、`magiclick-2p4`
- `xingzhi-cube-0.85tft-wifi/ml307`、`0.96oled-wifi/ml307`、`1.54tft-wifi/ml307`
- `zhengchen-1.54tft-ml307`
- `lceda-course-examples/eda-robot-pro`、`eda-super-bear`

---

## 二、其他工程问题

### 残留 C++ 文件污染 C 树

下面这些 `.h` 文件仍是 C++（用了 `class`、`std::`、`static_cast`），混在 C 板子目录里：

- `sensecap-watcher/sscma_camera.h`
- `sp-esp32-s3-1.28-box/power_manager.h`
- `sp-esp32-s3-1.54-muma/power_manager.h`
- `surfer-c3-1.14tft/power_manager.h`
- `xingzhi-abs-2.0/customlcddisplay.h`、`power_manager.h`
- `xingzhi-metal-1.54-wifi/cst816x.h`（被 .c 包含 → **编译失败**）
- `zhengchen-1.54tft-wifi/zhengchen_lcd_display.h`、`power_manager.h`
- `waveshare/esp32-s3-lcd-0.85/led_strip_control.h`（~~被 .c 包含 → **编译失败**~~ → 已改为纯 C 桩；实现见 `esp32-s3-lcd-0.85.c`）

### 内存/资源泄漏

多个板子 `destroy()` 函数只 `free(ctx)`，不释放它创建的 buttons、display、codec、PMIC、I2C bus：

- `atk-dnesp32s3-box3`、`box2-4g`、`box2-wifi`
- `df-k10`
- `kevin-sp-v3-dev`、`kevin-sp-v4-dev`
- `kevin-yuying-313lcd`

（实际上整个应用很少 destroy 板子，所以影响不大，但代码质量低。）

### 缺 ESP_ERROR_CHECK

多个板子的 LCD/触摸/相机初始化调用没用 `ESP_ERROR_CHECK`，硬件故障时静默失败：

`atk-dnesp32s3*`、`jiuchuan-s3`、`labplus-ledong-v2`、`labplus-mpython-v3`、`kevin-yuying-313lcd`、`esp-s3-lcd-ev-board`、`m5stack-tab5` 等

### 性能退化

- `no_audio_codec.c::no_audio_read/write` 每帧 `malloc(samples * 4)` 然后 `free`。原 C++ 实现是 `std::vector` 或栈分配。在音频热路径上每秒 ~100 次堆操作。

### 接口不完整

- `display_ops_t` 有 `clear_chat_messages` 字段，但 `display.h` 没提供 `display_clear_chat_messages()` 内联函数。
- `display_ops_t` 没有 `set_theme`，C 端无法切换主题。
- `display_cpp_bridge.cc::disp_lock/unlock` 都是空实现，**C 端调用时 LVGL 多线程访问会数据竞争**。

---

## 三、量化总结

| 类别 | 受影响板子数 |
|---|---|
| 总板子数 | 187（100 个目录） |
| 已实测可用 | **1** (`bread-compact-wifi`) |
| 已编译验证 | 2 (`bread-compact-wifi`, `bread-compact-ml307`) |
| 编译失败（已确认） | ≥ 5 |
| 编译失败（推断） | 约 30~50 |
| 有 Backlight ABI bug | ~~89~~ **已修**（适配器 + `destroy` 释放） |
| 有 Camera ABI bug | 2（其他 17 个虽不暴露但功能缺失） |
| BOARD_KIND_DUAL 引脚/默认网未配 | ~~5~~ **已修**（各板 `modem_*` + `default_net_type`） |
| PowerSaveTimer 丢失 | 40+ |
| 触摸屏丢失 | 30+ |
| 摄像头丢失 | 19 |
| 自定义显示丢失 | 6 |
| MCP 工具丢失 | 11+ |
| WiFi 配网入口失效 | ~60 |

---

## 四、修复优先级建议

按影响范围排序：

### P0 - 阻塞所有使用 LCD 的板子（~89 个）
1. ~~修复 `c_board_wrapper.cc` 的 `GetBacklight()` 生命周期与 ABI~~（**已修**：`CBacklightAdapter` + 各板 `destroy()` 中 `backlight_destroy`）

### P1 - 阻塞双网络板子（~5 个）
2. ~~`BOARD_KIND_DUAL`：`create_board()` 分支 + 各双网板补全 `modem_*` / `default_net_type`~~（**已修**）
3. ~~`audio_service_init` 不安全 cast~~（**已修**：`codec_unwrap_cpp`）

### P2 - 音频功能不正确
4. ~~`CAudioCodecAdapter` 转发 `SetOutputVolume` / `SetInputGain` / `Start`~~（**已修**，见 Bug 3）
5. ~~`audio_codec_destroy()` 潜在无限递归~~（**已修**，见 Bug 4）
6. ~~每帧堆分配~~（**已缓解**）：`no_audio_codec.c` 使用 `NO_AUDIO_STATIC_SAMPLES` 固定缓冲，超大帧才 `malloc`；`codec_cpp_bridge.cc` 在 `codec_wrapper` 内复用 `std::vector` PCM 缓冲，避免每次 `read`/`write` 新建 vector

**纯 C 化后续（未在本轮实现）**：`AudioService`、`CAudioCodecAdapter`、`codec_cpp_bridge` 仍为 C++；完全纯 C 需要把采集/编码/解码管线与板级 codec 接口迁到 C 层，并替换当前 `std::vector` 缓冲为显式 `audio_codec_t` 侧环形缓冲或静态上限。若你希望优先拆哪一层（例如先 C 化 `audio_service` 回调边界），可单独开任务说明。

### P3 - 编译失败的板子需逐一修
7. ~~`lichuang-dev`、`esp-box-3`、`kevin-sp-v3-dev`、`xingzhi-metal-1.54-wifi`、`waveshare/esp32-s3-lcd-0.85`~~（均已修）；另见 `df-k10`/`esp-hi` MCP 回调修复

### P4 - C API 缺口
8. 暴露 `app_enter_wifi_config_mode()` 给 C
9. `display_ops_t` 加 `lock`/`unlock` 真实实现（连接 LVGL）
10. `led_cpp_bridge` 传递 state/voice 参数

### P5 - 功能补全（按硬件 SKU 优先级排）
11-N. 补回各板子缺失的 PowerSaveTimer、Camera、Touch、PMIC、MCP 工具等

### P6 - 代码卫生
- 清理残留 C++ 头文件
- 补 `ESP_ERROR_CHECK`
- 修内存泄漏

---

## 五、原始数据

完整的子代理逐板报告保存在 `/tmp/audit_reports.md`（1043 行）。
