# C 语言重构遗留问题清单

**审查范围**：187 个新转换的 C 板子文件 + 核心 C/C++ 桥接代码  
**审查方式**：（1）抽样编译 5 个代表板子；（2）6 个子代理对全部板子做 .cc HEAD vs .c 现状的静态差异分析。  
**结论**：当前只有 **`bread-compact-wifi`** 经过编译+硬件验证。**至少 80% 的其他板子存在不同程度的问题**，从「编译失败」到「编译通过但功能缺失」到「编译通过但运行时崩溃」都有。

---

## 一、严重程度分类

### 🔴 编译失败（无法生成固件，已实测确认）

> **2026-05-13 更新**：下列 5 个板子中，**前 4 个**已在主线中修复并通过 `ninja -C build` 验证（`waveshare/esp32-s3-lcd-0.85` 的 C++ 头文件未被任何 `.c` 编译单元包含，CMake 只 glob `*.c`/`*.cc`，**待单独选板验证**）。

| 板子 | 错误类型 | 状态 |
|---|---|---|
| `lichuang-dev` | 缺 `#include "led/led.h"`（`led_t`），codec 宏需 `es8311_codec.h` | **已修** |
| `esp-box-3` | 缺 `esp_lcd_panel_io.h`、panel ops、ES8311/ES7210 头；`device_state.h` 已补 | **已修** |
| `kevin-sp-v3-dev` | 误用 `board_btn_handle_t`，应为 `board_btn_t*` + `board_btn_gpio_cfg_t` | **已修** |
| `xingzhi-metal-1.54-wifi` | `cst816x.h` 为 C++；主 `.c` 缺 SPI panel IO / ES8311 头 | **已修**（`cst816x.h` 改为纯 C 声明） |
| `waveshare/esp32-s3-lcd-0.85` | `led_strip_control.h` / `power_manager.h` 仍为 C++ | **未改**（当前未被 board `.c` 引用） |

**附加修复（同轮）**：`esp-sparkbot` 缺 LCD/codec 头；`df-k10` / `esp-hi` 的 MCP 工具回调误用不存在的 `mcp_property_*` API，已改为 `cJSON` + `mcp_tool_param_t`。

### 🟠 编译通过但有架构性 ABI Bug（运行时必崩）

#### Bug 1：`Backlight` / `Camera` 强制类型转换错误

`c_board_wrapper.cc` 把 C 的 `backlight_t*` / `camera_t*` 用 `static_cast` 当 C++ `Backlight*` / `Camera*` 用。这就是之前 Display/Led/Codec 的同款 bug。

```92:104:main/boards/common/c_board_wrapper.cc
Backlight *GetBacklight() override {
    if (desc_->get_backlight) {
        return static_cast<Backlight *>(desc_->get_backlight(desc_));
    }
    return WifiBoard::GetBacklight();
}

Camera *GetCamera() override {
    if (desc_->get_camera) {
        return static_cast<Camera *>(desc_->get_camera(desc_));
    }
    return WifiBoard::GetCamera();
}
```

**受影响板子**：89 个板子设置了 `.get_backlight`（基本所有带 LCD 屏的板子），2 个设置了 `.get_camera`。

#### Bug 2：`BOARD_KIND_DUAL` 被忽略

`create_board()` 只处理 `BOARD_KIND_WIFI`，所有声明 `BOARD_KIND_DUAL` 的板子（Wi-Fi + ML307 双网络）会被静默降级为纯 Wi-Fi：

- `magiclick-2p5`
- `minsi-k08-dual`
- `nulllab-ai-vox-v3`
- `atk-dnesp32s3-box2-4g`、`atk-dnesp32s3-box2-wifi`（也用 DUAL）

ML307 蜂窝模块在这些板子上完全无法启动。

#### Bug 3：`CAudioCodecAdapter` 没转发音量/增益/Start

```c
class CAudioCodecAdapter : public NoAudioCodec {
    explicit CAudioCodecAdapter(audio_codec_t *c) : NoAudioCodec(c) {}
};
```

`NoAudioCodec` 没重写 `SetOutputVolume()` / `SetInputGain()` / `Start()`，基类只更新 C++ 成员变量，不会调 `c_codec_->ops->set_output_volume()`。**所有带 C 自定义 codec 的板子（sensecap、m5stack 系列、df-k10、lilygo 系列等）的音量控制不会真正生效**。

#### Bug 4：`audio_codec_destroy()` 无限递归

```c
void audio_codec_destroy(audio_codec_t *codec) {
    if (codec && codec->ops && codec->ops->destroy)
        codec->ops->destroy(codec);  // 调用 codec 自己的 destroy
}
```

多个板子 codec 的 destroy ops 里又调 `audio_codec_destroy(codec)` → 无限递归 → 栈溢出。**只在 codec 析构时触发**（实际运行很少触发）：

- `sensecap-watcher/sensecap_audio_codec.c`
- `df-k10/k10_audio_codec.c`
- `m5stack-tab5/tab5_audio_codec.c`
- `m5stack-core-s3/cores3_audio_codec.c`
- `lilygo-*/adc_pdm_audio_codec.c`
- 还有几个 PDM 变体

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

```cpp
audio_service.Initialize(static_cast<AudioCodec *>(codec));  // void* 强转 C++
```

若上层传入的是 `audio_codec_t*`（C），又是同样的 ABI 错误。

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
- `waveshare/esp32-s3-lcd-0.85/led_strip_control.h`（被 .c 包含 → **编译失败**）

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
| 有 Backlight ABI bug | 89 |
| 有 Camera ABI bug | 2（其他 17 个虽不暴露但功能缺失） |
| BOARD_KIND_DUAL 失效 | 5 |
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
1. 修复 `c_board_wrapper.cc` 的 `GetBacklight()` ABI bug（用类似 Display 的 unwrap 或 adapter 方案）

### P1 - 阻塞双网络板子（~5 个）
2. 在 `c_board_wrapper.cc::create_board()` 实现 `BOARD_KIND_DUAL` 分支
3. 修复 `audio_service_init` 的不安全 cast

### P2 - 音频功能不正确
4. `CAudioCodecAdapter` 添加 `SetOutputVolume`/`SetInputGain`/`Start` override 转发到 ops
5. 修复 `audio_codec_destroy()` 的潜在无限递归
6. 修复 `no_audio_codec.c` 每帧 malloc

### P3 - 编译失败的板子需逐一修
7. ~~`lichuang-dev`、`esp-box-3`、`kevin-sp-v3-dev`、`xingzhi-metal-1.54-wifi`~~（已修）；`waveshare/esp32-s3-lcd-0.85`（遗留 C++ 头文件，待选板验证）；另见 `df-k10`/`esp-hi` MCP 回调修复

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
