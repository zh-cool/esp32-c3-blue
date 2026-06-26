#!/usr/bin/env python3
"""
BLE 设备扫描器 — 扫描周围蓝牙设备，显示所有服务与特征值

基于 bleak + Tkinter 的图形化 BLE 浏览器。
"""

import asyncio
import os
import queue
import sys
import threading
import time
from datetime import datetime
from typing import Optional
from tkinter import StringVar, ttk, scrolledtext, messagebox, Tk, Toplevel
from tkinter.constants import *

import bleak
from bleak import BleakScanner, BleakClient
from bleak.backends.device import BLEDevice
from bleak.backends.service import BleakGATTServiceCollection


# ── 常量 ──────────────────────────────────────────────────────────
APP_TITLE = "BLE 设备扫描器"
WIN_W, WIN_H = 900, 720
LOG_POLL_MS = 100


# ══════════════════════════════════════════════════════════════════
#  帮助函数
# ══════════════════════════════════════════════════════════════════
def uuid_short(uuid: str) -> str:
    """将标准 128-bit UUID 显示为短格式（如果可能）"""
    if uuid.startswith("0000") and uuid.endswith("-0000-1000-8000-00805f9b34fb"):
        return f"0x{uuid[4:8]}"
    return uuid


def gatt_props_display(props) -> str:
    """格式化 GATT 特征值属性"""
    flags = []
    if "read" in props:
        flags.append("R")
    if "write" in props:
        flags.append("W")
    if "write-without-response" in props:
        flags.append("W/oResp")
    if "notify" in props:
        flags.append("N")
    if "indicate" in props:
        flags.append("I")
    return "[" + ", ".join(flags) + "]" if flags else "[]"


DESCRIPTOR_NAMES = {
    "00002900-0000-1000-8000-00805f9b34fb": "Extended Properties",
    "00002901-0000-1000-8000-00805f9b34fb": "User Description",
    "00002902-0000-1000-8000-00805f9b34fb": "Client Characteristic Configuration",
    "00002903-0000-1000-8000-00805f9b34fb": "Server Characteristic Configuration",
    "00002904-0000-1000-8000-00805f9b34fb": "Presentation Format",
    "00002905-0000-1000-8000-00805f9b34fb": "Aggregate Format",
    "00002906-0000-1000-8000-00805f9b34fb": "Valid Range",
    "00002907-0000-1000-8000-00805f9b34fb": "External Report Reference",
    "00002908-0000-1000-8000-00805f9b34fb": "Report Reference",
}


def descriptor_name(desc_uuid: str) -> str:
    """返回描述符的常用名称"""
    return DESCRIPTOR_NAMES.get(desc_uuid, "")


