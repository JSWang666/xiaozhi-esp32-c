#!/usr/bin/env python3
"""
Generate a detailed C refactor checklist markdown.
"""

from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INV_FILE = ROOT / "c_refactor_inventory.json"
OUT_FILE = ROOT / "C_REFACTOR_CHECKLIST.md"


def load_inventory() -> dict:
    return json.loads(INV_FILE.read_text())


def write(out: list[str], line: str = "") -> None:
    out.append(line)


def generate() -> str:
    inv = load_inventory()
    summary = inv["main_summary"]
    boards = inv["boards"]["items"]
    board_count = inv["boards"]["count"]

    lines: list[str] = []
    write(lines, "# ESP-IDF 工程 C 语言重构清单（全量）")
    write(lines)
    write(lines, "## 0. 项目基线（自动盘点）")
    write(lines, f"- `main/` C++实现文件（`.cc/.cpp/.cxx`）：**{summary['cc_like_count']}**")
    write(lines, f"- `main/` C实现文件（`.c`）：**{summary['c_count']}**")
    write(lines, f"- `main/` 头文件（`.h/.hpp`）：**{summary['header_count']}**")
    write(lines, f"- 板级目录数量（不含`boards/common`）：**{board_count}**")
    write(lines, "- 盘点文件：`c_refactor_inventory.json`（由 `scripts/c_refactor_inventory.py` 生成）")
    write(lines)
    write(lines, "## 1. 总体重构策略")
    write(lines, "- [ ] 保持业务行为一致（网络、音频、UI、MCP、OTA、电源管理）")
    write(lines, "- [ ] C 优先：核心逻辑迁移为 C，允许少量 C++ 边界适配层")
    write(lines, "- [ ] 分阶段可回滚：每阶段都可以独立编译与回归")
    write(lines, "- [ ] 引入统一迁移状态标记（每个模块/板型都有 owner、状态、验收记录）")
    write(lines)
    write(lines, "## 2. 构建系统改造（CMake / ESP-IDF）")
    build_items = [
        "为 C 与 C++ 混编定义统一规则（文件后缀、include 边界、extern C 约定）",
        "将核心模块源文件注册拆分为分层列表（core_c_sources / legacy_cpp_sources / board_sources）",
        "增加重构开关（例如 `CONFIG_C_REFACTOR_STAGE`）用于阶段切换",
        "确保链接顺序不改变现有行为（尤其协议、音频与板级创建函数）",
        "为脚本产物（语言配置、assets）保持现有生成行为不变",
        "在 CI 增加 C 重构分支构建任务（至少覆盖 1 个 WiFi 板和 1 个 4G 板）",
    ]
    for item in build_items:
        write(lines, f"- [ ] {item}")
    write(lines)
    write(lines, "## 3. 核心 API C 化清单")
    write(lines, "### 3.1 Application 层")
    for item in [
        "定义 `app_context_t` 替代 C++ 单例 `Application`",
        "定义生命周期接口：`app_create/app_init/app_run/app_destroy`",
        "将事件循环位图与处理函数迁移为 C 函数表",
        "将调度队列 `std::deque<std::function<...>>` 替换为 C 任务队列",
        "为告警、状态切换、唤醒词触发提供 C API",
    ]:
        write(lines, f"- [ ] {item}")
    write(lines, "### 3.2 Protocol 层")
    for item in [
        "定义 `protocol_iface_t`（函数指针表）替代虚函数接口",
        "拆分 MQTT/WebSocket 为 C 实现模块与可选 C++ 适配层",
        "统一入站 JSON 与音频回调签名（C 风格上下文指针）",
        "替换 `std::unique_ptr<AudioStreamPacket>` 为 C 包结构与释放函数",
    ]:
        write(lines, f"- [ ] {item}")
    write(lines, "### 3.3 Audio 层")
    for item in [
        "定义 `audio_service_t`，显式管理 init/start/stop",
        "替换 STL 队列与互斥为 FreeRTOS 队列 + 明确锁策略",
        "编码/解码任务上下文改为 C 结构，移除 lambda 捕获",
        "唤醒词与 VAD 回调改为 C 回调（函数指针 + 用户参数）",
    ]:
        write(lines, f"- [ ] {item}")
    write(lines, "### 3.4 Board 抽象层")
    for item in [
        "定义 `board_ops_t` 替代 C++ `virtual` 接口",
        "把 `create_board()` 约束为 C 工厂函数（返回 `board_handle_t`）",
        "网络事件回调改为 C 函数签名",
        "将各子能力（led/display/audio/network）封装为 C 句柄",
    ]:
        write(lines, f"- [ ] {item}")
    write(lines)
    write(lines, "## 4. 数据结构与内存模型清单")
    for item in [
        "建立统一内存所有权规则（创建者释放、跨线程移交、回调只借用）",
        "为每种动态对象定义 `*_create/*_destroy` 与 `*_reset`",
        "替换 `std::string`：确定 `char*` + 长度、固定缓冲区或 arena 策略",
        "替换 `std::vector`/`std::deque`：优先 FreeRTOS queue/ringbuffer",
        "替换 RAII：通过显式 cleanup 标签（goto）保证异常路径释放",
        "定义错误码体系（统一 `esp_err_t` 映射）",
    ]:
        write(lines, f"- [ ] {item}")
    write(lines)
    write(lines, "## 5. 核心模块迁移顺序（必须按序）")
    phases = [
        ("P1", "协议基座：`protocol.*` + 二进制包结构 + 回调桥"),
        ("P2", "状态机：`device_state_machine.*` 与应用状态切换"),
        ("P3", "应用主循环：`application.*` + `main.cc` 到 `main.c`"),
        ("P4", "音频主干：`audio_service.*` + processor/wake_word/codec 桥接"),
        ("P5", "系统能力：`settings`、`ota`、`assets`、`mcp_server`"),
        ("P6", "显示与灯效：`display/*`、`led/*`"),
    ]
    for phase, text in phases:
        write(lines, f"- [ ] {phase}: {text}")
    write(lines)
    write(lines, "## 6. 板级公共层迁移（`boards/common`）")
    common_targets = [
        "board / wifi_board / ml307_board / nt26_board / dual_network_board",
        "esp_video / esp32_camera / blufi 以及网络接入相关模块",
        "power_save_timer / sleep_timer / button / knob / backlight / i2c_device",
        "adc_battery_monitor / sy6970 / axp2101 / system_reset",
        "press_to_talk_mcp_tool / afsk_demod",
    ]
    for item in common_targets:
        write(lines, f"- [ ] {item}")
    write(lines)
    write(lines, "## 7. 板级迁移追踪（全部 99 个目录）")
    write(lines, "- 状态定义：`not_started` / `in_progress` / `ported` / `validated`")
    write(lines)
    write(lines, "| 板型目录 | C++文件数 | C文件数 | 状态 | 备注 |")
    write(lines, "|---|---:|---:|---|---|")
    for item in boards:
        write(
            lines,
            f"| `{item['board']}` | {item['cc_count']} | {item['c_count']} | not_started | |",
        )
    write(lines)
    write(lines, "## 8. 回归测试与验收")
    for item in [
        "编译回归：每次迁移至少验证一个 WiFi 板 + 一个蜂窝板",
        "核心功能回归：开机、联网、激活、语音对话、中断恢复",
        "音频回归：唤醒、VAD、编码发送、下行解码播放、AEC 模式切换",
        "协议回归：WebSocket 与 MQTT+UDP 路径均可用",
        "MCP 回归：工具注册、调用、异常路径",
        "OTA 回归：版本检测、升级失败恢复、升级成功重启",
        "UI 回归：状态文本、情绪动画、通知/告警",
        "功耗回归：空闲态/会话态电流与温度",
        "稳定性回归：长时运行、断网重连、异常注入",
    ]:
        write(lines, f"- [ ] {item}")
    write(lines)
    write(lines, "## 9. 发布前清理")
    for item in [
        "删除已完成迁移路径中的 C++ 业务实现（保留必要边界层）",
        "更新开发文档（模块接口、迁移规范、板级接入模板）",
        "更新 CI，确保默认流水线覆盖 C 迁移主分支",
        "输出最终迁移报告（性能、内存、风险遗留）",
    ]:
        write(lines, f"- [ ] {item}")
    write(lines)
    write(lines, "## 10. 每周执行节奏建议")
    write(lines, "- [ ] 周一：更新基线与问题清单")
    write(lines, "- [ ] 周二到周四：核心/板级迁移 + 日回归")
    write(lines, "- [ ] 周五：全量回归、性能对比、风险审计")
    write(lines)
    write(lines, "---")
    write(lines, "该清单由脚本自动生成，命令：")
    write(lines, "`python3 scripts/c_refactor_inventory.py && python3 scripts/c_refactor_generate_checklist.py`")
    write(lines)
    return "\n".join(lines)


def main() -> None:
    OUT_FILE.write_text(generate() + "\n")
    print(f"Wrote {OUT_FILE}")


if __name__ == "__main__":
    main()
