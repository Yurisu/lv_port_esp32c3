#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32量产固件生成脚本
从build文件夹中拷贝所有必需的固件文件和烧写批处理到output文件夹
"""

import os
import shutil
import sys
from pathlib import Path

def create_production_firmware():
    # 定义路径
    project_root = Path("d:/c3/lv_port_esp32/lv_port_esp32c3")
    build_dir = project_root / "build"
    output_dir = project_root / "output"
    
    # 确保output目录存在
    output_dir.mkdir(exist_ok=True)
    
    print(f"项目根目录: {project_root}")
    print(f"Build目录: {build_dir}")
    print(f"输出目录: {output_dir}")
    
    # 检查build目录是否存在
    if not build_dir.exists():
        print(f"错误: Build目录不存在: {build_dir}")
        return False
    
    # 需要拷贝的关键固件文件列表
    firmware_files = [
        # 应用程序固件
        ("lvgl-demo.bin", "应用程序固件"),
        ("lvgl-demo.elf", "应用程序ELF文件（调试用）"),
        
        # 引导程序
        ("bootloader/bootloader.bin", "引导程序固件"),
        ("bootloader/bootloader.elf", "引导程序ELF文件（调试用）"),
        
        # 分区表
        ("partition_table/partition-table.bin", "分区表"),
        
        # Flash参数文件
        ("flash_args", "Flash参数配置"),
        ("flash_project_args", "项目Flash参数"),
        ("app-flash_args", "应用Flash参数"),
        ("bootloader-flash_args", "引导程序Flash参数"),
        ("partition-table-flash_args", "分区表Flash参数"),
        ("flasher_args.json", "Flasher参数JSON"),
        
        # 其他可能有用的文件
        ("project_description.json", "项目描述文件"),
        ("compile_commands.json", "编译命令文件（IDE支持）"),
    ]
    
    # 拷贝固件文件
    print("\n正在拷贝固件文件...")
    copied_files = []
    
    for src_rel_path, description in firmware_files:
        src_path = build_dir / src_rel_path
        dst_path = output_dir / Path(src_rel_path).name
        
        if src_path.exists():
            try:
                shutil.copy2(src_path, dst_path)
                print(f"✓ 已拷贝: {description}")
                print(f"  源: {src_rel_path}")
                print(f"  目标: {dst_path.name}")
                copied_files.append((src_rel_path, dst_path.name, description))
            except Exception as e:
                print(f"✗ 拷贝失败: {src_rel_path} - {e}")
        else:
            print(f"⚠ 文件不存在: {src_rel_path}")
    
    # 创建批处理文件
    create_batch_files(output_dir, copied_files)
    
    # 创建说明文件
    create_readme(output_dir, copied_files)
    
    print(f"\n=== 完成 ===")
    print(f"已成功拷贝 {len(copied_files)} 个固件文件到 {output_dir}")
    print(f"输出目录内容:")
    for item in output_dir.iterdir():
        print(f"  - {item.name}")
    
    return True

def create_batch_files(output_dir, copied_files):
    """创建Windows批处理烧写文件"""
    
    # 查找必要的文件路径
    app_bin = None
    bootloader_bin = None
    partition_bin = None
    flash_args = None
    
    for src_rel, dst_name, desc in copied_files:
        if "应用程序固件" in desc:
            app_bin = dst_name
        elif "引导程序固件" in desc:
            bootloader_bin = dst_name
        elif "分区表" in desc and "partition-table.bin" in src_rel:
            partition_bin = dst_name
        elif "Flash参数配置" in desc:
            flash_args = dst_name
    
    # 创建主烧写批处理文件
    bat_content = f"""@echo off
chcp 65001 >nul
echo ========================================
echo ESP32量产固件烧写工具
echo ========================================
echo.

REM 检查Python和esptool是否可用
python --version >nul 2>&1
if errorlevel 1 (
    echo 错误: 未找到Python，请安装Python并添加到PATH
    pause
    exit /b 1
)

pip show esptool >nul 2>&1
if errorlevel 1 (
    echo 正在安装esptool...
    pip install esptool
)