# ══════════════════════════════════════════════════════════════════
#  BLE 扫描器引擎（asyncio）
# ══════════════════════════════════════════════════════════════════
class BLEScannerEngine:
    """封装 bleak 异步操作，结果通过 queue 回传"""

    def __init__(self, log_queue: queue.Queue, notify_queue: queue.Queue):
        self._log_q = log_queue
        self._notify_q = notify_queue
        self._client: Optional[BleakClient] = None
        self._notify_handles: list[tuple[str, int]] = []  # (uuid, handle)

    def log(self, msg: str):
        self._log_q.put_nowait(("log", msg))

    async def scan(self, timeout: float = 10.0) -> list[BLEDevice]:
        self.log(f"扫描 BLE 设备（{timeout:.0f} 秒）...")
        devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
        # 只保留有名称的设备
        results = []
        for addr, (device, adv_data) in devices.items():
            if device.name:
                results.append(device)
        self.log(f"发现 {len(results)} 个设备")
        return results

    async def connect_and_discover(
        self, device: BLEDevice, adapter: str = "hci0",
    ) -> Optional[list[dict]]:
        """连接设备并发现所有服务，返回结构化信息"""
        self.log(f"连接 {device.name} [{device.address}] ...")
        try:
            self._client = BleakClient(device, adapter=adapter)
            await self._client.connect(timeout=15.0)

            services = self._client.services
            svc_list = list(services)

            # 构建 handle → BleakGATTCharacteristic 映射（供描述符查找用）
            char_by_handle: dict[int, BleakGATTCharacteristic] = {}
            for svc in svc_list:
                for char in svc.characteristics:
                    char_by_handle[char.handle] = char

            total_desc = len(services.descriptors)
            self.log(f"共发现 {total_desc} 个描述符")

            result = []
            for svc in svc_list:
                svc_info = {
                    "type": "service",
                    "uuid": svc.uuid,
                    "description": svc.description or "",
                    "handle": svc.handle,
                    "children": [],
                }
                for char in svc.characteristics:
                    char_info = {
                        "type": "characteristic",
                        "uuid": char.uuid,
                        "description": char.description or "",
                        "handle": char.handle,
                        "properties": list(char.properties),
                        "children": [],
                    }
                    # 从全局描述符列表中找出属于该特征值的描述符
                    for desc_h, desc in services.descriptors.items():
                        if desc.characteristic_handle == char.handle:
                            desc_value = None
                            # 自动读取 User Description（0x2901）的值
                            if desc.uuid == "00002901-0000-1000-8000-00805f9b34fb":
                                val = await self.read_descriptor(desc.handle)
                                if val:
                                    try:
                                        desc_value = val.decode("utf-8", errors="replace")
                                    except Exception:
                                        desc_value = val.hex()
                            desc_info = {
                                "type": "descriptor",
                                "uuid": desc.uuid,
                                "description": desc.description or "",
                                "handle": desc.handle,
                                "value": desc_value,
                            }
                            char_info["children"].append(desc_info)
                    svc_info["children"].append(char_info)
                result.append(svc_info)

            # 自动订阅有 notify 属性的特征值
            await self._subscribe_notify(svc_list)

            return result

        except Exception as e:
            self.log(f"错误：{e}")
            return None

    async def _subscribe_notify(self, svc_list):
        """订阅所有带 notify 属性的特征值"""
        self._notify_handles.clear()
        for svc in svc_list:
            for char in svc.characteristics:
                if "notify" in char.properties:
                    try:
                        uuid_shortened = uuid_short(char.uuid)
                        await self._client.start_notify(
                            char,
                            self._make_notify_handler(char.uuid, char.handle),
                        )
                        self._notify_handles.append((char.uuid, char.handle))
                        self.log(f"  订阅通知: {uuid_shortened} (handle={char.handle})")
                    except Exception as e:
                        self.log(f"  订阅失败 {uuid_short(char.uuid)}: {e}")

    def _make_notify_handler(self, char_uuid: str, handle: int):
        """创建通知回调，将数据发往通知队列"""
        def _handler(_sender, data: bytearray):
            uuid_s = uuid_short(char_uuid)
            hex_str = data.hex()
            try:
                text = data.decode("utf-8", errors="replace")
                if text.isprintable():
                    self._notify_q.put_nowait(("notify", uuid_s, text))
                else:
                    self._notify_q.put_nowait(("notify", uuid_s, hex_str))
            except Exception:
                self._notify_q.put_nowait(("notify", uuid_s, hex_str))
        return _handler

    async def disconnect(self):
        if self._client and self._client.is_connected:
            # 停止所有通知
            for char_uuid, _ in self._notify_handles:
                try:
                    await self._client.stop_notify(char_uuid)
                except Exception:
                    pass
            self._notify_handles.clear()
            await self._client.disconnect()
        self._client = None

    async def reconnect_and_discover(
        self, device: BLEDevice, adapter: str = "hci0",
    ) -> Optional[list[dict]]:
        """先断开当前连接，再连接新设备并发现服务"""
        await self.disconnect()
        return await self.connect_and_discover(device, adapter=adapter)

    async def read_char(self, char_uuid: str) -> Optional[bytearray]:
        """读取特征值"""
        if not self._client or not self._client.is_connected:
            return None
        try:
            return await self._client.read_gatt_char(char_uuid)
        except Exception as e:
            self.log(f"读取失败 {uuid_short(char_uuid)}: {e}")
            return None

    async def write_char(self, char_uuid: str, data: bytes, with_response: bool = True) -> bool:
        """写入特征值"""
        if not self._client or not self._client.is_connected:
            return False
        try:
            await self._client.write_gatt_char(char_uuid, data, response=with_response)
            return True
        except Exception as e:
            self.log(f"写入失败 {uuid_short(char_uuid)}: {e}")
            return False

    async def read_descriptor(self, desc_handle: int) -> Optional[bytearray]:
        """读取描述符值（如 0x2901 User Description）"""
        if not self._client or not self._client.is_connected:
            return None
        try:
            return await self._client.read_gatt_descriptor(desc_handle)
        except Exception as e:
            return None


