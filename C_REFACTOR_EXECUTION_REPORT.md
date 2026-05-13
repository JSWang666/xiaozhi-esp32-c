# C Refactor Execution Report

## 本次已完成内容

### 1) 基线与清单
- 新增 `scripts/c_refactor_inventory.py`，自动统计 C/C++ 文件与板级目录。
- 生成 `c_refactor_inventory.json` 基线数据。
- 新增 `scripts/c_refactor_generate_checklist.py`，自动生成全量清单。
- 生成根目录 `C_REFACTOR_CHECKLIST.md`（含 99 个板级目录追踪表）。

### 2) C API 规范
- 新增 `C_REFACTOR_API_SPEC.md`，定义：
  - C 接口统一约束
  - 内存所有权与资源释放规则
  - app/protocol/audio/board 分层 API 设计
  - C++ 边界层策略与验收门槛

### 3) 核心迁移（兼容桥）
- 新增 C API 头文件：
  - `main/c_api/app_c_api.h`
  - `main/c_api/protocol_c_api.h`
  - `main/c_api/audio_c_api.h`
  - `main/c_api/board_c_api.h`
- 新增 C++ 兼容桥实现：
  - `main/c_api/app_cpp_bridge.cc`
  - `main/c_api/protocol_cpp_bridge.cc`
  - `main/c_api/audio_cpp_bridge.cc`
  - `main/c_api/board_cpp_bridge.cc`
- 入口切换：
  - `main/main.cc` 已改为通过 `app_c_api.h` 调用 `app_create/app_init/app_run`。
- 构建接入：
  - `main/CMakeLists.txt` 已加入上述 bridge 源文件。

## 板级迁移说明（全量范围）

- 本次未对 99 个板目录逐个手工改写为 `.c`，而是先完成“统一 C 接口 + board 兼容桥”。
- 通过 `board_cpp_bridge.cc`，当前所有板级实现仍可复用既有 C++ 逻辑，但上层已经可按 C API 编程。
- 这是全量板级迁移的必要前置，可在不破坏现有功能前提下推进分批 C 化。

## 稳定性与发布前检查建议

- 编译检查：
  - 执行 `idf.py set-target <chip> && idf.py build` 验证桥接代码可编译。
- 功能冒烟（建议最少）：
  - 1 个 WiFi 板 + 1 个蜂窝板。
  - 验证开机、联网、唤醒、会话、音频播放、MCP、OTA 入口。
- 性能回归：
  - 对比内存峰值、CPU 占用、启动时间、语音首包时延。

## 后续剩余工作（下一轮）

- 将 `application.cc` 的事件循环和调度队列从 C++ 容器改为 C 数据结构。
- 将 `protocol/audio` 的桥接逐步替换为原生 C 实现（保留桥接仅用于回滚）。
- 按 `C_REFACTOR_CHECKLIST.md` 板级追踪表逐个板型收敛到 `ported/validated`。

