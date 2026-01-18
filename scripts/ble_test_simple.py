# -*- coding: utf-8 -*-
"""
简化的BLE连接测试脚本
用于快速诊断连接问题
"""

import asyncio
import sys
from bleak import BleakClient, BleakScanner

# 配置
par_notification_characteristic = "0000ffe1-0000-1000-8000-00805f9b34fb"
par_device_addr = "10:00:3B:D1:43:36"


def notification_handler(characteristic, data):
    print(f"[通知] {data.hex()}")


async def test_scan():
    """测试扫描功能"""
    print("\n" + "="*60)
    print("测试1: 扫描BLE设备")
    print("="*60)

    print("扫描中(5秒)...")
    devices = await BleakScanner.discover(timeout=5.0)

    if devices:
        print(f"\n找到 {len(devices)} 个设备:")
        for idx, device in enumerate(devices, 1):
            name = device.name or "未知"
            marker = " <-- 目标" if device.address.upper() == par_device_addr.upper() else ""
            print(f"  {idx}. {device.address} - {name}{marker}")
    else:
        print("未找到任何BLE设备")

    return devices


async def test_connect():
    """测试连接功能"""
    print("\n" + "="*60)
    print("测试2: 连接并获取服务")
    print("="*60)

    # 查找设备
    print(f"\n查找设备: {par_device_addr}")
    device = await BleakScanner.find_device_by_address(
        par_device_addr,
        timeout=10.0,
        cb=dict(use_bdaddr=False)
    )

    if not device:
        print("✗ 未找到设备")
        return False

    print(f"✓ 找到设备: {device.name or '未知'}")

    # 连接
    print("\n正在连接(这可能需要30-60秒)...")
    client = BleakClient(device)

    try:
        await client.connect()
        print("✓ 连接成功!")
        print(f"  地址: {client.address}")
        print(f"  MTU: {client.mtu_size}")
    except Exception as e:
        print(f"✗ 连接失败: {e}")
        import traceback
        traceback.print_exc()
        return False

    # 获取服务
    print("\n获取服务列表...")
    try:
        services = client.services
        print(f"✓ 找到 {len(services)} 个服务")

        for service in services:
            print(f"\n  服务: {service.uuid}")
            for char in service.characteristics:
                props = []
                if char.readable: props.append("读")
                if char.writable: props.append("写")
                if char.notify: props.append("通知")
                print(f"    特征: {char.uuid} ({'/'.join(props)})")

                if par_notification_characteristic in char.uuid:
                    print(f"      *** 找到FFE2特征! ***")

                    # 尝试启用通知
                    if char.notify:
                        try:
                            await client.start_notify(char.uuid, notification_handler)
                            print("      ✓ FFE2通知已启用")
                            print("      等待数据(5秒)...")
                            await asyncio.sleep(5.0)
                            await client.stop_notify(char.uuid)
                        except Exception as e:
                            print(f"      ✗ 通知失败: {e}")

    except Exception as e:
        print(f"✗ 获取服务失败: {e}")
        import traceback
        traceback.print_exc()

    # 断开
    print("\n断开连接...")
    await client.disconnect()
    print("✓ 已断开")

    return True


async def test_direct():
    """直接连接测试"""
    print("\n" + "="*60)
    print("测试3: 直接连接(跳过扫描)")
    print("="*60)

    print(f"直接连接到: {par_device_addr}")
    print("提示: 如果此方法有效,说明问题在扫描阶段")

    try:
        # 直接使用地址连接(跳过扫描)
        client = BleakClient(par_device_addr)
        print("连接中...")
        await client.connect()
        print("✓ 连接成功!")
        print(f"  MTU: {client.mtu_size}")
        await client.disconnect()
        return True
    except Exception as e:
        print(f"✗ 失败: {e}")
        return False


async def main():
    print("\n" + "="*60)
    print("BLE连接诊断工具")
    print("="*60)
    print(f"目标设备: {par_device_addr}")

    # 测试1: 扫描
    await test_scan()

    # 测试2: 连接
    success = await test_connect()

    if not success:
        print("\n⚠ 连接测试失败")
        print("\n可能的原因:")
        print("1. 设备未广播(Advertising未开启)")
        print("2. 设备已被其他设备连接")
        print("3. Windows蓝牙驱动问题")
        print("4. 设备距离太远(>5米)")
        print("5. 设备休眠或低电量")
        print("\n建议:")
        print("- 重启ESP32-C3设备")
        print("- 确保设备距离<2米")
        print("- 检查Windows蓝牙设置")
        print("- 尝试重启电脑")

    # 测试3: 直接连接
    print("\n是否尝试直接连接(跳过扫描)? (y/n): ", end="")
    if input().lower() == 'y':
        await test_direct()

    print("\n" + "="*60)
    print("诊断完成")
    print("="*60)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n\n测试被用户中断")
        sys.exit(0)
