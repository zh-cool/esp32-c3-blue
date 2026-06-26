# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

ESP32-C3 项目工作区中的 Python 工具项目。使用 Python 3.12 + uv 包管理。

本仓库是 [../BLE](../BLE)（ESP-IDF BLE Wi-Fi 配网 + OTA 客户端）的配套工具，提供 Python 层面的辅助功能。

## 相关项目

| 项目 | 路径 | 说明 |
|------|------|------|
| BLE | `../BLE` | ESP-IDF 固件 + Tkinter GUI 配网客户端 (`esp_prov_gui.py`)，BLE 配网 + OTA 升级 |
| wifi_prov | `../wifi_prov` | ESP-IDF Wi-Fi 配网固件（参考示例） |

## 环境配置

```bash
uv python pin 3.12        # Python 版本管理（已配置 .python-version）
uv sync                   # 安装依赖
uv add <package>          # 添加依赖
uv run python main.py     # 运行
```

项目使用 `uv`（基于 `pyproject.toml`），无需 `pip install -r requirements.txt`。

## 核心功能

**BLE 设备扫描器** — `main.py` 是基于 `bleak` + `tkinter` 的图形化 BLE 浏览器，可扫描周围蓝牙设备，连接后展示完整的 GATT 服务/特征值/描述符树。

### 用法

```bash
uv run python main.py
```

### 架构

遵循与 `../BLE/esp_prov_gui.py` 相同的异步模式：
- **UI 线程**（主线程）— `BLEScannerGUI`，Tkinter + ttk
- **后台 asyncio 事件循环** — 独立线程跑 `asyncio.new_event_loop().run_forever()`
- **通信桥接** — `queue.Queue` 将异步结果传递给 UI 线程；`root.after()` 轮询队列
- **BLE 引擎** — `BLEScannerEngine` 封装 `BleakScanner.discover()` / `BleakClient.connect()` / `get_services()`
