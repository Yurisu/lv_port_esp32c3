# -*- coding: utf-8 -*-
"""
Windows蓝牙设备管理工具
用于列出和移除已配对的BLE设备
"""

import subprocess
import sys


def list_paired_devices():
    """列出Windows上已配对的蓝牙设备"""
    print("\n" + "="*60)
    print("已配对的蓝牙设备")
    print("="*60)

    try:
        # 使用PowerShell列出已配对设备
        ps_script = '''
        Get-ChildItem "Registry::HKLM\\SYSTEM\\CurrentControlSet\\Enum\\Bluetooth" -ErrorAction SilentlyContinue | ForEach-Object {
            $adapter = $_
            Get-ChildItem $_.PSPath -ErrorAction SilentlyContinue | ForEach-Object {
                $device = $_
                Get-ChildItem $_.PSPath -ErrorAction SilentlyContinue | ForEach-Object {
                    $instance = $_
                    $props = Get-ItemProperty $instance.PSPath -ErrorAction SilentlyContinue
                    if ($props.FriendlyName) {
                        [PSCustomObject]@{
                            Name = $props.FriendlyName
                            DeviceDesc = $props.DeviceDesc
                            InstanceID = $instance.PSChildName
                        }
                    }
                }
            }
        } | Format-Table -AutoSize
        '''

        result = subprocess.run(
            ['powershell', '-Command', ps_script],
            capture_output=True,
            text=True,
            encoding='gbk',
            errors='ignore'
        )

        if result.stdout and result.stdout.strip():
            print("\n" + result.stdout)
            return True
        else:
            print("未找到已配对的设备")
            return False

    except Exception as e:
        print(f"获取设备列表失败: {e}")
        return False


def remove_device_by_name(device_name):
    """通过设备名称移除设备"""
    print(f"\n尝试移除设备: {device_name}")

    try:
        # 使用PowerShell移除设备
        ps_script = f'''
        $device = Get-PnpDevice | Where-Object {{ $_.FriendlyName -like "*{device_name}*" }}
        if ($device) {{
            $dev = $device | Where-Object {{ $_.Status -eq "OK" }}
            if ($dev) {{
                Write-Host "找到设备: $($dev.FriendlyName)"
                & pnputil /remove-device $dev.InstanceId
                Write-Host "✓ 设备已移除"
            }} else {{
                Write-Host "⚠ 设备未找到或未连接"
            }}
        }} else {{
            Write-Host "✗ 未找到名称包含 '{device_name}' 的设备"
        }}
        '''

        result = subprocess.run(
            ['powershell', '-Command', ps_script],
            capture_output=True,
            text=True,
            encoding='gbk',
            errors='ignore'
        )

        print(result.stdout)
        return True

    except Exception as e:
        print(f"移除设备失败: {e}")
        return False


def check_device_status(target_name):
    """检查目标设备是否已配对"""
    print(f"\n检查设备状态: {target_name}")

    try:
        ps_script = '''
        $paired = Get-ChildItem "Registry::HKLM\\SYSTEM\\CurrentControlSet\\Enum\\Bluetooth" -ErrorAction SilentlyContinue |
        Get-ChildItem -ErrorAction SilentlyContinue |
        Get-ChildItem -ErrorAction SilentlyContinue |
        Get-ItemProperty -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName } |
        Select-Object FriendlyName, DeviceDesc

        if ($paired) {
            Write-Host "已配对的设备:"
            $paired | Format-Table -AutoSize
        } else {
            Write-Host "没有已配对的设备"
        }
        '''

        result = subprocess.run(
            ['powershell', '-Command', ps_script],
            capture_output=True,
            text=True,
            encoding='gbk',
            errors='ignore'
        )

        print(result.stdout)

        # 检查是否包含目标设备
        if target_name.lower() in result.stdout.lower():
            print(f"\n⚠ 警告: 设备 '{target_name}' 似乎已配对!")
            print("这可能会阻止Python脚本连接。")
            print("建议: 先移除此设备,然后再尝试连接。")
            return True
        else:
            print(f"\n✓ 设备 '{target_name}' 未配对,可以连接")
            return False

    except Exception as e:
        print(f"检查失败: {e}")
        return False


def main():
    print("\n" + "="*60)
    print("Windows蓝牙设备管理工具")
    print("="*60)

    while True:
        print("\n" + "="*60)
        print("菜单")
        print("="*60)
        print("1. 列出已配对的设备")
        print("2. 检查目标设备状态")
        print("3. 移除指定设备(按名称)")
        print("4. 检查并移除目标设备")
        print("5. 退出")
        print("="*60)

        choice = input("\n请选择操作 (1-5): ").strip()

        if choice == "1":
            # 列出设备
            list_paired_devices()

        elif choice == "2":
            # 检查设备状态
            target_name = input("\n请输入设备名称(或部分名称): ").strip()
            if target_name:
                check_device_status(target_name)

        elif choice == "3":
            # 移除设备
            target_name = input("\n请输入要移除的设备名称: ").strip()
            if target_name:
                remove_device_by_name(target_name)

        elif choice == "4":
            # 检查并移除
            target_name = "VSchess"  # 默认目标
            print(f"\n目标设备: {target_name}")

            if check_device_status(target_name):
                confirm = input("\n是否移除此设备? (y/n): ").strip().lower()
                if confirm == 'y':
                    remove_device_by_name(target_name)
                    print("\n✓ 现在可以运行Python连接脚本了!")

        elif choice == "5":
            # 退出
            print("\n退出程序!")
            break

        else:
            print("无效的选择,请重新输入!")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n程序被用户中断")
        sys.exit(0)
