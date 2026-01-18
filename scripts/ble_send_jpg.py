import asyncio
import platform
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

# BLE设备地址和服务UUID（已配对设备直接填写Windows配对的地址）
DEVICE_ADDRESS = "43:22:D6:1B:8E:A6"  # Windows配对的设备地址（在蓝牙设置中查看）
HID_SERVICE_UUID = "0000180d-0000-1000-8000-00805f9b34fb"  # HID Service UUID
HID_REPORT_UUID = "00002A39-0000-1000-8000-00805f9b34fb"  # HID Report Characteristic UUID
VENDOR_REPORT_ID = 4  # Vendor Report ID

# 分块大小 (考虑BLE MTU限制)
CHUNK_SIZE = 512

# 连接重试次数（应对Windows独占冲突）
CONNECT_RETRY_COUNT = 3
CONNECT_TIMEOUT = 20.0  # 连接超时时间（秒）

async def find_vendor_report_char(client):
    """找到Vendor Report特征"""
    services = await client.get_services()
    
    for service in services:
        if service.uuid.lower() == HID_SERVICE_UUID.lower():
            print(f"找到HID服务: {service.uuid}")
            for char in service.characteristics:
                if char.uuid.lower() == HID_REPORT_UUID.lower():
                    # 检查是否可写
                    if "write" in char.properties or "write-without-response" in char.properties:
                        print(f"找到可写的Report特征: {char.uuid}, Handle: {char.handle}")
                        # 尝试获取Report Reference Descriptor来确认是否为Vendor Report
                        for desc in char.descriptors:
                            print(f"  Descriptor: {desc.uuid}")
                        return char
    
    return None

async def send_file(file_path, device_address):
    """发送文件到已配对的BLE设备"""
    # 连接重试逻辑
    client = None
    for retry in range(CONNECT_RETRY_COUNT):
        try:
            print(f"尝试连接已配对设备 {device_address} (重试 {retry+1}/{CONNECT_RETRY_COUNT})...")
            # Windows特化：禁用BLE缓存、强制使用地址连接
            client = BleakClient(
                device_address,
                timeout=CONNECT_TIMEOUT,
                # Windows专属参数：避免系统独占
                winrt={
                    "use_cached_services": False,
                    "enable_bluetooth_le_peripheral_role": False
                } if platform.system() == "Windows" else {}
            )
            await client.connect()
            print(f"成功连接到已配对设备: {device_address}")
            break
        except BleakError as e:
            print(f"连接失败: {e}")
            if retry == CONNECT_RETRY_COUNT - 1:
                print("所有重试均失败，无法连接已配对设备")
                return
            await asyncio.sleep(2.0)  # 重试前等待
    
    if not client or not client.is_connected:
        return

    try:
        # 找到Vendor Report特征
        vendor_char = await find_vendor_report_char(client)
        if vendor_char is None:
            print("错误: 未找到Vendor Report特征")
            return
        
        vendor_char_uuid = vendor_char.uuid
        print(f"使用Vendor Report特征: {vendor_char_uuid}")

        # 读取文件
        with open(file_path, "rb") as f:
            data = f.read()

        total_length = len(data)
        print(f"文件大小: {total_length} 字节")

        # 构建传输协议数据包
        # 格式: [Report_ID(1字节)][序列号(2字节)][总包数(2字节)][数据段...]
        total_packets = (total_length + CHUNK_SIZE - 1) // CHUNK_SIZE
        print(f"总共需要发送 {total_packets} 个数据包")

        packet_num = 0
        offset = 0
        while offset < total_length:
            chunk = data[offset:offset + CHUNK_SIZE]
            packet_num += 1
            
            # 构建数据包
            packet = bytes([VENDOR_REPORT_ID]) + \
                    packet_num.to_bytes(2, 'big') + \
                    total_packets.to_bytes(2, 'big') + \
                    chunk
            
            await client.write_gatt_char(vendor_char_uuid, packet)
            print(f"发送数据包 {packet_num}/{total_packets}: 大小 {len(chunk)} 字节")
            offset += CHUNK_SIZE
            
            # 避免发送过快（Windows下可适当延长，防止系统阻塞）
            await asyncio.sleep(0.1)

        # 发送结束标志
        end_packet = bytes([VENDOR_REPORT_ID]) + \
                    (total_packets + 1).to_bytes(2, 'big') + \
                    (total_packets + 1).to_bytes(2, 'big') + \
                    b"END"
        await client.write_gatt_char(vendor_char_uuid, end_packet)
        print("文件发送完成")
    finally:
        # 确保断开连接，释放Windows系统资源
        if client.is_connected:
            await client.disconnect()
            print("已断开设备连接")

async def get_paired_device_address():
    """
    获取Windows已配对的VSchess设备地址（可选：手动填写地址更稳定）
    也可直接返回预配置的DEVICE_ADDRESS
    """
    if platform.system() != "Windows":
        return DEVICE_ADDRESS
    
    print("扫描Windows已配对的HID设备...")
    # 扫描时包含已配对设备（BleakScanner默认扫描可发现设备，已配对设备可能不可发现）
    devices = await BleakScanner.discover(
        timeout=10.0,
        # Windows专属：扫描已配对设备
        winrt={"scan_mode": "active"} if platform.system() == "Windows" else {}
    )
    
    for device in devices:
        if device.name and "VSchess" in device.name:
            print(f"找到已配对设备: {device.name} - {device.address}")
            return device.address
    
    # 未扫描到时使用预配置地址
    print(f"未扫描到已配对设备，使用预配置地址: {DEVICE_ADDRESS}")
    return DEVICE_ADDRESS

if __name__ == "__main__":
    import sys
    
    async def main():
        # 适配Windows已配对设备：优先用预配置地址，或自动获取
        device_address = await get_paired_device_address()
        if not device_address:
            print("未找到已配对的VSchess设备")
            print("请检查Windows蓝牙设置中是否已配对该设备")
            return
        
        # 检查文件参数
        if len(sys.argv) != 2:
            print("用法: python ble_send_jpg.py <文件路径>")
            sys.exit(1)
        
        file_path = sys.argv[1]
        await send_file(file_path, device_address)
    
    # Windows下需处理事件循环策略（解决asyncio报错）
    if platform.system() == "Windows":
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    
    asyncio.run(main())