echo.
echo 可用的烧写选项:
echo 1. 烧写所有固件（推荐量产使用）
echo 2. 仅烧写应用程序
echo 3. 仅烧写引导程序
echo 4. 仅烧写分区表
echo 5. 擦除整个Flash
echo 6. 查看设备信息
echo.
set /p choice=请选择操作 (1-6): 

if "%choice%"=="1" goto flash_all
if "%choice%"=="2" goto flash_app
if "%choice%"=="3" goto flash_bootloader
if "%choice%"=="4" goto flash_partition
if "%choice%"=="5" goto erase_flash
if "%choice%"=="6" goto device_info
goto invalid_choice

:flash_all
echo.
echo 正在烧写所有固件...
esptool.py --chip esp32c3 --port COMX --baud 460800 write_flash -z 0x0 {bootloader_bin} 0x8000 {partition_bin} 0x10000 {app_bin}
goto end

:flash_app
echo.
echo 正在烧写应用程序...
esptool.py --chip esp32c3 --port COMX --baud 460800 write_flash -z 0x10000 {app_bin}
goto end

:flash_bootloader
echo.
echo 正在烧写引导程序...
esptool.py --chip esp32c3 --port COMX --baud 460800 write_flash -z 0x0 {bootloader_bin}
goto end

:flash_partition
echo.
echo 正在烧写分区表...
esptool.py --chip esp32c3 --port COMX --baud 460800 write_flash -z 0x8000 {partition_bin}
goto end

:erase_flash
echo.
echo 正在擦除Flash...
esptool.py --chip esp32c3 --port COMX erase_flash
goto end

:device_info
echo.
echo 读取设备信息...
esptool.py --chip esp32c3 --port COMX flash_id
echo.
esptool.py --chip esp32c3 --port COMX chip_id
goto end

:invalid_choice
echo.
echo 无效选择，请重新运行脚本并输入1-6之间的数字。

goto end

:end
echo.
echo 操作完成。
pause
"""
    
    bat_file = output_dir / "flash_firmware.bat"
    with open(bat_file, 'w', encoding='utf-8') as f:
        f.write(bat_content)
    print("✓ 已创建: flash_firmware.bat")
    
    # 创建快速烧写脚本（假设COM端口为COM3）
    quick_bat_content = f"""@echo off
chcp 65001 >nul
echo ========================================
echo ESP32快速烧写脚本 (COM3)
echo ========================================
echo.

if not exist "{app_bin}" (
    echo 错误: 找不到应用程序文件 {app_bin}
    pause
    exit /b 1
)

if not exist "{bootloader_bin}" (
    echo 错误: 找不到引导程序文件 {bootloader_bin}
    pause
    exit /b 1
)

if not exist "{partition_bin}" (
    echo 错误: 找不到分区表文件 {partition_bin}
    pause
    exit /b 1
)

echo 正在烧写所有固件到COM3...
esptool.py --chip esp32c3 --port COM3 --baud 460800 write_flash -z 0x0 {bootloader_bin} 0x8000 {partition_bin} 0x10000 {app_bin}

echo.
echo 烧写完成！
pause
"""
    
    quick_bat_file = output_dir / "quick_flash_com3.bat"
    with open(quick_bat_file, 'w', encoding='utf-8') as f:
        f.write(quick_bat_content)
    print("✓ 已创建: quick_flash_com3.bat")
    
    # 创建Linux/Mac版本的shell脚本
    sh_content = f"""#!/bin/bash
echo "========================================"
echo "ESP32 Production Firmware Flasher"
echo "========================================"
echo ""

# 检查esptool是否安装
if ! command -v esptool.py &> /dev/null; then
    echo "Installing esptool..."
    pip3 install esptool
fi

echo "Available options:"
echo "1. Flash all firmware (recommended for production)"
echo "2. Flash application only"
echo "3. Flash bootloader only"
echo "4. Flash partition table only"
echo "5. Erase entire flash"
echo "6. Show device information"
echo ""
read -p "Select option (1-6): " choice

