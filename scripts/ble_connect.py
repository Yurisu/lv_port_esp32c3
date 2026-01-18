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
par_notification_characteristic = "0000ffe2-0000-1000-8000-00805f9b34fb"
par_device_addr = "10:00:3B:D1:43:36"

# 连接参数
CONNECTION_TIMEOUT = 30.0  # 连接超时时间(秒)
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

                # 保存设备信息
                self.device = device

                # 创建断开连接事件
                disconnected_event = asyncio.Event()

                # 尝试连接
                print(f"\n正在连接到设备...")
                self.client = BleakClient(
                    device,
                    disconnected_callback=lambda client: self.disconnected_callback(client)
                )

                # 设置连接超时
                try:
                    await asyncio.wait_for(
                        self.client.connect(),
                        timeout=CONNECTION_TIMEOUT
                    )
                except asyncio.TimeoutError:
                    print(f"✗ 连接超时 ({CONNECTION_TIMEOUT}秒)")
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
                
                # 列出服务
                await self.list_services()
                
                # 启用FFE2特征通知
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
                    properties = []
                    if char.readable:
                        properties.append("读")
                    if char.writable:
                        properties.append("写")
                    if char.notify:
                        properties.append("通知")
                    if char.indicate:
                        properties.append("指示")
                    
                    props_str = "/".join(properties) if properties else "无"
                    
                    print(f"  特征: {char.uuid}")
                    print(f"    属性: {props_str}")
                    if char.description:
                        print(f"    描述: {char.description}")
                        
                    # 标记FFE2特征
                    if par_notification_characteristic in char.uuid:
                        print(f"    *** FFE2特征 - 已找到! ***")
                        
        except Exception as e:
            print(f"✗ 列出服务失败: {e}")
    
    async def enable_ffe2_notification(self):
        """启用FFE2特征的通知"""
        print(f"\n{'='*60}")
        print("启用FFE2特征通知")
        print(f"{'='*60}")
        
        try:
            # 查找FFE2特征
            service_uuid = None
            for service in self.client.services:
                for char in service.characteristics:
                    if par_notification_characteristic in char.uuid:
                        service_uuid = service.uuid
                        print(f"✓ 找到FFE2特征!")
                        print(f"  特征UUID: {char.uuid}")
                        print(f"  服务UUID: {service.uuid}")
                        
                        properties = []
                        if char.readable:
                            properties.append("读")
                        if char.writable:
                            properties.append("写")
                        if char.notify:
                            properties.append("通知")
                        
                        props_str = "/".join(properties) if properties else "无"
                        print(f"  支持的操作: {props_str}")
                        
                        # 尝试读取当前值
                        if char.readable:
                            try:
                                value = await self.client.read_gatt_char(char.uuid)
                                print(f"  当前值(hex): {value.hex()}")
                                try:
                                    text = value.decode('utf-8', errors='ignore')
                                    print(f"  当前值(text): {text}")
                                except:
                                    pass
                            except Exception as e:
                                print(f"  读取失败: {e}")
                        
                        # 启用通知
                        if char.notify:
                            try:
                                await self.client.start_notify(char.uuid, self.notification_handler)
                                print(f"✓ 已启用FFE2特征的通知")
                                print(f"  等待接收数据...\n")
                                return True
                            except Exception as e:
                                print(f"✗ 启用通知失败: {e}")
                                return False
                        else:
                            print(f"✗ FFE2特征不支持通知")
                            return False
            
            if not service_uuid:
                print(f"✗ 未找到FFE2特征 ({par_notification_characteristic})")
                return False
                
        except Exception as e:
            print(f"✗ 启用FFE2通知失败: {e}")
            import traceback
            traceback.print_exc()
            return False
        
        return False
    
    async def disconnect(self):
        """断开当前连接"""
        if self.client and self.client.is_connected:
            print(f"\n正在断开连接...")
            try:
                await self.client.stop_notify(par_notification_characteristic)
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
    print(f"\n{'='*60}")
    print("BLE设备连接工具")
    print(f"{'='*60}")
    print(f"目标设备地址: {par_device_addr}")
    print(f"FFE2特征UUID: {par_notification_characteristic}")
    print(f"连接超时: {CONNECTION_TIMEOUT}秒")
    print(f"最大重试: {MAX_RETRIES}次")
    
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
