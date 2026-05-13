# C Refactor API Spec

本文档定义 C 语言重构阶段的统一接口与内存规则，作为后续核心模块和板级迁移的准入标准。

## 1. 通用约束

- 所有 C 接口使用 `esp_err_t` 返回错误码，成功为 `ESP_OK`。
- 句柄类型统一为不透明指针：`typedef struct xxx xxx_t;`
- 创建/销毁命名统一：`xxx_create` / `xxx_destroy`。
- 涉及跨任务调用的回调统一形态：`void (*cb)(void *user_ctx, ...)`。
- 字符串参数规则：
  - 入参字符串默认借用（caller 保证生命周期覆盖调用）。
  - 出参字符串默认由 callee 分配，caller 调用 `xxx_free_string` 释放。

## 2. 内存所有权规则

- Packet/Buffer 类型禁止隐式共享，采用显式转移：
  - 生产方调用 `packet_alloc` 创建。
  - 投递到队列成功后，所有权转移给消费方。
  - 队列投递失败则生产方负责释放。
- 回调中收到的对象默认只读借用；若需异步使用必须拷贝。
- cleanup 约束：
  - 函数内资源释放使用统一 `goto cleanup` 路径。
  - 每个模块提供独立 `reset` 接口用于异常恢复。

## 3. 应用层 C API（app）

- 生命周期：
  - `app_context_t *app_create(const app_config_t *cfg);`
  - `esp_err_t app_init(app_context_t *ctx);`
  - `esp_err_t app_run(app_context_t *ctx);`
  - `void app_destroy(app_context_t *ctx);`
- 事件接口：
  - `esp_err_t app_post_event(app_context_t *ctx, app_event_t event, const void *data);`
  - `esp_err_t app_schedule(app_context_t *ctx, app_task_fn fn, void *arg);`
- 控制接口：
  - `esp_err_t app_start_listening(app_context_t *ctx);`
  - `esp_err_t app_stop_listening(app_context_t *ctx);`
  - `esp_err_t app_toggle_chat(app_context_t *ctx);`

## 4. 协议层 C API（protocol）

- 抽象接口改为函数指针表 `protocol_iface_t`：
  - `start/open_audio/close_audio/send_audio/send_json/send_mcp`。
- 统一回调注册：
  - `on_connected/on_disconnected/on_network_error/on_audio/on_json`。
- 实现分离：
  - `protocol_ws_*`：WebSocket 实现。
  - `protocol_mqtt_*`：MQTT+UDP 实现。
- 二进制音频包结构统一由 `protocol_packet_t` 表达。

## 5. 音频层 C API（audio）

- 句柄：
  - `audio_service_t *audio_service_create(const audio_service_cfg_t *cfg);`
  - `void audio_service_destroy(audio_service_t *svc);`
- 运行控制：
  - `audio_service_start/stop/reset_decoder/enable_wake_word/enable_vad`。
- 队列接口：
  - `audio_service_push_decode_packet`
  - `audio_service_pop_send_packet`
  - `audio_service_pop_wake_word_packet`
- 回调：
  - 发送队列可用、唤醒词命中、VAD 变化。

## 6. 板级层 C API（board）

- 抽象结构：
  - `board_ops_t`：board 能力函数表。
  - `board_handle_t`：板实例句柄。
- 工厂：
  - `board_handle_t *board_create(const board_init_cfg_t *cfg);`
  - `void board_destroy(board_handle_t *b);`
- 能力：
  - `get_audio_codec/get_display/get_led/get_network/start_network/set_power_level`。
- 网络事件：
  - `board_set_network_callback(board_handle_t *b, board_net_cb cb, void *user_ctx)`。

## 7. 兼容层策略（C++ 边界）

- 在过渡阶段允许少量 C++ 包装层：
  - `*_cpp_bridge.cc` 内部调用现有 C++ 类。
  - 对外只暴露 C 头文件。
- 迁移完成条件：
  - 业务主路径（app/protocol/audio/board common）不再直接 include C++ STL 头。

## 8. 验收门槛

- 新增 C 接口必须提供：
  - 头文件注释（线程安全、所有权、错误码）。
  - 最小调用样例（可放在注释或测试桩）。
  - 与旧接口功能对照表。