# ══════════════════════════════════════════════════════════════════
#  GUI 主程序
# ══════════════════════════════════════════════════════════════════
class BLEScannerGUI:
    def __init__(self):
        self.root = Tk()
        self.root.title(APP_TITLE)
        self.root.geometry(f"{WIN_W}x{WIN_H}")
        self.root.minsize(700, 500)

        # ── 状态变量 ────────────────────────────────────────────
        self.engine: Optional[BLEScannerEngine] = None
        self._devices: list[BLEDevice] = []
        self._selected_device: Optional[BLEDevice] = None
        self._connected = False
        self._service_data: Optional[list[dict]] = None
        self._async_busy = False
        self.log_queue = queue.Queue()
        self.notify_queue = queue.Queue()

        # ── 启动持久化 asyncio 事件循环 ─────────────────────────
        self._loop = None
        self._loop_thread = threading.Thread(target=self._start_loop, daemon=True)
        self._loop_thread.start()
        while self._loop is None:
            time.sleep(0.01)

        self._build_ui()
        self._poll_log()
        self._poll_notify()

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ────────────────────────────────────────────────────────────────
    #  事件循环管理
    # ────────────────────────────────────────────────────────────────
    def _start_loop(self):
        """后台线程：运行 asyncio 事件循环"""
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()

    def _run_async(self, coro, callback=None):
        """将协程调度到后台事件循环"""
        if self._async_busy:
            messagebox.showinfo("提示", "请等待当前操作完成")
            return
        self._async_busy = True
        self._update_buttons()

        async def _wrapper():
            try:
                result = await coro
                self.root.after(0, lambda: self._on_async_done(result, callback))
            except Exception as e:
                self.log_queue.put_nowait(("log", f"异步错误: {e}"))
                self.root.after(0, lambda: self._on_async_done(None, callback))

        asyncio.run_coroutine_threadsafe(_wrapper(), self._loop)

    def _on_async_done(self, result, callback):
        self._async_busy = False
        self._update_buttons()
        if callback:
            callback(result)

    def _update_buttons(self):
        state = DISABLED if self._async_busy else NORMAL
        self.btn_scan.config(state=state)
        self.btn_disconnect.config(state=NORMAL if self._connected else DISABLED)

    # ────────────────────────────────────────────────────────────────
    #  构建界面
    # ────────────────────────────────────────────────────────────────
    def _build_ui(self):
        style = ttk.Style()
        style.configure("Title.TLabel", font=("", 11, "bold"))
        style.configure("Status.TLabel", font=("", 9))

        main = ttk.Frame(self.root, padding=8)
        main.pack(fill=BOTH, expand=True)

        ttk.Label(main, text=APP_TITLE, style="Title.TLabel").pack(anchor=W, pady=(0, 6))

        # ── 上半部分：设备列表 ──────────────────────────────────
        top = ttk.LabelFrame(main, text="BLE 设备", padding=4)
        top.pack(fill=BOTH, expand=True, pady=(0, 6))

        # 工具栏
        toolbar = ttk.Frame(top)
        toolbar.pack(fill=X, pady=(0, 4))

        ttk.Label(toolbar, text="BLE 适配器：").pack(side=LEFT, padx=(0, 4))
        self.adapter_var = StringVar(value="hci0")
        ttk.Entry(toolbar, textvariable=self.adapter_var, width=10).pack(side=LEFT, padx=(0, 8))

        self.btn_scan = ttk.Button(toolbar, text="📡 扫描", command=self.on_scan)
        self.btn_scan.pack(side=LEFT, padx=(0, 4))

        self.btn_disconnect = ttk.Button(toolbar, text="断开", state=DISABLED,
                                         command=self.on_disconnect)
        self.btn_disconnect.pack(side=LEFT)

        # 设备列表（TreeView）
        cols = ("name", "addr", "rssi")
        self.device_tree = ttk.Treeview(top, columns=cols, show="headings", height=8)
        self.device_tree.heading("name", text="设备名称")
        self.device_tree.heading("addr", text="MAC 地址")
        self.device_tree.heading("rssi", text="RSSI")
        self.device_tree.column("name", width=280)
        self.device_tree.column("addr", width=180)
        self.device_tree.column("rssi", width=70, anchor=CENTER)
        vsb = ttk.Scrollbar(top, orient=VERTICAL, command=self.device_tree.yview)
        self.device_tree.configure(yscrollcommand=vsb.set)
        self.device_tree.pack(side=LEFT, fill=BOTH, expand=True)
        vsb.pack(side=RIGHT, fill=Y)
        self.device_tree.bind("<<TreeviewSelect>>", self._on_device_select)
        self.device_tree.bind("<Double-1>", lambda e: self.on_connect())

        self.scan_status_var = StringVar(value="⇨ 单击设备自动连接并显示服务")
        ttk.Label(top, textvariable=self.scan_status_var, font=("", 8),
                  foreground="gray").pack(side=BOTTOM, anchor=W, pady=(2, 0))

        # ── 下半部分：服务树 ────────────────────────────────────
        bottom = ttk.LabelFrame(main, text="GATT 服务 / 特征值 / 描述符", padding=4)
        bottom.pack(fill=BOTH, expand=True)     # 不额外占用垂直空间

        svc_cols = ("uuid", "info", "handle")
        self.svc_tree = ttk.Treeview(bottom, columns=svc_cols, show="tree headings", height=12)
        self.svc_tree.heading("#0", text="UUID 简称")
        self.svc_tree.heading("uuid", text="完整 UUID")
        self.svc_tree.heading("info", text="类型 / 信息")
        self.svc_tree.heading("handle", text="Handle")
        self.svc_tree.column("#0", width=100, minwidth=70)
        self.svc_tree.column("uuid", width=240, minwidth=160)
        self.svc_tree.column("info", width=260, minwidth=140)
        self.svc_tree.column("handle", width=70, anchor=CENTER)
        svc_vsb = ttk.Scrollbar(bottom, orient=VERTICAL, command=self.svc_tree.yview)
        self.svc_tree.configure(yscrollcommand=svc_vsb.set)
        self.svc_tree.pack(side=LEFT, fill=BOTH, expand=True)
        svc_vsb.pack(side=RIGHT, fill=Y)
        self.svc_tree.bind("<Double-1>", self._on_svc_tree_double_click)

        # ── 底部：日志 + 通知（Notebook 标签页） ────────────────
        bottom_notebook = ttk.Notebook(main)
        bottom_notebook.pack(fill=BOTH, side=BOTTOM, pady=(4, 0))

        # 日志标签页
        log_frame = ttk.Frame(bottom_notebook)
        bottom_notebook.add(log_frame, text="  日志  ")
        self.log_text = scrolledtext.ScrolledText(log_frame, height=8, font=("Consolas", 9),
                                                   wrap=WORD, state=DISABLED)
        self.log_text.pack(fill=BOTH, expand=True)

        # 通知标签页
        notify_frame = ttk.Frame(bottom_notebook)
        bottom_notebook.add(notify_frame, text="  通知  ")
        self.notify_text = scrolledtext.ScrolledText(notify_frame, height=8, font=("Consolas", 9),
                                                      wrap=WORD, state=DISABLED, foreground="#0066cc")
        self.notify_text.pack(fill=BOTH, expand=True)

        # ── 状态栏 ──────────────────────────────────────────────
        self.status_var = StringVar(value="就绪")
        bar = ttk.Frame(self.root, relief=SUNKEN, borderwidth=1)
        bar.pack(fill=X, side=BOTTOM)
        ttk.Label(bar, textvariable=self.status_var, style="Status.TLabel").pack(
            side=LEFT, padx=6, pady=2,
        )

    def _get_char_info_by_handle(self, handle: int) -> Optional[dict]:
        """根据 handle 查找特征值信息"""
        if not self._service_data:
            return None
        for svc in self._service_data:
            for char in svc["children"]:
                if char["handle"] == handle:
                    return char
        return None

    def _on_svc_tree_double_click(self, event=None):
        """双击服务树：特征值弹出读写对话框"""
        if not self._connected or not self.engine:
            return
        sel = self.svc_tree.selection()
        if not sel:
            return
        item = self.svc_tree.item(sel[0])
        # 确定类型：特征值（有父节点且父节点无祖父节点）
        parent_id = self.svc_tree.parent(sel[0])
        if not parent_id:
            return  # 服务节点，忽略
        grandparent_id = self.svc_tree.parent(parent_id) if parent_id else ""
        if grandparent_id:
            return  # 描述符节点，忽略

        # 是特征值节点
        values = item["values"]
        char_uuid = values[0]  # UUID
        handle = int(values[2]) if values[2] else 0
        char_info = self._get_char_info_by_handle(handle)
        props = char_info["properties"] if char_info else []

        self._show_char_dialog(char_uuid, handle, props)

    def _show_char_dialog(self, char_uuid: str, handle: int, props: list[str]):
        """显示特征值读写对话框"""
        dialog = Toplevel(self.root)
        dialog.title(f"特征值操作 — {uuid_short(char_uuid)}")
        dialog.geometry("520x420")
        dialog.minsize(400, 300)
        dialog.transient(self.root)
        dialog.update_idletasks()
        dialog.grab_set()

        main = ttk.Frame(dialog, padding=12)
        main.pack(fill=BOTH, expand=True)

        # ── 基本信息 ────────────────────────────────────────────
        info_f = ttk.LabelFrame(main, text="基本信息", padding=8)
        info_f.pack(fill=X, pady=(0, 8))

        ttk.Label(info_f, text=f"UUID：{char_uuid}", font=("", 9)).pack(anchor=W)
        ttk.Label(info_f, text=f"Handle：0x{handle:04X} ({handle})", font=("", 9)).pack(anchor=W)
        ttk.Label(info_f, text=f"属性：{gatt_props_display(props)}", font=("", 9)).pack(anchor=W)

        can_read = "read" in props
        can_write = "write" in props or "write-without-response" in props
        write_with_resp = "write" in props

        # ── 读取区域 ────────────────────────────────────────────
        if can_read:
            read_f = ttk.LabelFrame(main, text="读取", padding=8)
            read_f.pack(fill=X, pady=(0, 8))

            self._read_result_var = StringVar(value="（点击读取按钮）")
            read_row = ttk.Frame(read_f)
            read_row.pack(fill=X)
            btn_read = ttk.Button(read_row, text="读取",
                                  command=lambda: self._do_read_char(char_uuid, dialog))
            btn_read.pack(side=LEFT, padx=(0, 6))
            ttk.Entry(read_row, textvariable=self._read_result_var, width=50,
                      font=("Consolas", 9)).pack(side=LEFT, fill=X, expand=True)

        # ── 写入区域 ────────────────────────────────────────────
        if can_write:
            write_f = ttk.LabelFrame(main, text="写入", padding=8)
            write_f.pack(fill=X, pady=(0, 8))

            ttk.Label(write_f, text="数据格式：").pack(anchor=W)
            self._write_mode = StringVar(value="text")
            mode_row = ttk.Frame(write_f)
            mode_row.pack(fill=X, pady=(0, 4))
            ttk.Radiobutton(mode_row, text="文本", variable=self._write_mode,
                            value="text").pack(side=LEFT, padx=(0, 10))
            ttk.Radiobutton(mode_row, text="Hex", variable=self._write_mode,
                            value="hex").pack(side=LEFT)

            self._write_input_var = StringVar()
            ttk.Entry(write_f, textvariable=self._write_input_var,
                     font=("Consolas", 10)).pack(fill=X, pady=(0, 4))

            self._write_status_var = StringVar()
            write_row = ttk.Frame(write_f)
            write_row.pack(fill=X)
            btn_write = ttk.Button(write_row, text="写入",
                                   command=lambda: self._do_write_char(
                                       char_uuid, write_with_resp, dialog))
            btn_write.pack(side=LEFT, padx=(0, 6))
            ttk.Label(write_row, textvariable=self._write_status_var,
                     font=("", 8), foreground="gray").pack(side=LEFT)

        # ── 关闭按钮 ────────────────────────────────────────────
        ttk.Button(main, text="关闭", command=dialog.destroy).pack(pady=(8, 0))

    def _do_read_char(self, char_uuid: str, dialog):
        """异步读取特征值，结果回填对话框"""
        if not self.engine:
            return

        def _callback(data):
            if data is None:
                self._read_result_var.set("读取失败")
                return
            hex_str = data.hex()
            try:
                text = data.decode("utf-8", errors="replace")
                if text.isprintable():
                    self._read_result_var.set(text)
                else:
                    self._read_result_var.set(hex_str)
            except Exception:
                self._read_result_var.set(hex_str)

        self._run_async(self.engine.read_char(char_uuid), callback=_callback)

    def _do_write_char(self, char_uuid: str, with_response: bool, dialog):
        """异步写入特征值"""
        if not self.engine:
            return
        raw = self._write_input_var.get().strip()
        if not raw:
            self._write_status_var.set("输入为空")
            return

        try:
            if self._write_mode.get() == "hex":
                data = bytes.fromhex(raw.replace(" ", ""))
            else:
                data = raw.encode("utf-8")
        except ValueError as e:
            self._write_status_var.set(f"格式错误: {e}")
            return

        def _callback(success):
            if success:
                uuid_s = uuid_short(char_uuid)
                self._write_status_var.set("✓ 写入成功")
                self.log_queue.put_nowait(("log", f"写入 {uuid_s}: {data.hex()}"))
            else:
                self._write_status_var.set("✗ 写入失败")

        self._run_async(
            self.engine.write_char(char_uuid, data, with_response=with_response),
            callback=_callback,
        )

    # ────────────────────────────────────────────────────────────────
    #  事件处理
    # ────────────────────────────────────────────────────────────────
    def _on_device_select(self, _event=None):
        sel = self.device_tree.selection()
        if sel:
            item = self.device_tree.item(sel[0])
            addr = item["values"][1]
            for dev in self._devices:
                if dev.address == addr:
                    if self._selected_device is not dev:
                        self._selected_device = dev
                        self.on_connect()
                    elif self._connected:
                        # 已连接同一设备，不做任何操作
                        return
                    else:
                        self.on_connect()
                    return
        self._selected_device = None

    def on_scan(self):
        """扫描 BLE 设备"""
        # 清空列表
        for item in self.device_tree.get_children():
            self.device_tree.delete(item)
        self._devices = []
        self._selected_device = None
        self.svc_status_text = ""
        self.scan_status_var.set("扫描中...")
        self.status_var.set("正在扫描 BLE 设备...")
        # 清空服务树
        for item in self.svc_tree.get_children():
            self.svc_tree.delete(item)

        # 创建引擎（如需）
        if self.engine is None:
            self.engine = BLEScannerEngine(self.log_queue, self.notify_queue)

        self._run_async(self.engine.scan(timeout=10.0), callback=self._on_scan_done)

    def _on_scan_done(self, devices: list[BLEDevice]):
        if not devices:
            self.scan_status_var.set("未发现设备")
            self.status_var.set("扫描完成，未发现设备")
            return

        self._devices = devices
        for dev in devices:
            rssi = getattr(dev, "rssi", "N/A")
            self.device_tree.insert("", END, values=(dev.name, dev.address, rssi))

        self.scan_status_var.set(f"发现 {len(devices)} 个设备")
        self.status_var.set(f"扫描完成，发现 {len(devices)} 个设备")

    def on_connect(self):
        """连接选中的设备并发现所有服务"""
        if not self._selected_device:
            return
        if self.engine is None:
            return

        self._service_data = None

        # 清空服务树
        for item in self.svc_tree.get_children():
            self.svc_tree.delete(item)

        adapter = self.adapter_var.get().strip() or "hci0"
        self.status_var.set(f"正在连接 {self._selected_device.name} ...")
        self.log_queue.put_nowait(("log", f"--- 连接 {self._selected_device.name} ---"))

        if self._connected:
            coro = self.engine.reconnect_and_discover(self._selected_device, adapter=adapter)
        else:
            coro = self.engine.connect_and_discover(self._selected_device, adapter=adapter)

        self._run_async(coro, callback=self._on_discover_done)

    def _on_discover_done(self, services: Optional[list[dict]]):
        if not services:
            messagebox.showerror("错误", "连接失败或无法获取服务")
            self.status_var.set("连接失败")
            self._connected = False
            self._update_buttons()
            return

        self._connected = True
        self._service_data = services
        self._update_buttons()

        # 填充服务树
        for svc in services:
            svc_short = uuid_short(svc["uuid"])
            svc_name = svc["description"] or "Primary Service"
            svc_id = self.svc_tree.insert("", END, text=svc_short,
                                          values=(svc["uuid"], svc_name, svc["handle"]),
                                          open=True)

            for char in svc["children"]:
                props_str = gatt_props_display(char["properties"])
                char_short = uuid_short(char["uuid"])
                if "notify" in char["properties"]:
                    props_str += " 📡"

                # 查找该特征值的 User Description (0x2901) 描述符值
                user_desc = ""
                for desc in char["children"]:
                    if desc["uuid"] == "00002901-0000-1000-8000-00805f9b34fb" and desc.get("value"):
                        user_desc = f" — {desc['value']}"
                        break

                desc_count = len(char["children"])
                char_info_text = f"特征值 {props_str}{user_desc}"
                if desc_count:
                    char_info_text += f"  ({desc_count} 个描述符)"
                char_id = self.svc_tree.insert(svc_id, END, text=char_short,
                                               values=(char["uuid"], char_info_text, char["handle"]),
                                               open=True)

                for desc in char["children"]:
                    desc_short = uuid_short(desc["uuid"])
                    desc_name = descriptor_name(desc["uuid"])
                    desc_info = desc_name or ""
                    if desc.get("value"):
                        desc_info += f" → \"{desc['value']}\""
                    self.svc_tree.insert(char_id, END, text=desc_short,
                                         values=(desc["uuid"], desc_info, desc["handle"]))

        self.status_var.set(f"已连接: {self._selected_device.name} — {len(services)} 个服务")
        self.log_queue.put_nowait(
            ("log", f"共 {len(services)} 个服务, "
                    f"{sum(len(s['children']) for s in services)} 个特征值")
        )

    def on_disconnect(self):
        if self.engine is None:
            return
        self._run_async(self.engine.disconnect(), callback=self._on_disconnect_done)

    def _on_disconnect_done(self, _=None):
        self._connected = False
        self._service_data = None
        # 清空服务树
        for item in self.svc_tree.get_children():
            self.svc_tree.delete(item)
        self._update_buttons()
        self.status_var.set("已断开")

    # ────────────────────────────────────────────────────────────────
    #  日志轮询
    # ────────────────────────────────────────────────────────────────
    def _poll_log(self):
        while True:
            try:
                tag, msg = self.log_queue.get_nowait()
                self.log_text.config(state=NORMAL)
                self.log_text.insert(END, msg + "\n")
                self.log_text.see(END)
                self.log_text.config(state=DISABLED)
            except queue.Empty:
                break
        self.root.after(LOG_POLL_MS, self._poll_log)

    def _poll_notify(self):
        while True:
            try:
                tag, uuid_s, data = self.notify_queue.get_nowait()
                ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                self.notify_text.config(state=NORMAL)
                self.notify_text.insert(END, f"[{ts}] [{uuid_s}] {data}\n")
                self.notify_text.see(END)
                self.notify_text.config(state=DISABLED)
            except queue.Empty:
                break
        self.root.after(LOG_POLL_MS, self._poll_notify)

    def _on_close(self):
        self.log_queue.put_nowait(("log", "正在关闭..."))
        if self.engine and self._loop and not self._loop.is_closed():
            asyncio.run_coroutine_threadsafe(self.engine.disconnect(), self._loop)
            self._loop.call_soon_threadsafe(self._loop.stop)
        self.root.destroy()


# ══════════════════════════════════════════════════════════════════
#  入口
# ══════════════════════════════════════════════════════════════════
def main():
    app = BLEScannerGUI()
    app.root.mainloop()


if __name__ == "__main__":
    main()
