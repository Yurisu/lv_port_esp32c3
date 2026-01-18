# -*- coding: utf-8 -*-
"""
BLE设备连接工具 - 带重试和错误处理
功能:
1. 扫描并列出可用的BLE设备
2. 连接到指定设备并访问FFE2特征
3. 自动重试机制
4. 详细的错误诊断
"""

import asyncio
import sys
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError
from bleak.backends.characteristic import BleakGATTCharacteristic

# 配置参数
# Nordic UART Service (NUS) 特征UUID
par_nus_service = "0000ffe0-0000-1000-8000-00805f9b34fb"
par_tx_characteristic = "0000ffe1-0000-1000-8000-00805f9b34fb"  # TX - 用于接收数据(通知)
par_rx_characteristic = "0000ffe2-0000-1000-8000-00805f9b34fb"  # RX - 用于发送数据(写入)
par_device_addr = "10:00:3B:D1:43:36"

# 连接参数
CONNECTION_TIMEOUT = 60.0  # 连接超时时间(秒) - 增加到60秒
MAX_RETRIES = 3  # 最大重试次数
RETRY_DELAY = 2.0  # 重试延迟(秒)


class BLEConnectionManager:
    """BLE连接管理器"""
    
    def __init__(self):
        self.client = None
        self.device = None
        self.is_connected = False
        
    async def scan_devices(self, scan_duration=5.0):
        """
        扫描可用的BLE设备
        
        Args:
            scan_duration: 扫描持续时间(秒)
        """
        print(f"\n{'='*60}")
        print(f"正在扫描BLE设备... (持续{scan_duration}秒)")
        print(f"{'='*60}")
        
        try:
            devices = await BleakScanner.discover(timeout=scan_duration)
            
            if not devices:
                print("未发现任何BLE设备!")
                return []
            
            print(f"\n发现 {len(devices)} 个BLE设备:\n")
            print(f"{'序号':<6} {'地址':<18} {'名称':<30} {'RSSI'}")
            print("-" * 70)
            
            device_list = []
            for idx, device in enumerate(devices, 1):
                name = device.name or "未知设备"
                rssi = device.rssi or "N/A"
                marker = " <- 目标设备" if device.address.upper() == par_device_addr.upper() else ""
                print(f"{idx:<6} {device.address:<18} {name:<30} {rssi}{marker}")
                device_list.append({
                    'index': idx,
                    'address': device.address,
                    'name': name,
                    'rssi': rssi,
                    'device': device
                })
            
            return device_list
            
        except Exception as e:
            print(f"✗ 扫描失败: {e}")
            return []
    
    async def find_device_by_address(self, address, timeout=10.0):
        """
        通过MAC地址查找设备

        Args:
            address: 设备MAC地址
            timeout: 查找超时时间(秒)
        """
        print(f"\n正在查找设备: {address}")
        print(f"超时时间: {timeout}秒")

        try:
            # 先扫描设备
            await asyncio.sleep(0.5)
            device = await BleakScanner.find_device_by_address(
                address,
                timeout=timeout,
                cb=dict(use_bdaddr=False)  # Windows: use_bdaddr=False
            )

            if device:
                print(f"✓ 找到设备: {device.name or '未知'} ({device.address})")
                return device
            else:
                print(f"✗ 未找到地址为 {address} 的设备")
                return None

        except BleakError as e:
            print(f"✗ 查找设备失败: {e}")
            return None
        except Exception as e:
            print(f"✗ 查找设备时发生错误: {e}")
            return None

    def check_paired_warning(self, device_name):
        """检查并警告已配对的设备"""
        if not device_name:
            return

        print(f"\n⚠ 警告: 设备 '{device_name}' 似乎是一个HID设备")
        print("如果此设备已在Windows中配对,可能无法连接。")
        print("\n如果连接失败,请先:")
        print("  1. 打开Windows设置 → 设备 → 蓝牙")
        print("  2. 找到该设备并选择'移除'")
        print("  3. 或者运行: python ble_unpair.py")
        print("\n按Enter继续...")
        input()
    
    def notification_handler(self, characteristic: BleakGATTCharacteristic, data: bytearray):
        """通知处理回调函数"""
        print(f"\n[收到通知] UUID: {characteristic.uuid}")
        print(f"  数据长度: {len(data)} 字节")
        print(f"  数据内容(hex): {data.hex()}")
        try:
            # 尝试解码为UTF-8字符串
            text = data.decode('utf-8', errors='ignore')
            print(f"  数据内容(text): {text}")
        except:
            pass
    
    def disconnected_callback(self, client):
        """断开连接回调函数"""
        print(f"\n[断开连接] 设备已断开")
        self.is_connected = False
    
    async def connect_with_retry(self, address, max_retries=MAX_RETRIES, retry_delay=RETRY_DELAY):
        """
        带重试机制的设备连接
        
        Args:
            address: 设备MAC地址
            max_retries: 最大重试次数
            retry_delay: 重试延迟(秒)
        """
        for attempt in range(max_retries):
            print(f"\n{'='*60}")
            print(f"连接尝试 {attempt + 1}/{max_retries}")
            print(f"{'='*60}")
            
            try:
                # 查找设备
                device = await self.find_device_by_address(address, timeout=10.0)
                if not device:
                    print("未找到设备,请检查:")
                    print("  1. 设备是否已上电")
                    print("  2. 设备是否处于广播模式")
                    print("  3. 设备是否已被其他设备连接")
                    print("  4. 设备距离是否太远")

                    if attempt < max_retries - 1:
                        print(f"\n{retry_delay}秒后重试...")
                        await asyncio.sleep(retry_delay)
                    continue

                # 检查是否是HID设备(已配对)
                if attempt == 0:  # 只在第一次尝试时警告
                    self.check_paired_warning(device.name)

                # 保存设备信息
                self.device = device

                # 创建断开连接事件
                disconnected_event = asyncio.Event()

                # 尝试连接
                print(f"\n正在连接到设备...")
                print("提示: Windows上BLE连接可能需要较长时间(30-60秒),请耐心等待...")

                # 尝试不同的连接策略
                try:
                    # Windows上可能需要禁用MTU协商来加快连接速度
                    self.client = BleakClient(
                        device,
                        disconnected_callback=lambda client: self.disconnected_callback(client),
                        timeout=CONNECTION_TIMEOUT
                    )

                    print(f"开始连接过程...")
                    await self.client.connect()
                    print(f"✓ 基础连接已建立")
                except Exception as connect_error:
                    print(f"✗ 连接失败: {connect_error}")
                    if attempt < max_retries - 1:
                        print(f"{retry_delay}秒后重试...")
                        await asyncio.sleep(retry_delay)
                        continue
                    else:
                        return False

                print(f"✓ 成功连接到设备!")
                print(f"  地址: {self.client.address}")
                print(f"  名称: {self.device.name or '未知'}")
                print(f"  MTU: {self.client.mtu_size}")

                self.is_connected = True

                # Windows上可能需要手动触发服务发现
                print("\n正在获取服务列表...")
                try:
                    # 尝试获取服务，但如果超时就跳过
                    await asyncio.wait_for(self.client.get_services(), timeout=15.0)
                    print("✓ 服务列表获取成功")
                except asyncio.TimeoutError:
                    print("⚠ 服务发现超时，但连接已建立，继续尝试...")
                except Exception as e:
                    print(f"⚠ 服务发现警告: {e}，继续尝试...")
                
                # 列出服务
                await self.list_services()

                # 配置NUS服务并启用TX特征通知
                success = await self.enable_ffe2_notification()

                if success:
                    # 保持连接,等待断开
                    print(f"\n连接已建立,正在监听设备...")
                    print(f"按 Ctrl+C 断开连接\n")
                    
                    try:
                        await disconnected_event.wait()
                    except KeyboardInterrupt:
                        print("\n用户中断连接...")
                    
                    # 断开连接
                    await self.disconnect()
                    
                return True
                
            except BleakError as e:
                print(f"✗ BLE连接错误: {e}")
                self.is_connected = False
                
                # 断开可能的部分连接
                if self.client and self.client.is_connected:
                    try:
                        await self.client.disconnect()
                    except:
                        pass
                
                if attempt < max_retries - 1:
                    print(f"{retry_delay}秒后重试...")
                    await asyncio.sleep(retry_delay)
                else:
                    print(f"\n已达到最大重试次数 ({max_retries}), 连接失败")
                    return False
                    
            except asyncio.TimeoutError:
                print(f"✗ 操作超时")
                self.is_connected = False
                
                if self.client and self.client.is_connected:
                    try:
                        await self.client.disconnect()
                    except:
                        pass
                
                if attempt < max_retries - 1:
                    print(f"{retry_delay}秒后重试...")
                    await asyncio.sleep(retry_delay)
                else:
                    print(f"\n已达到最大重试次数 ({max_retries}), 连接失败")
                    return False
                    
            except Exception as e:
                print(f"✗ 发生未预期的错误: {e}")
                import traceback
                traceback.print_exc()
                self.is_connected = False
                
                if self.client and self.client.is_connected:
                    try:
                        await self.client.disconnect()
                    except:
                        pass
                
                if attempt < max_retries - 1:
                    print(f"{retry_delay}秒后重试...")
                    await asyncio.sleep(retry_delay)
                else:
                    print(f"\n已达到最大重试次数 ({max_retries}), 连接失败")
                    return False
        
        return False
    
    async def list_services(self):
        """列出设备提供的所有服务"""
        print(f"\n{'='*60}")
        print("设备服务和特征")
        print(f"{'='*60}")

        try:
            services = self.client.services

            for service in services:
                print(f"\n服务: {service.uuid}")
                if service.description:
                    print(f"  描述: {service.description}")

                for char in service.characteristics:
                    # 使用properties属性来判断特征功能
                    properties = []
                    try:
                        char_props = char.properties if hasattr(char, 'properties') else []

                        # 兼容不同后端的属性名称
                        if 'read' in str(char_props) or 'read' in dir(char):
                            properties.append("读")
                        if 'write' in str(char_props) or 'write' in dir(char):
                            properties.append("写")
                        if 'notify' in str(char_props) or 'notify' in dir(char):
                            properties.append("通知")
                        if 'indicate' in str(char_props) or 'indicate' in dir(char):
                            properties.append("指示")
                    except Exception as e:
                        # 如果获取属性失败，使用默认显示
                        pass

                    props_str = "/".join(properties) if properties else "未知"

                    print(f"  特征: {char.uuid}")
                    print(f"    属性: {props_str}")
                    if char.description:
                        print(f"    描述: {char.description}")

                    # 标记NUS特征
                    if par_tx_characteristic in char.uuid:
                        print(f"    *** TX特征 (FFE1) - 用于接收数据! ***")
                    if par_rx_characteristic in char.uuid:
                        print(f"    *** RX特征 (FFE2) - 用于发送数据! ***")

        except Exception as e:
            print(f"✗ 列出服务失败: {e}")
            import traceback
            traceback.print_exc()
    
    async def enable_ffe2_notification(self):
        """配置NUS服务并启用TX特征的通知来接收数据"""
        print(f"\n{'='*60}")
        print("配置Nordic UART Service (NUS)")
        print(f"{'='*60}")

        try:
            # 直接在所有服务中查找FFE1(TX)和FFE2(RX)特征
            # 不要求NUS服务UUID匹配，因为有些设备使用自定义服务UUID
            tx_characteristic = None  # FFE1 - 用于接收数据(通知)
            rx_characteristic = None  # FFE2 - 用于发送数据(写入)

            print(f"正在查找FFE1(TX)和FFE2(RX)特征...")

            for service in self.client.services:
                for char in service.characteristics:
                    if par_tx_characteristic in char.uuid:
                        tx_characteristic = char
                        print(f"  ✓ 找到TX特征 (FFE1): {char.uuid}")
                        print(f"    所属服务: {service.uuid}")

                    if par_rx_characteristic in char.uuid:
                        rx_characteristic = char
                        print(f"  ✓ 找到RX特征 (FFE2): {char.uuid}")
                        print(f"    所属服务: {service.uuid}")

            if not tx_characteristic:
                print(f"\n✗ 未找到TX特征 ({par_tx_characteristic}) - 无法接收数据!")
                return False

            # 启用TX特征的通知来接收数据
            print(f"\n正在启用TX特征通知(接收来自设备的数据)...")

            # 获取TX特征属性
            properties = []
            supports_notify = False
            supports_read = False

            try:
                if hasattr(tx_characteristic, 'properties'):
                    char_props = tx_characteristic.properties
                    props_str = str(char_props)
                    if 'notify' in props_str:
                        properties.append("通知")
                        supports_notify = True
                    if 'read' in props_str:
                        properties.append("读")
                        supports_read = True
            except:
                pass

            props_str = "/".join(properties) if properties else "未知"
            print(f"  TX特征支持: {props_str}")

            # 启用通知
            if supports_notify:
                try:
                    await self.client.start_notify(tx_characteristic.uuid, self.notification_handler)
                    print(f"✓ 已启用TX特征的通知!")
                    print(f"\n现在可以接收来自ESP32-C3的数据了:")
                    print(f"  - 设备发送数据 → FFE1(TX) → Python接收\n")

                    # 如果RX特征存在,显示信息
                    if rx_characteristic:
                        print(f"RX特征(FFE2)可用: 可以向ESP32-C3发送数据")

                    return True
                except Exception as e:
                    print(f"✗ 启用通知失败: {e}")
                    import traceback
                    traceback.print_exc()
                    return False
            else:
                print(f"✗ TX特征不支持通知,无法接收数据")
                return False

        except Exception as e:
            print(f"✗ 配置NUS失败: {e}")
            import traceback
            traceback.print_exc()
            return False

        return False
    
    async def disconnect(self):
        """断开当前连接"""
        if self.client and self.client.is_connected:
            print(f"\n正在断开连接...")
            try:
                await self.client.stop_notify(par_tx_characteristic)
            except:
                pass

            try:
                await self.client.disconnect()
                print("✓ 已断开连接")
            except:
                print("✗ 断开连接失败")
            finally:
                self.is_connected = False
        else:
            print("当前未连接到设备")


