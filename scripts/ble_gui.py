# -*- coding: utf-8 -*-
"""
BLE设备连接工具 - 图形界面版本
功能:
1. 扫描和连接BLE设备
2. 保持连接状态
3. 快速测试各种命令
"""

import asyncio
import sys
import os
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError
from bleak.backends.characteristic import BleakGATTCharacteristic

# Nordic UART Service (NUS) 特征UUID
par_nus_service = "0000ffe0-0000-1000-8000-00805f9b34fb"
par_tx_characteristic = "0000ffe1-0000-1000-8000-00805f9b34fb"  # TX - 用于接收数据(通知)
par_rx_characteristic = "0000ffe2-0000-1000-8000-00805f9b34fb"  # RX - 用于发送数据(写入)
par_device_addr = "10:00:3B:D1:43:36"

# 连接参数
CONNECTION_TIMEOUT = 60.0
MAX_PAYLOAD_SIZE = 255


def checksum16(data: bytes) -> bytes:
    """计算校验和（所有字节相加后取低16位）"""
    checksum = sum(data) & 0xFFFF
    return checksum.to_bytes(2, byteorder='big')


class BLEConnectionManager:
    """BLE连接管理器"""

    def __init__(self, log_callback=None, disconnect_callback=None):
        self.client = None
        self.device = None
        self.is_connected = False
        self.rx_characteristic = None
        self.log_callback = log_callback
        self.disconnect_callback = disconnect_callback  # 断开连接回调
        self.monitoring_task = None  # 连接监控任务

    def log(self, message):
        """记录日志"""
        if self.log_callback:
            self.log_callback(message)

    async def scan_devices(self, scan_duration=5.0):
        """扫描可用的BLE设备"""
        self.log(f"\n{'='*60}")
        self.log(f"正在扫描BLE设备... (持续{scan_duration}秒)")
        self.log(f"{'='*60}")

        try:
            # 使用回调函数扫描设备，避免 FutureWarning
            found_devices = {}

            def detection_callback(device, advertisement_data):
                """检测到设备时的回调"""
                found_devices[device.address] = {
                    'device': device,
                    'advertisement': advertisement_data,
                    'name': device.name or advertisement_data.local_name or "未知设备",
                    'rssi': advertisement_data.rssi
                }

            scanner = BleakScanner(detection_callback)
            await scanner.start()
            await asyncio.sleep(scan_duration)
            await scanner.stop()

            if not found_devices:
                self.log("未发现任何BLE设备!")
                return []

            self.log(f"\n发现 {len(found_devices)} 个BLE设备:\n")
            self.log(f"{'序号':<6} {'地址':<18} {'名称':<30} {'RSSI'}")
            self.log("-" * 70)

            device_list = []
            for idx, (addr, info) in enumerate(found_devices.items(), 1):
                device = info['device']
                name = info['name']
                rssi = info['rssi']
                marker = " <- 目标设备" if device.address.upper() == par_device_addr.upper() else ""
                self.log(f"{idx:<6} {device.address:<18} {name:<30} {rssi}{marker}")
                device_list.append({
                    'index': idx,
                    'address': device.address,
                    'name': name,
                    'rssi': rssi,
                    'device': device
                })

            return device_list

        except Exception as e:
            self.log(f"✗ 扫描失败: {e}")
            import traceback
            traceback.print_exc()
            return []

    async def connect_device(self, address):
        """连接到指定设备"""
        self.log(f"\n正在查找设备: {address}")

        try:
            await asyncio.sleep(0.5)
            device = await BleakScanner.find_device_by_address(
                address,
                timeout=10.0,
                cb=dict(use_bdaddr=False)
            )

            if not device:
                self.log(f"✗ 未找到地址为 {address} 的设备")
                return False

            self.log(f"✓ 找到设备: {device.name or '未知'} ({device.address})")
            self.device = device

            self.log(f"\n正在连接到设备...")
            self.client = BleakClient(
                device,
                timeout=CONNECTION_TIMEOUT
            )

            await self.client.connect()
            self.log(f"✓ 成功连接到设备!")
            self.log(f"  地址: {self.client.address}")
            self.log(f"  名称: {self.device.name or '未知'}")
            self.log(f"  MTU: {self.client.mtu_size}")

            self.is_connected = True

            # 获取服务并配置特征
            self.log("\n正在获取服务列表...")
            await self.list_services()
            success = await self.configure_rx_characteristic()

            if success:
                # 启动连接监控任务
                self.monitoring_task = asyncio.create_task(self._monitor_connection())

            return success

        except Exception as e:
            self.log(f"✗ 连接错误: {e}")
            return False

    def _find_nus_characteristics(self):
        """查找FFE1(TX)和FFE2(RX)特征"""
        tx_characteristic = None
        rx_characteristic = None

        for service in self.client.services:
            for char in service.characteristics:
                if par_tx_characteristic in char.uuid:
                    tx_characteristic = char
                    self.log(f"  ✓ 找到TX特征 (FFE1): {char.uuid}")

                if par_rx_characteristic in char.uuid:
                    rx_characteristic = char
                    self.rx_characteristic = char
                    self.log(f"  ✓ 找到RX特征 (FFE2): {char.uuid}")

        return tx_characteristic, rx_characteristic

    async def configure_rx_characteristic(self):
        """配置RX特征(FFE2)用于发送数据"""
        self.log(f"\n{'='*60}")
        self.log("配置RX特征(FFE2)")
        self.log(f"{'='*60}")

        try:
            self.log(f"正在查找FFE1(TX)和FFE2(RX)特征...")
            tx_characteristic, rx_characteristic = self._find_nus_characteristics()

            if not rx_characteristic:
                self.log(f"\n✗ 未找到RX特征 ({par_rx_characteristic}) - 无法发送数据!")
                return False

            # 启用TX特征的通知来接收数据
            if tx_characteristic:
                self.log(f"\n正在启用TX特征通知(接收来自设备的数据)...")
                try:
                    await self.client.start_notify(
                        tx_characteristic.uuid,
                        self.notification_handler
                    )
                    self.log(f"✓ 已启用TX特征的通知!")
                except Exception as e:
                    self.log(f"✗ 启用通知失败: {e}")
                    return False

            self.log(f"\n✓ RX特征已就绪，可以发送数据!")
            return True

        except Exception as e:
            self.log(f"✗ 配置RX特征失败: {e}")
            return False

    async def list_services(self):
        """列出设备提供的所有服务"""
        self.log(f"\n{'='*60}")
        self.log("设备服务和特征")
        self.log(f"{'='*60}")

        try:
            services = self.client.services
            for service in services:
                self.log(f"\n服务: {service.uuid}")
                for char in service.characteristics:
                    self.log(f"  特征: {char.uuid}")
                    if par_tx_characteristic in char.uuid:
                        self.log(f"    *** TX特征 (FFE1) - 用于接收数据! ***")
                    if par_rx_characteristic in char.uuid:
                        self.log(f"    *** RX特征 (FFE2) - 用于发送数据! ***")

        except Exception as e:
            self.log(f"✗ 列出服务失败: {e}")

    def notification_handler(self, characteristic: BleakGATTCharacteristic, data: bytearray):
        """通知处理回调函数"""
        self.log(f"\n[收到通知] 数据长度: {len(data)} 字节")
        self.log(f"  数据内容(hex): {data.hex()}")
        try:
            text = data.decode('utf-8', errors='ignore')
            self.log(f"  数据内容(text): {text}")
            
            param_value = ''
            
            # 解析参数响应格式：param_name=value
            if '=' in text:
                parts = text.split('=', 1)
                if len(parts) == 2:
                    param_name = parts[0].strip()
                    param_value = parts[1].strip()
                    # self.log(f"  解析到参数: {param_name} = {param_value}")
                    # 通过日志消息传递参数值给GUI层处理
                    self.log(f"PARSE_PARAM:{param_name}:{param_value}")
        except:
            pass

    async def send_data(self, data: bytearray):
        """向FFE2(RX)特征发送数据"""
        if not (self.client and self.client.is_connected and self.rx_characteristic):
            self.log("✗ 未连接到设备或RX特征未找到")
            return False

        try:
            self.log(f"\n正在发送数据到FFE2(RX)...")
            self.log(f"  数据(hex): {data.hex()}")
            await self.client.write_gatt_char(par_rx_characteristic, data)
            self.log(f"✓ 数据发送成功!")
            return True
        except Exception as e:
            self.log(f"✗ 数据发送失败: {e}")
            return False

    async def send_delete_command(self, filename: str):
        """发送删除文件命令（0xD4）"""
        self.log(f"\n{'='*60}")
        self.log("发送删除文件命令 (0xD4)")
        self.log(f"{'='*60}")

        try:
            filename_bytes = filename.encode('ascii')
            filename_len = len(filename_bytes)

            # 构建待校验数据：长度 + 文件名
            checksum_data = bytes([filename_len]) + filename_bytes
            checksum = checksum16(checksum_data)

            # 组装帧：命令(0xD4) + checksum(2字节) + 长度(1字节) + 文件名
            frame = bytes([0xD4]) + checksum + bytes([filename_len]) + filename_bytes

            self.log(f"文件名: {filename}")
            self.log(f"帧(hex): {frame.hex()}")

            success = await self.send_data(frame)
            return success

        except Exception as e:
            self.log(f"✗ 发送删除命令失败: {e}")
            return False

    async def send_system_command(self, command_type: str):
        """发送系统命令"""
        self.log(f"\n{'='*60}")
        self.log(f"发送系统命令: {command_type}")
        self.log(f"{'='*60}")

        try:
            if command_type == "format":
                # 格式化命令：0xEE 0x66 0x6F 0x72 0x6D 0x61 0x74
                data = bytearray([0xEE, 0x66, 0x6F, 0x72, 0x6D, 0x61, 0x74])
            elif command_type == "dir":
                # 列出目录命令：0xEE 0x64 0x69 0x72
                data = bytearray([0xEE, 0x64, 0x69, 0x72])
            elif command_type == "reset":
                # 重启命令：0xEE 0x72 0x65 0x73 0x65 0x74
                data = bytearray([0xEE, 0x72, 0x65, 0x73, 0x65, 0x74])
            elif command_type == "pm_c":
                # 禁止节能命令：0xEE 0x70 0x6D 0x5F 0x63 ("pm_c")
                data = bytearray([0xEE, 0x70, 0x6D, 0x5F, 0x63])
            elif command_type == "status":
                # 获取4个状态命令：0xEE 0x73 0x74 0x61 0x74 0x75 0x73 ("status")
                data = bytearray([0xEE, 0x73, 0x74, 0x61, 0x74, 0x75, 0x73])
            else:
                self.log(f"✗ 未知命令类型: {command_type}")
                return False

            self.log(f"命令数据(hex): {data.hex()}")
            success = await self.send_data(data)
            return success

        except Exception as e:
            self.log(f"✗ 发送系统命令失败: {e}")
            return False

    async def send_get_param(self, param_key: str):
        """发送获取参数命令（0xA2）"""
        self.log(f"\n{'='*60}")
        self.log("发送获取参数命令 (0xA2)")
        self.log(f"{'='*60}")

        try:
            param_bytes = (param_key + "?").encode('ascii')
            param_len = len(param_bytes)

            # 构建待校验数据：长度 + 参数
            checksum_data = bytes([param_len]) + param_bytes
            checksum = checksum16(checksum_data)

            # 组装帧：命令(0xA2) + checksum(2字节) + 长度(1字节) + 参数
            frame = bytes([0xA2]) + checksum + bytes([param_len]) + param_bytes

            self.log(f"参数名: {param_key}")
            self.log(f"帧(hex): {frame.hex()}")

            success = await self.send_data(frame)
            return success

        except Exception as e:
            self.log(f"✗ 发送获取参数命令失败: {e}")
            return False

    async def send_set_param(self, param_key: str, param_value: str):
        """发送设置参数命令（0xA1）"""
        self.log(f"\n{'='*60}")
        self.log("发送设置参数命令 (0xA1)")
        self.log(f"{'='*60}")

        try:
            param_str = f"{param_key}={param_value}"
            param_bytes = param_str.encode('ascii')
            param_len = len(param_bytes)

            # 构建待校验数据：长度 + 参数
            checksum_data = bytes([param_len]) + param_bytes
            checksum = checksum16(checksum_data)

            # 组装帧：命令(0xA1) + checksum(2字节) + 长度(1字节) + 参数
            frame = bytes([0xA1]) + checksum + bytes([param_len]) + param_bytes

            self.log(f"参数名: {param_key}")
            self.log(f"参数值: {param_value}")
            self.log(f"帧(hex): {frame.hex()}")

            success = await self.send_data(frame)
            return success

        except Exception as e:
            self.log(f"✗ 发送设置参数命令失败: {e}")
            return False

    async def send_image_file(self, filepath: str):
        """发送图片文件"""
        self.log(f"\n{'='*60}")
        self.log("开始图片传输")
        self.log(f"{'='*60}")

        try:
            if not os.path.exists(filepath):
                self.log(f"✗ 文件不存在: {filepath}")
                return False

            with open(filepath, 'rb') as f:
                file_data = f.read()

            filename = os.path.basename(filepath)
            file_size = len(file_data)

            self.log(f"文件信息:")
            self.log(f"  文件名: {filename}")
            self.log(f"  文件大小: {file_size} 字节")

            # 发送0xD1初始化帧
            self.log(f"\n发送0xD1初始化帧...")
            init_frame = self._build_d1_init_frame(file_size, filename)
            if not await self.send_data(init_frame):
                return False

            # 发送0xD2数据帧
            self.log(f"\n发送0xD2数据帧...")
            await self._send_d2_data_frames(file_data)

            self.log(f"\n{'='*60}")
            self.log("✓ 图片传输完成!")
            self.log(f"{'='*60}")
            return True

        except Exception as e:
            self.log(f"✗ 图片传输失败: {e}")
            import traceback
            traceback.print_exc()
            return False

    def _build_d1_init_frame(self, file_size: int, filename: str) -> bytes:
        """构建0xD1初始化帧"""
        filename_bytes = filename.encode('ascii')
        file_size_bytes = file_size.to_bytes(4, byteorder='big')
        length_byte = len(filename_bytes).to_bytes(1, byteorder='big')

        # 构建待校验数据：文件大小 + 长度 + 文件名
        checksum_data = file_size_bytes + length_byte + filename_bytes
        checksum = checksum16(checksum_data)

        # 组装帧
        frame = bytes([0xD1]) + checksum + file_size_bytes + length_byte + filename_bytes
        return frame

    async def _send_d2_data_frames(self, file_data: bytes):
        """发送0xD2数据帧"""
        total_size = len(file_data)
        packet_num = 0

        for offset in range(0, total_size, MAX_PAYLOAD_SIZE):
            packet_num += 1
            payload = file_data[offset:offset + MAX_PAYLOAD_SIZE]
            payload_size = len(payload)

            # 构建数据帧
            data_frame = self._build_d2_data_frame(packet_num, payload)

            # 发送数据
            self.log(f"\n  发送第{packet_num}帧，大小: {payload_size}字节")
            if not await self.send_data(data_frame):
                return False

            await asyncio.sleep(0.01)

        return True

    def _build_d2_data_frame(self, packet_num: int, payload: bytes) -> bytes:
        """构建单个0xD2数据帧"""
        packet_num_bytes = packet_num.to_bytes(4, byteorder='big')
        length_byte = len(payload).to_bytes(1, byteorder='big')

        # 构建待校验数据
        checksum_data = packet_num_bytes + length_byte + payload
        checksum = checksum16(checksum_data)

        # 组装帧
        frame = bytes([0xD2]) + checksum + packet_num_bytes + length_byte + payload
        return frame

    async def _monitor_connection(self):
        """监控连接状态"""
        try:
            while self.is_connected and self.client and self.client.is_connected:
                await asyncio.sleep(2.0)  # 每2秒检查一次

            # 如果循环退出，说明连接已断开
            if self.is_connected:
                self.log(f"\n⚠ 检测到设备断开连接!")
                self.log(f"  连接状态: {self.client.is_connected if self.client else 'None'}")
                self.is_connected = False
                self.client = None

                # 调用断开回调
                if self.disconnect_callback:
                    self.disconnect_callback()

        except asyncio.CancelledError:
            # 任务被取消（正常断开时）
            pass
        except Exception as e:
            self.log(f"✗ 连接监控异常: {e}")
            self.is_connected = False
            self.client = None

            # 调用断开回调
            if self.disconnect_callback:
                self.disconnect_callback()

    async def disconnect(self):
        """断开当前连接"""
        # 取消监控任务
        if self.monitoring_task and not self.monitoring_task.done():
            self.monitoring_task.cancel()
            try:
                await self.monitoring_task
            except asyncio.CancelledError:
                pass
            self.monitoring_task = None

        if self.client and self.client.is_connected:
            self.log(f"\n正在断开连接...")
            try:
                await self.client.stop_notify(par_tx_characteristic)
            except Exception as e:
                self.log(f"  停止通知时出现异常（可忽略）: {e}")

            try:
                await self.client.disconnect()
                self.log("✓ 已断开连接")
            except Exception as e:
                self.log(f"✗ 断开连接失败: {e}")
            finally:
                self.is_connected = False
                self.client = None
        else:
            self.log("当前未连接到设备")

    async def send_nfc_read_command(self):
        """发送NFC读取命令"""
        self.log(f"\n{'='*60}")
        self.log("发送NFC读取命令")
        self.log(f"{'='*60}")

        try:
            # NFC读取命令：0xEE 0x4E 0x46 0x43 (ASCII "NFC")
            frame = bytes([0xEE, 0x4E, 0x46, 0x43])
            self.log(f"命令数据(hex): {frame.hex()}")

            success = await self.send_data(frame)
            return success

        except Exception as e:
            self.log(f"✗ 发送NFC读取命令失败: {e}")
            return False


