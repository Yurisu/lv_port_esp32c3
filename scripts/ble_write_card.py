# -*- coding: utf-8 -*-
"""
NFC写卡工具 - 图形界面版本
功能:
1. 扫描和连接BLE设备
2. 写入16字节十六进制数据到NFC卡
"""

import asyncio
import threading
import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext
from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic

# Nordic UART Service (NUS) 特征UUID
par_nus_service = "0000ffe0-0000-1000-8000-00805f9b34fb"
par_tx_characteristic = "0000ffe1-0000-1000-8000-00805f9b34fb"  # TX - 用于接收数据(通知)
par_rx_characteristic = "0000ffe2-0000-1000-8000-00805f9b34fb"  # RX - 用于发送数据(写入)
par_device_addr = "10:00:3B:D1:43:36"

# 连接参数
CONNECTION_TIMEOUT = 60.0


def checksum16(data: bytes) -> bytes:
    """计算校验和（所有字节相加后取低16位）"""
    checksum = sum(data) & 0xFFFF
    return checksum.to_bytes(2, byteorder='big')


class BLEConnectionManager:
    """BLE连接管理器"""

    def __init__(self, log_callback=None, disconnect_callback=None, read_callback=None):
        self.client = None
        self.device = None
        self.is_connected = False
        self.rx_characteristic = None
        self.log_callback = log_callback
        self.disconnect_callback = disconnect_callback
        self.read_callback = read_callback
        self.monitoring_task = None

    def log(self, message):
        """记录日志"""
        if self.log_callback:
            self.log_callback(message)

    def update_read_display(self, data_text):
        """更新Read数据显示"""
        if self.read_callback:
            self.read_callback(data_text)

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
            self.client = BleakClient(device, timeout=CONNECTION_TIMEOUT)

            await self.client.connect()
            self.log(f"✓ 成功连接到设备!")
            self.log(f"  地址: {self.client.address}")
            self.log(f"  名称: {self.device.name or '未知'}")
            self.log(f"  MTU: {self.client.mtu_size}")

            self.is_connected = True

            # 获取服务并配置特征
            success = await self.configure_rx_characteristic()

            if success:
                # 启动连接监控任务
                self.monitoring_task = asyncio.create_task(self._monitor_connection())

            return success

        except Exception as e:
            error_msg = str(e)
            self.log(f"✗ 连接错误: {error_msg}")
            if "was not found" in error_msg or "not found" in error_msg.lower():
                self.log(f"⚠ 提示: 这种情况很有可能是设备信号不好或设备断电")
                self.log(f"⚠ 建议: 1) 请确认设备已上电  2) 尝试将设备靠近电脑  3) 检查是否有干扰")
            return False

    async def configure_rx_characteristic(self):
        """配置RX特征(FFE2)用于发送数据"""
        self.log(f"\n正在配置RX特征(FFE2)...")

        try:
            # 查找FFE1(TX)和FFE2(RX)特征
            tx_characteristic = None
            rx_characteristic = None

            for service in self.client.services:
                for char in service.characteristics:
                    if par_tx_characteristic in char.uuid:
                        tx_characteristic = char
                        self.log(f"  ✓ 找到TX特征 (FFE1)")

                    if par_rx_characteristic in char.uuid:
                        rx_characteristic = char
                        self.rx_characteristic = char
                        self.log(f"  ✓ 找到RX特征 (FFE2)")

            if not rx_characteristic:
                self.log(f"✗ 未找到RX特征 ({par_rx_characteristic}) - 无法发送数据!")
                return False

            # 启用TX特征的通知来接收数据
            if tx_characteristic:
                self.log(f"\n正在启用TX特征通知...")
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

    def notification_handler(self, characteristic: BleakGATTCharacteristic, data: bytearray):
        """通知处理回调函数"""
        self.log(f"\n[收到通知] 数据长度: {len(data)} 字节")
        self.log(f"  数据内容(hex): {data.hex()}")

        try:
            # 解析数据格式: 命令(1字节) + checksum(2字节) + 长度(1字节) + 数据
            if len(data) >= 4:
                command = data[0]
                checksum = data[1:3]
                length = data[3]
                payload = data[4:]

                self.log(f"  命令: 0x{command:02X}")
                self.log(f"  校验和: {checksum.hex()}")
                self.log(f"  数据长度: {length} 字节")

                # 解析数据内容
                if len(payload) > 0:
                    try:
                        text = payload.decode('utf-8', errors='ignore')
                        # 清理控制字符
                        clean_text = ''.join(c if ord(c) >= 32 or c == '\n' else '?' for c in text)
                        self.log(f"  数据内容(text): {clean_text}")

                        # 特殊处理Read命令数据
                        if 'Read:' in clean_text:
                            self.log(f"\n{'='*60}")
                            self.log("✓ 写卡成功! 收到Read数据:")

                            # 提取十六进制数据
                            hex_part = clean_text.replace('Read:', '').strip()
                            self.log(f"  原始数据: {hex_part}")

                            # 解析十六进制数据并格式化显示（传递当前计数+1）
                            read_count = 0
                            if self.read_callback and hasattr(self, '_gui_ref') and self._gui_ref:
                                read_count = self._gui_ref.read_data_count + 1

                            read_display = self.parse_read_data(hex_part, read_count)

                            # 更新Read显示区域
                            self.update_read_display(read_display)

                            # 尝试将十六进制解析为字节并显示
                            try:
                                hex_bytes = bytes.fromhex(hex_part.replace(' ', ''))
                                self.log(f"  字节值: {hex_bytes.hex()}")
                                # 尝试解码为文本
                                try:
                                    decoded = hex_bytes.decode('utf-8', errors='ignore')
                                    self.log(f"  解码文本: {decoded}")
                                except:
                                    pass
                            except:
                                pass
                            self.log(f"{'='*60}")

                    except Exception as e:
                        self.log(f"  解析失败: {e}")
            else:
                # 短数据直接显示
                try:
                    text = data.decode('utf-8', errors='ignore')
                    self.log(f"  数据内容(text): {text}")
                except:
                    pass

        except Exception as e:
            self.log(f"  解析错误: {e}")

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

    async def send_write_card_command(self, data: bytes):
        """发送写卡命令（0xB1）"""
        self.log(f"\n{'='*60}")
        self.log("发送写卡命令 (0xB1)")
        self.log(f"{'='*60}")

        try:
            # 验证数据长度
            if len(data) != 16:
                self.log(f"✗ 数据长度错误: 应为16字节, 实际为{len(data)}字节")
                return False

            self.log(f"写卡数据(hex): {data.hex()}")
            self.log(f"写卡数据(ASCII): {data.decode('ascii', errors='replace')}")

            # 构建待校验数据：长度(1字节) + 数据(16字节)
            length_byte = bytes([16])
            checksum_data = length_byte + data
            checksum = checksum16(checksum_data)

            # 组装帧：命令(0xB1) + checksum(2字节) + 长度(1字节) + 数据(16字节)
            frame = bytes([0xB1]) + checksum + length_byte + data

            self.log(f"帧格式: 命令(0xB1) + checksum(2字节) + 长度(1字节) + 数据(16字节)")
            self.log(f"帧(hex): {frame.hex()}")

            success = await self.send_data(frame)
            return success

        except Exception as e:
            self.log(f"✗ 发送写卡命令失败: {e}")
            import traceback
            traceback.print_exc()
            return False

    async def _monitor_connection(self):
        """监控连接状态"""
        try:
            while self.is_connected and self.client and self.client.is_connected:
                await asyncio.sleep(2.0)

            if self.is_connected:
                self.log(f"\n⚠ 检测到设备断开连接!")
                self.is_connected = False
                self.client = None

                if self.disconnect_callback:
                    self.disconnect_callback()

        except asyncio.CancelledError:
            pass
        except Exception as e:
            self.log(f"✗ 连接监控异常: {e}")
            self.is_connected = False
            self.client = None

            if self.disconnect_callback:
                self.disconnect_callback()

    def parse_read_data(self, hex_part, count):
        """解析Read数据并格式化显示"""
        # 清理数据（去除特殊字符）
        cleaned_hex = hex_part.replace(')', '').replace('(', '').strip()

        # 按空格分割，去除空格后重新格式化为每2字符加空格
        hex_values = cleaned_hex.replace(' ', '')

        # 格式化为每2字符加空格
        formatted_hex = ' '.join([hex_values[i:i+2] for i in range(0, len(hex_values), 2)])

        return formatted_hex

    async def disconnect(self):
        """断开当前连接"""
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