async def main():
    """主函数"""
    import platform
    system = platform.system()

    print(f"\n{'='*60}")
    print("BLE设备连接工具")
    print(f"{'='*60}")
    print(f"系统: {system}")
    print(f"目标设备地址: {par_device_addr}")
    print(f"NUS服务UUID: {par_nus_service}")
    print(f"TX特征UUID (接收数据): {par_tx_characteristic}")
    print(f"RX特征UUID (发送数据): {par_rx_characteristic}")
    print(f"连接超时: {CONNECTION_TIMEOUT}秒")
    print(f"最大重试: {MAX_RETRIES}次")

    if system == "Windows":
        print("\nWindows提示:")
        print("- BLE连接可能需要30-60秒")
        print("- 请确保Windows蓝牙适配器已启用")
        print("- 建议关闭其他蓝牙连接")
        print("- 如果连接失败,可以尝试重新扫描后连接")

    manager = BLEConnectionManager()

    while True:
        print(f"\n{'='*60}")
        print("菜单")
        print(f"{'='*60}")
        print("1. 扫描附近设备")
        print("2. 连接到目标设备 (自动重试)")
        print("3. 连接到指定地址")
        print("4. 断开连接")
        print("5. 退出")
        print(f"{'='*60}")

        choice = input("\n请选择操作 (1-5): ").strip()

        if choice == "1":
            # 扫描设备
            devices = await manager.scan_devices(scan_duration=5.0)

        elif choice == "2":
            # 连接到目标设备
            if manager.is_connected:
                print("当前已连接到设备,请先断开!")
                continue

            print(f"\n开始连接到目标设备: {par_device_addr}")
            await manager.connect_with_retry(par_device_addr)

        elif choice == "3":
            # 连接到指定地址
            if manager.is_connected:
                print("当前已连接到设备,请先断开!")
                continue

            address = input(f"\n请输入设备地址 (默认: {par_device_addr}): ").strip()
            if not address:
                address = par_device_addr

            await manager.connect_with_retry(address)

        elif choice == "4":
            # 断开连接
            await manager.disconnect()

        elif choice == "5":
            # 退出
            await manager.disconnect()
            print("\n退出程序!")
            break

        else:
            print("无效的选择,请重新输入!")


if __name__ == "__main__":
    # 检查依赖
    try:
        import bleak
        version = getattr(bleak, '__version__', 'unknown')
        print(f"✓ bleak库已安装 (版本: {version})")
    except ImportError:
        print("✗ 错误: 缺少bleak库!")
        print("请运行以下命令安装依赖:")
        print("  pip install bleak")
        sys.exit(1)

    # 运行主程序
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n程序被用户中断")
        sys.exit(0)
    except Exception as e:
        print(f"\n程序异常终止: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