case $choice in
    1)
        echo "Flashing all firmware..."
        esptool.py --chip esp32c3 --port /dev/ttyUSB0 --baud 460800 write_flash -z 0x0 {bootloader_bin} 0x8000 {partition_bin} 0x10000 {app_bin}
        ;;
    2)
        echo "Flashing application..."
        esptool.py --chip esp32c3 --port /dev/ttyUSB0 --baud 460800 write_flash -z 0x10000 {app_bin}
        ;;
    3)
        echo "Flashing bootloader..."
        esptool.py --chip esp32c3 --port /dev/ttyUSB0 --baud 460800 write_flash -z 0x0 {bootloader_bin}
        ;;
    4)
        echo "Flashing partition table..."
        esptool.py --chip esp32c3 --port /dev/ttyUSB0 --baud 460800 write_flash -z 0x8000 {partition_bin}
        ;;
    5)
        echo "Erasing flash..."
        esptool.py --chip esp32c3 --port /dev/ttyUSB0 erase_flash
        ;;
    6)
        echo "Reading device information..."
        esptool.py --chip esp32c3 --port /dev/ttyUSB0 flash_id
        echo ""
        esptool.py --chip esp32c3 --port /dev/ttyUSB0 chip_id
        ;;
    *)
        echo "Invalid selection"
        ;;
esac

echo ""
echo "Operation completed."
"""
    
    sh_file = output_dir / "flash_firmware.sh"
    with open(sh_file, 'w', encoding='utf-8') as f:
        f.write(sh_content)
    
    # 设置执行权限 (在Windows上这不会有实际效果，但在Linux/Mac上有用)
    try:
        os.chmod(sh_file, 0o755)
    except:
        pass
    print("✓ 已创建: flash_firmware.sh")

def create_readme(output_dir, copied_files):
    """创建说明文档"""
    readme_content = f"""# ESP32量产固件包

## 文件说明

本目录包含用于量产ESP32-C3设备的完整固件和烧写工具。

### 固件文件

"""
    
    for src_rel, dst_name, desc in copied_files:
        readme_content += f"- **{dst_name}**: {desc}\n"
    
    readme_content += f"""

### 烧写工具

1. **flash_firmware.bat** - Windows图形界面烧写工具
   - 提供多种烧写选项
   - 自动检查Python和esptool依赖
   - 支持擦除、信息查询等功能

2. **quick_flash_com3.bat** - Windows快速烧写脚本
   - 预设COM3端口
   - 一键烧写所有固件
   - 适用于生产线快速烧写

3. **flash_firmware.sh** - Linux/Mac烧写脚本
   - 适用于Linux或Mac系统
   - 预设/dev/ttyUSB0设备

### 使用方法

#### Windows系统：
1. 双击 `flash_firmware.bat` 打开交互式烧写工具
2. 或双击 `quick_flash_com3.bat` 进行快速烧写（需修改COM端口）

#### Linux/Mac系统：
1. 给脚本执行权限：`chmod +x flash_firmware.sh`
2. 运行脚本：`./flash_firmware.sh`

### 烧写地址分配

- **0x0**: Bootloader (引导程序)
- **0x8000**: Partition Table (分区表)  
- **0x10000**: Application (应用程序)

### 注意事项

1. 烧写前请确保设备进入下载模式
2. 根据实际硬件修改串口号（COM端口或/dev/ttyUSBx）
3. 首次使用可能需要安装Python和esptool
4. 建议量产时使用稳定的波特率（如115200）以确保可靠性
5. 烧写完成后建议重启设备进行验证

### 技术支持

如有问题请联系开发团队。
"""
    
    readme_file = output_dir / "README.md"
    with open(readme_file, 'w', encoding='utf-8') as f:
        f.write(readme_content)
    print("✓ 已创建: README.md")

if __name__ == "__main__":
    try:
        success = create_production_firmware()
        sys.exit(0 if success else 1)
    except KeyboardInterrupt:
        print("\n用户取消操作")
        sys.exit(1)
    except Exception as e:
        print(f"\n错误: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