class WriteCardGUI:
    """NFC写卡工具图形界面"""

    def __init__(self, root):
        self.root = root
        self.root.title("NFC写卡工具")
        self.root.geometry("900x800")

        self.manager = None
        self.loop = None
        self.loop_thread = None
        self.auto_scroll = True
        self.read_data_count = 0

        self.setup_ui()

    def setup_ui(self):
        """设置UI界面"""
        # 顶部连接面板
        conn_frame = ttk.LabelFrame(self.root, text="连接控制", padding=10)
        conn_frame.pack(fill=tk.X, padx=10, pady=5)

        addr_frame = ttk.Frame(conn_frame)
        addr_frame.pack(fill=tk.X, pady=5)
        ttk.Label(addr_frame, text="设备地址:").pack(side=tk.LEFT)
        self.address_entry = ttk.Entry(addr_frame, width=20)
        self.address_entry.pack(side=tk.LEFT, padx=5)
        self.address_entry.insert(0, par_device_addr)

        btn_frame = ttk.Frame(conn_frame)
        btn_frame.pack(fill=tk.X, pady=5)
        self.connect_btn = ttk.Button(btn_frame, text="连接", command=self.connect_device)
        self.connect_btn.pack(side=tk.LEFT, padx=5)
        self.disconnect_btn = ttk.Button(btn_frame, text="断开", command=self.disconnect_device, state=tk.DISABLED)
        self.disconnect_btn.pack(side=tk.LEFT, padx=5)

        # 写卡数据面板
        data_frame = ttk.LabelFrame(self.root, text="写卡数据 (16字节)", padding=10)
        data_frame.pack(fill=tk.X, padx=10, pady=5)

        # 数据格式选择
        format_frame = ttk.Frame(data_frame)
        format_frame.pack(fill=tk.X, pady=5)
        ttk.Label(format_frame, text="输入格式:").pack(side=tk.LEFT)
        self.format_var = tk.StringVar(value="hex")
        ttk.Radiobutton(format_frame, text="十六进制", variable=self.format_var, value="hex", command=self.on_format_change).pack(side=tk.LEFT, padx=10)
        ttk.Radiobutton(format_frame, text="ASCII", variable=self.format_var, value="ascii", command=self.on_format_change).pack(side=tk.LEFT, padx=10)

        # 数据输入框
        input_frame = ttk.Frame(data_frame)
        input_frame.pack(fill=tk.X, pady=5)
        ttk.Label(input_frame, text="数据:").pack(side=tk.LEFT)
        self.data_entry = ttk.Entry(input_frame, width=60, font=("Courier", 10))
        self.data_entry.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        self.data_entry.insert(0, "01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00")

        # 常用数据模板
        template_frame = ttk.Frame(data_frame)
        template_frame.pack(fill=tk.X, pady=5)
        ttk.Label(template_frame, text="快速模板:").pack(side=tk.LEFT)
        ttk.Button(template_frame, text="六边形坐标(0,0,0)", command=lambda: self.load_template("hex", "01 00 00 00")).pack(side=tk.LEFT, padx=2)
        ttk.Button(template_frame, text="角色类型", command=lambda: self.load_template("hex", "02 00 00 00")).pack(side=tk.LEFT, padx=2)
        ttk.Button(template_frame, text="清空数据", command=lambda: self.load_template("hex", "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00")).pack(side=tk.LEFT, padx=2)

        # 数据预览
        preview_frame = ttk.Frame(data_frame)
        preview_frame.pack(fill=tk.X, pady=5)
        ttk.Label(preview_frame, text="预览:").pack(side=tk.LEFT)
        self.preview_label = ttk.Label(preview_frame, text="[空]", font=("Courier", 9))
        self.preview_label.pack(side=tk.LEFT, padx=5)

        # 写卡按钮
        write_btn_frame = ttk.Frame(data_frame)
        write_btn_frame.pack(fill=tk.X, pady=10)
        ttk.Button(write_btn_frame, text="验证数据", command=self.validate_data).pack(side=tk.LEFT, padx=5)
        ttk.Button(write_btn_frame, text="写入NFC卡", command=self.write_card).pack(side=tk.LEFT, padx=5)

        # Read数据显示区域
        read_frame = ttk.LabelFrame(self.root, text="读取数据", padding=10)
        read_frame.pack(fill=tk.X, padx=10, pady=5)

        # Read数据计数和控制按钮
        read_info_frame = ttk.Frame(read_frame)
        read_info_frame.pack(fill=tk.X, pady=2)
        self.read_count_label = ttk.Label(read_info_frame, text="已读取: 0 次")
        self.read_count_label.pack(side=tk.LEFT)
        ttk.Button(read_info_frame, text="清空Read数据", command=self.clear_read_data).pack(side=tk.RIGHT)

        # Read数据内容
        input_frame = ttk.Frame(read_frame)
        input_frame.pack(fill=tk.X, pady=5)
        ttk.Label(input_frame, text="数据:").pack(side=tk.LEFT)
        self.read_text = ttk.Entry(input_frame, width=60, font=("Courier", 10))
        self.read_text.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        self.read_text.insert(0, "等待读取数据...")

        # 日志显示区域
        log_frame = ttk.LabelFrame(self.root, text="日志输出", padding=10)
        log_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)

        # 日志控制按钮
        control_btn_frame = ttk.Frame(log_frame)
        control_btn_frame.pack(fill=tk.X, pady=2)

        self.scroll_btn = ttk.Button(control_btn_frame, text="停止滚动",
                                    command=self.toggle_scroll_mode)
        self.scroll_btn.pack(side=tk.LEFT, padx=5)
        ttk.Button(control_btn_frame, text="清空日志",
                  command=self.clear_log).pack(side=tk.LEFT, padx=5)

        self.log_text = scrolledtext.ScrolledText(log_frame, height=12, wrap=tk.WORD)
        self.log_text.pack(fill=tk.BOTH, expand=True)

        self.scroll_btn = ttk.Button(control_btn_frame, text="停止滚动",
                                    command=self.toggle_scroll_mode)
        self.scroll_btn.pack(side=tk.LEFT, padx=5)
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
        self.log_text.insert(tk.END, message + "\n")
        if self.auto_scroll:
            self.log_text.see(tk.END)
        self.root.update()

    def clear_log(self):
        """清空日志"""
        self.log_text.delete(1.0, tk.END)

    def clear_read_data(self):
        """清空Read数据"""
        self.read_text.delete(0, tk.END)
        self.read_text.insert(0, "等待读取数据...")
        self.read_data_count = 0
        self.read_count_label.config(text="已读取: 0 次")

    def add_read_data(self, data_text):
        """添加Read数据"""
        # 清空所有内容，只显示最新的读取数据
        self.read_text.delete(0, tk.END)
        self.read_text.insert(0, data_text)
        self.read_data_count += 1
        self.read_count_label.config(text=f"已读取: {self.read_data_count} 次")

    def toggle_scroll_mode(self):
        """切换滚动模式"""
        self.auto_scroll = not self.auto_scroll
        if self.auto_scroll:
            self.scroll_btn.config(text="停止滚动")
            self.log("✓ 已启用自动滚动")
        else:
            self.scroll_btn.config(text="启用滚动")
            self.log("✓ 已停止自动滚动")

    def run_async(self, coro):
        """在事件循环中运行异步任务"""
        if self.loop and self.loop.is_running():
            asyncio.run_coroutine_threadsafe(coro, self.loop)

    def on_format_change(self):
        """格式切换事件"""
        self.update_preview()

    def load_template(self, format_type, value):
        """加载模板数据"""
        self.format_var.set(format_type)
        if format_type == "hex":
            # 将16字节的模板补全
            hex_values = value.split()
            while len(hex_values) < 16:
                hex_values.append("00")
            self.data_entry.delete(0, tk.END)
            self.data_entry.insert(0, " ".join(hex_values))
        elif format_type == "ascii":
            # 将ASCII补全到16字节
            while len(value) < 16:
                value += "\x00"
            self.data_entry.delete(0, tk.END)
            self.data_entry.insert(0, value)

        self.update_preview()

    def update_preview(self):
        """更新数据预览"""
        try:
            data = self.parse_data_input()
            if data:
                hex_str = " ".join(f"{b:02X}" for b in data)
                ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in data)
                self.preview_label.config(text=f"HEX: {hex_str} | ASCII: {ascii_str}")
            else:
                self.preview_label.config(text="[无效数据]")
        except:
            self.preview_label.config(text="[格式错误]")

    def parse_data_input(self) -> bytes:
        """解析输入数据"""
        format_type = self.format_var.get()
        input_str = self.data_entry.get().strip()

        if format_type == "hex":
            # 解析十六进制
            hex_str = input_str.replace(" ", "").replace("0x", "").replace(",", "")
            if len(hex_str) % 2 != 0:
                raise ValueError("十六进制字符串长度必须为偶数")

            if len(hex_str) != 32:  # 16字节 = 32个十六进制字符
                raise ValueError("必须输入16字节数据")

            data = bytes.fromhex(hex_str)
        else:
            # 解析ASCII
            if len(input_str) > 16:
                raise ValueError("ASCII字符串长度不能超过16字符")

            data = input_str.encode('ascii')
            # 填充到16字节
            if len(data) < 16:
                data = data + b'\x00' * (16 - len(data))

        return data

    def validate_data(self):
        """验证数据格式"""
        try:
            data = self.parse_data_input()
            self.log(f"\n{'='*60}")
            self.log("数据验证通过!")
            self.log(f"{'='*60}")
            self.log(f"数据长度: {len(data)} 字节")
            self.log(f"HEX格式: {data.hex()}")
            self.log(f"ASCII格式: {data.decode('ascii', errors='replace')}")
            self.log("✓ 验证成功: 数据格式正确，长度16字节")
        except ValueError as e:
            self.log(f"✗ 数据验证失败: {e}")
        except Exception as e:
            self.log(f"✗ 数据验证失败: {e}")

    def connect_device(self):
        """连接设备"""
        if self.manager and self.manager.is_connected:
            self.log("⚠ 设备已连接!")
            return

        address = self.address_entry.get().strip()
        if not address:
            self.log("✗ 错误: 请输入设备地址!")
            return

        def connect_task():
            async def do_connect():
                self.manager = BLEConnectionManager(
                    log_callback=self.log,
                    disconnect_callback=self.on_disconnect,
                    read_callback=self.add_read_data
                )
                # 保存GUI引用以便访问计数器
                self.manager._gui_ref = self
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
            self.address_entry.config(state=tk.DISABLED)
        else:
            self.connect_btn.config(state=tk.NORMAL)
            self.disconnect_btn.config(state=tk.DISABLED)
            self.address_entry.config(state=tk.NORMAL)

    def on_disconnect(self):
        """设备断开回调"""
        self.root.after(0, lambda: self.update_connection_state(False))
        self.root.after(0, lambda: self.log("\n⚠ 设备已断开连接，请重新连接后再操作"))

    def write_card(self):
        """写卡"""
        if not self.check_connected():
            return

        try:
            data = self.parse_data_input()
        except ValueError as e:
            self.log(f"✗ 错误: 数据格式错误 - {e}")
            return

        self.log(f"\n{'='*60}")
        self.log("准备写入NFC卡...")
        self.log(f"HEX: {data.hex()}")
        self.log(f"ASCII: {data.decode('ascii', errors='replace')}")

        def task():
            async def do_write():
                await self.manager.send_write_card_command(data)
            self.run_async(do_write())

        threading.Thread(target=task, daemon=True).start()

    def check_connected(self):
        """检查连接状态"""
        if not (self.manager and self.manager.is_connected):
            self.log("⚠ 警告: 请先连接到设备!")
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
    app = WriteCardGUI(root)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()


if __name__ == "__main__":
    main()
