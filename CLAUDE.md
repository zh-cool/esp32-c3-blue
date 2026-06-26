# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

ESP-IDF 项目，目标芯片 ESP32-C3（RISC-V），项目名 "blue"。当前为空白框架，仅包含 `app_main(void)` 入口。

## 常用命令

```bash
# 配置项目（菜单式配置）
idf.py menuconfig

# 构建
idf.py build

# 烧录
idf.py -p /dev/ttyACM0 flash

# 烧录 + 串口监视
idf.py -p /dev/ttyACM0 flash monitor

# 仅串口监视
idf.py -p /dev/ttyACM0 monitor

# 清除构建产物
idf.py fullclean

# 添加组件依赖（从 ESP Component Registry）
idf.py add-dependency "namespace/component"
```

> 串口设备路径 `/dev/ttyACM0` 根据实际环境调整。

## 项目结构

```
blue/
├── CMakeLists.txt          # 顶层 CMake，调用 ESP-IDF 构建系统
├── main/
│   ├── CMakeLists.txt      # 组件注册，声明源码和头文件路径
│   └── blue.c              # 主入口 app_main()
├── client/                 # Python BLE 测试客户端（独立项目，不参与固件构建）
├── .mcp.json               # MCP 服务器配置（RainMaker, 文档, 组件注册）
└── .claude/
    └── settings.local.json # Claude Code 本地设置（启用 MCP 服务器）
```

## 开发说明

### 构建系统
- 标准 ESP-IDF 构建流程：`cmake` → `idf.py` 封装
- 顶层的 `CMakeLists.txt` 引用 `$ENV{IDF_PATH}/tools/cmake/project.cmake`
- 所有源代码放在 `main/` 组件目录下，由 `main/CMakeLists.txt` 注册
- 尚未生成 `sdkconfig`，首次 `idf.py build` 时会自动生成默认配置

### MCP 服务器集成
项目已启用三个 Espressif MCP 服务器，可直接用于开发辅助：

| MCP 服务器 | 用途 |
|---|---|
| **espressif-documentation** | 语义搜索 ESP-IDF 文档（中英文），查 API、配置、示例 |
| **esp-component-registry** | 搜索和浏览 ESP-IDF 组件库，安装第三方依赖 |
| **rainmaker** | 管理 ESP RainMaker 设备（节点、分组、参数、定时） |

这些通过 `.mcp.json` 和 `settings.local.json` 配置，Claude Code 会话中自动生效。

## 开发说明

- 芯片：ESP32-C3（RISC-V 单核 160MHz）
- 协议栈可通过 `idf.py menuconfig` 配置（不预设 BLE 或 Wi-Fi，项目名 "blue" 暗示可能用于 BLE 项目）
- 添加新源文件需更新 `main/CMakeLists.txt` 中的 `SRCS` 列表
- 当前无 `sdkconfig` — 首次构建会自动生成；定制配置建议执行 `idf.py menuconfig`

### client/ 目录

`client/` 是 Python BLE 测试客户端，使用 `bleak` 库，项目结构独立（`pyproject.toml`、`uv.lock`）。不参与 ESP-IDF 构建。开发固件时无需关注其代码。