class BLEGUI:
    """BLE连接工具图形界面"""

    def __init__(self, root):
        self.root = root
        self.root.title("BLE设备连接工具")
        self.root.geometry("900x700")

        self.manager = None
        self.loop = None
        self.loop_thread = None
        self.auto_scroll = True  # 自动滚动开关

        self.setup_ui()

    def setup_ui(self):
        """设置UI界面"""
        # 顶部控制面板
        control_frame = ttk.LabelFrame(self.root, text="连接控制", padding=10)
        control_frame.pack(fill=tk.X, padx=10, pady=5)

        # 设备地址输入
        addr_frame = ttk.Frame(control_frame)
        addr_frame.pack(fill=tk.X, pady=5)
        ttk.Label(addr_frame, text="设备地址:").pack(side=tk.LEFT)
        self.address_entry = ttk.Entry(addr_frame, width=20)
        self.address_entry.pack(side=tk.LEFT, padx=5)
        self.address_entry.insert(0, par_device_addr)

        # 连接/断开按钮
        btn_frame = ttk.Frame(control_frame)
        btn_frame.pack(fill=tk.X, pady=5)
        self.connect_btn = ttk.Button(btn_frame, text="连接", command=self.connect_device)
        self.connect_btn.pack(side=tk.LEFT, padx=5)
        self.disconnect_btn = ttk.Button(btn_frame, text="断开", command=self.disconnect_device, state=tk.DISABLED)
        self.disconnect_btn.pack(side=tk.LEFT, padx=5)
        self.scan_btn = ttk.Button(btn_frame, text="扫描设备", command=self.scan_devices)
        self.scan_btn.pack(side=tk.LEFT, padx=5)

        # 系统命令面板
        sys_frame = ttk.LabelFrame(self.root, text="系统命令", padding=10)
        sys_frame.pack(fill=tk.X, padx=10, pady=5)

        # 系统命令按钮
        sys_btn_frame = ttk.Frame(sys_frame)
        sys_btn_frame.pack(fill=tk.X, pady=5)
        ttk.Button(sys_btn_frame, text="格式化文件系统", command=self.send_format).pack(side=tk.LEFT, padx=5)
        ttk.Button(sys_btn_frame, text="列出目录", command=self.send_dir).pack(side=tk.LEFT, padx=5)
        ttk.Button(sys_btn_frame, text="禁止节能", command=self.send_pm_c).pack(side=tk.LEFT, padx=5)
        ttk.Button(sys_btn_frame, text="重启MCU", command=self.send_reset).pack(side=tk.LEFT, padx=5)
        ttk.Button(sys_btn_frame, text="读取NFC", command=self.send_nfc_read).pack(side=tk.LEFT, padx=5)
        ttk.Button(sys_btn_frame, text="获取4个状态", command=self.send_status).pack(side=tk.LEFT, padx=5)
        ttk.Button(sys_btn_frame, text="获取心跳包", command=self.send_heartbeat).pack(side=tk.LEFT, padx=5)

        # 系统参数面板
        param_frame = ttk.LabelFrame(self.root, text="系统参数", padding=10)
        param_frame.pack(fill=tk.X, padx=10, pady=5)

        # 参数选择
        param_select_frame = ttk.Frame(param_frame)
        param_select_frame.pack(fill=tk.X, pady=5)
        ttk.Label(param_select_frame, text="选择参数:").pack(side=tk.LEFT)
        
        # 参数列表
        param_options = [
            "heartbeat", "run_interval", "show_mac", "backlight", "bg_mode",
            "pos_label", "pos_label_x", "pos_label_y", "role_name",
            "role_type", "role_action", "role_pos", "rolename_label",
            "rolename_label_x", "rolename_label_y", "Comp", "True_Head", "Comp_offset",
            "start_count"
        ]
        self.param_combo = ttk.Combobox(param_select_frame, values=param_options, width=15, state="readonly")
        self.param_combo.pack(side=tk.LEFT, padx=5)
        self.param_combo.current(0)
        self.param_combo.bind("<<ComboboxSelected>>", self.on_param_selected)

        # 值输入框
        ttk.Label(param_select_frame, text="参数值:").pack(side=tk.LEFT)
        self.param_value_entry = ttk.Entry(param_select_frame, width=20)
        self.param_value_entry.pack(side=tk.LEFT, padx=5)

        # 读取和写入按钮
        param_btn_frame = ttk.Frame(param_frame)
        param_btn_frame.pack(fill=tk.X, pady=5)
        ttk.Button(param_btn_frame, text="读取参数", command=self.read_param).pack(side=tk.LEFT, padx=5)
        ttk.Button(param_btn_frame, text="写入参数", command=self.write_param).pack(side=tk.LEFT, padx=5)

        # 参数说明标签
        param_info_frame = ttk.Frame(param_frame)
        param_info_frame.pack(fill=tk.X, pady=5)
        self.param_info_label = ttk.Label(param_info_frame, text="参数说明: 心跳包开关 (0=关闭, 1=开启)")
        self.param_info_label.pack(anchor=tk.W)

        # 文件操作面板
        file_frame = ttk.LabelFrame(self.root, text="文件操作", padding=10)
        file_frame.pack(fill=tk.X, padx=10, pady=5)

        # 删除文件
        del_frame = ttk.Frame(file_frame)
        del_frame.pack(fill=tk.X, pady=5)
        ttk.Label(del_frame, text="删除文件名:").pack(side=tk.LEFT)
        self.delete_entry = ttk.Entry(del_frame, width=20)
        self.delete_entry.pack(side=tk.LEFT, padx=5)
        self.delete_entry.insert(0, "1.sjpg")
        ttk.Button(del_frame, text="删除", command=self.send_delete).pack(side=tk.LEFT, padx=5)

        # 图片传输
        img_frame = ttk.Frame(file_frame)
        img_frame.pack(fill=tk.X, pady=5)
        ttk.Label(img_frame, text="图片文件:").pack(side=tk.LEFT)
        self.image_entry = ttk.Entry(img_frame, width=30)
        self.image_entry.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        ttk.Button(img_frame, text="浏览...", command=self.browse_image).pack(side=tk.LEFT, padx=5)
        ttk.Button(img_frame, text="发送图片", command=self.send_image).pack(side=tk.LEFT, padx=5)

        # 日志显示区域
        log_frame = ttk.LabelFrame(self.root, text="日志输出", padding=10)
        log_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)

        self.log_text = scrolledtext.ScrolledText(log_frame, height=20, wrap=tk.WORD)
        self.log_text.pack(fill=tk.BOTH, expand=True)

        # 日志控制按钮
        control_btn_frame = ttk.Frame(log_frame)
        control_btn_frame.pack(anchor=tk.E)

        ttk.Checkbutton(control_btn_frame, text="自动滚动", 
                      variable=self.create_var_auto_scroll(),
                      command=self.toggle_auto_scroll).pack(side=tk.LEFT, padx=5)
        ttk.Button(control_btn_frame, text="清空日志", 
                  command=self.clear_log).pack(side=tk.LEFT, padx=5)

        # 初始化事件循环
        self.start_event_loop()

    def start_event_loop(self):
        """启动异步事件循环"""
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)

        def run_loop():
            self.loop.run_forever()

        self.loop_thread = threading.Thread(target=run_loop, daemon=True)
        self.loop_thread.start()

        self.log("✓ 事件循环已启动")

    def log(self, message):
        """记录日志到UI"""
        # 处理参数解析消息
        if message.startswith("PARSE_PARAM:"):
            parts = message.split(':', 2)
            if len(parts) == 3:
                param_name = parts[1]
                param_value = parts[2]
                self.root.after(0, lambda: self._update_param_value(param_name, param_value))
            # 不再return，继续执行日志插入
        
        self.log_text.insert(tk.END, message + "\n")
        if self.auto_scroll:
            self.log_text.see(tk.END)
        self.root.update()
    
    def _update_param_value(self, param_name, param_value):
        """更新参数值输入框"""
        # 如果是原始响应，直接填入值框
        # if param_name == "raw_response":
        self.param_value_entry.delete(0, tk.END)
        self.param_value_entry.insert(0, param_value)
        self.log(f"✓ 接收到的数据: {param_value}")
        return
        
        # 检查参数是否在列表中
        param_options = [
            "heartbeat", "run_interval", "show_mac", "backlight", "bg_mode",
            "pos_label", "pos_label_x", "pos_label_y", "role_name",
            "role_type", "role_action", "role_pos", "rolename_label",
            "rolename_label_x", "rolename_label_y", "Comp", "True_Head", "Comp_offset",
            "start_count"
        ]
        
        if param_name in param_options:
            # 设置下拉框选中该参数
            self.param_combo.set(param_name)
            # 更新参数说明
            info_text = self.get_param_info(param_name)
            self.param_info_label.config(text=f"参数说明: {info_text}")
            # 设置参数值
            self.param_value_entry.delete(0, tk.END)
            self.param_value_entry.insert(0, param_value)
            self.log(f"✓ 已自动填入参数: {param_name} = {param_value}")

    def clear_log(self):
        """清空日志"""
        self.log_text.delete(1.0, tk.END)

    def create_var_auto_scroll(self):
        """创建自动滚动变量"""
        import tkinter as tk
        self.auto_scroll_var = tk.BooleanVar(value=True)
        return self.auto_scroll_var

    def toggle_auto_scroll(self):
        """切换自动滚动状态"""
        self.auto_scroll = self.auto_scroll_var.get()

    def run_async(self, coro):
        """在事件循环中运行异步任务"""
        if self.loop and self.loop.is_running():
            asyncio.run_coroutine_threadsafe(coro, self.loop)

    def scan_devices(self):
        """扫描设备"""
        if self.manager and self.manager.is_connected:
            messagebox.showwarning("警告", "请先断开当前连接!")
            return

        def scan_task():
            async def do_scan():
                self.manager = BLEConnectionManager(log_callback=self.log)
                await self.manager.scan_devices()
                await asyncio.sleep(1)

            self.run_async(do_scan())

        threading.Thread(target=scan_task, daemon=True).start()

    def connect_device(self):
        """连接设备"""
        if self.manager and self.manager.is_connected:
            messagebox.showinfo("提示", "设备已连接!")
            return

        address = self.address_entry.get().strip()
        if not address:
            messagebox.showerror("错误", "请输入设备地址!")
            return

        def connect_task():
            async def do_connect():
                self.manager = BLEConnectionManager(
                    log_callback=self.log,
                    disconnect_callback=self.on_disconnect
                )
                success = await self.manager.connect_device(address)
                if success:
                    self.root.after(0, lambda: self.update_connection_state(True))

            self.run_async(do_connect())

        threading.Thread(target=connect_task, daemon=True).start()

    def disconnect_device(self):
        """断开设备"""
        if not (self.manager and self.manager.is_connected):
            return

        def disconnect_task():
            async def do_disconnect():
                await self.manager.disconnect()
                self.root.after(0, lambda: self.update_connection_state(False))

            self.run_async(do_disconnect())

        threading.Thread(target=disconnect_task, daemon=True).start()

    def update_connection_state(self, connected):
        """更新连接状态"""
        if connected:
            self.connect_btn.config(state=tk.DISABLED)
            self.disconnect_btn.config(state=tk.NORMAL)
            self.scan_btn.config(state=tk.DISABLED)
            self.address_entry.config(state=tk.DISABLED)
        else:
            self.connect_btn.config(state=tk.NORMAL)
            self.disconnect_btn.config(state=tk.DISABLED)
            self.scan_btn.config(state=tk.NORMAL)
            self.address_entry.config(state=tk.NORMAL)

    def on_disconnect(self):
        """设备断开回调"""
        # 在主线程中更新UI
        self.root.after(0, lambda: self.update_connection_state(False))
        self.root.after(0, lambda: self.log("\n⚠ 设备已断开连接，请重新连接后再操作"))

    def send_format(self):
        """发送格式化命令"""
        if not self.check_connected():
            return

        def task():
            async def do_format():
                await self.manager.send_system_command("format")
            self.run_async(do_format())

        threading.Thread(target=task, daemon=True).start()

    def send_dir(self):
        """发送列出目录命令"""
        if not self.check_connected():
            return

        def task():
            async def do_dir():
                await self.manager.send_system_command("dir")
            self.run_async(do_dir())

        threading.Thread(target=task, daemon=True).start()

    def send_pm_c(self):
        """发送禁止节能命令"""
        if not self.check_connected():
            return

        def task():
            async def do_pm_c():
                await self.manager.send_system_command("pm_c")
            self.run_async(do_pm_c())

        threading.Thread(target=task, daemon=True).start()

    def send_reset(self):
        """发送重启命令"""
        if not self.check_connected():
            return

        if not messagebox.askyesno("确认", "确定要重启MCU吗?"):
            return

        def task():
            async def do_reset():
                await self.manager.send_system_command("reset")
            self.run_async(do_reset())

        threading.Thread(target=task, daemon=True).start()

    def send_nfc_read(self):
        """发送NFC读取命令"""
        if not self.check_connected():
            return

        def task():
            async def do_nfc_read():
                await self.manager.send_nfc_read_command()
            self.run_async(do_nfc_read())

        threading.Thread(target=task, daemon=True).start()

    def send_status(self):
        """发送获取4个状态命令"""
        if not self.check_connected():
            return

        def task():
            async def do_status():
                await self.manager.send_system_command("status")
            self.run_async(do_status())

        threading.Thread(target=task, daemon=True).start()

    def send_heartbeat(self):
        """发送设置心跳包状态值为2的命令"""
        if not self.check_connected():
            return

        def task():
            async def do_heartbeat():
                await self.manager.send_set_param("heartbeat", "2")
            self.run_async(do_heartbeat())

        threading.Thread(target=task, daemon=True).start()

    def send_delete(self):
        """发送删除文件命令"""
        if not self.check_connected():
            return

        filename = self.delete_entry.get().strip()
        if not filename:
            messagebox.showerror("错误", "请输入文件名!")
            return

        if not messagebox.askyesno("确认", f"确定要删除文件 '{filename}' 吗?"):
            return

        def task():
            async def do_delete():
                await self.manager.send_delete_command(filename)
            self.run_async(do_delete())

        threading.Thread(target=task, daemon=True).start()

    def browse_image(self):
        """浏览图片文件"""
        filepath = filedialog.askopenfilename(
            title="选择图片文件",
            filetypes=[
                ("所有文件", "*.*"),
                ("SJPEG图片", "*.sjpg"),
            ]
        )
        if filepath:
            self.image_entry.delete(0, tk.END)
            self.image_entry.insert(0, filepath)

    def send_image(self):
        """发送图片文件"""
        if not self.check_connected():
            return

        filepath = self.image_entry.get().strip()
        if not filepath:
            messagebox.showerror("错误", "请选择图片文件!")
            return

        if not os.path.exists(filepath):
            messagebox.showerror("错误", "文件不存在!")
            return

        if not messagebox.askyesno("确认", f"确定要发送文件 '{filepath}' 吗?"):
            return

        def task():
            async def do_send():
                await self.manager.send_image_file(filepath)
            self.run_async(do_send())

        threading.Thread(target=task, daemon=True).start()

    def on_param_selected(self, event):
        """参数选择变化时的回调"""
        param_name = self.param_combo.get()
        info_text = self.get_param_info(param_name)
        self.param_info_label.config(text=f"参数说明: {info_text}")

    def get_param_info(self, param_name: str) -> str:
        """获取参数说明"""
        param_info = {
            "heartbeat": "心跳包开关 (0=关闭, 1=开启)",
            "run_interval": "运行间隙（秒）范围: 10~60000",
            "show_mac": "显示MAC (0=关闭, 1=开启)",
            "backlight": "背光开关 (0=关闭, 1=开启)",
            "bg_mode": "底图模式 (1=1张128*128, 0=2张128*64)",
            "pos_label": "显示位置标签 (0=关闭, 1=开启)",
            "pos_label_x": "位置标签X坐标 (范围: 0~128)",
            "pos_label_y": "位置标签Y坐标 (范围: 0~128)",
            "role_name": "角色名称 (字符串, 长度<20)",
            "role_type": "角色类型 (字符串, 长度<20)",
            "role_action": "角色行动 (字符串, 长度<20)",
            "role_pos": "角色位置 (字符串, 长度<20)",
            "rolename_label": "显示角色名称标签 (0=关闭, 1=开启)",
            "rolename_label_x": "角色名称标签X坐标 (范围: 0~128)",
            "rolename_label_y": "角色名称标签Y坐标 (范围: 0~128)",
            "Comp": "指南针方向 (只读, 0~360)",
            "True_Head": "指南针真北 (只读, 0~360)",
            "Comp_offset": "指南针偏移 (范围: 0~360)",
            "start_count": "启动次数 (只读)"
        }
        return param_info.get(param_name, "未知参数")

    def read_param(self):
        """读取参数"""
        if not self.check_connected():
            return

        param_name = self.param_combo.get()
        if not param_name:
            messagebox.showerror("错误", "请选择参数!")
            return

        def task():
            async def do_read():
                await self.manager.send_get_param(param_name)
            self.run_async(do_read())

        threading.Thread(target=task, daemon=True).start()

    def write_param(self):
        """写入参数"""
        if not self.check_connected():
            return

        param_name = self.param_combo.get()
        param_value = self.param_value_entry.get().strip()

        if not param_name:
            messagebox.showerror("错误", "请选择参数!")
            return

        if not param_value:
            messagebox.showerror("错误", "请输入参数值!")
            return

        # 验证只读参数
        readonly_params = ["Comp", "True_Head"]
        if param_name in readonly_params:
            messagebox.showerror("错误", f"参数 '{param_name}' 是只读参数, 不能写入!")
            return

        if not messagebox.askyesno("确认", f"确定要设置参数 '{param_name} = {param_value}' 吗?"):
            return

        def task():
            async def do_write():
                await self.manager.send_set_param(param_name, param_value)
            self.run_async(do_write())

        threading.Thread(target=task, daemon=True).start()

    def check_connected(self):
        """检查连接状态"""
        if not (self.manager and self.manager.is_connected):
            messagebox.showwarning("警告", "请先连接到设备!")
            return False
        return True

    def on_closing(self):
        """窗口关闭事件"""
        if self.loop and self.loop.is_running():
            self.loop.call_soon_threadsafe(self.loop.stop)

        if self.loop_thread and self.loop_thread.is_alive():
            self.loop_thread.join(timeout=1.0)

        self.root.destroy()


def main():
    """主函数"""
    root = tk.Tk()
    app = BLEGUI(root)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()


if __name__ == "__main__":
    main()
