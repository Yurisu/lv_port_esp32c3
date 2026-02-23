#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
简化版ESP32量产固件生成脚本
只拷贝必需文件并创建简单的烧写批处理
"""

import os
import shutil
from pathlib import Path

def create_simple_production_firmware():
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
    
    # 需要拷贝的关键固件文件列表（只保留必需的）
    firmware_files = [
        ("lvgl-demo.bin", "lvgl-demo.bin"),
        ("bootloader/bootloader.bin", "bootloader.bin"),
        ("partition_table/partition-table.bin", "partition-table.bin"),
    ]
    
    # 拷贝固件文件
    print("\n正在拷贝固件文件...")
    copied_files = []
    
    for src_rel_path, dst_name in firmware_files:
        src_path = build_dir / src_rel_path
        dst_path = output_dir / dst_name
        
        if src_path.exists():
            try:
                shutil.copy2(src_path, dst_path)
                print(f"✓ 已拷贝: {src_rel_path} -> {dst_name}")
                copied_files.append(dst_name)
            except Exception as e:
                print(f"✗ 拷贝失败: {src_rel_path} - {e}")
        else:
            print(f"⚠ 文件不存在: {src_rel_path}")
    
    # 创建简单的run.bat文件
    create_run_bat(output_dir)
    
    print(f"\n=== 完成 ===")
    print(f"已成功拷贝 {len(copied_files)} 个固件文件到 {output_dir}")
    print(f"输出目录内容:")
    for item in output_dir.iterdir():
        print(f"  - {item.name}")
    
    return True

def create_run_bat(output_dir):
    """创建简单的run.bat烧写文件"""
    
    # 创建run.bat内容
    bat_content = f"""@echo off
chcp 65001 >nul
echo ========================================
echo ESP32固件烧写工具
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
echo 正在烧写固件到COM5...
esptool.py --chip esp32c3 -b 460800 --before default_reset --after hard_reset --port COM5 write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 bootloader.bin 0x10000 lvgl-demo.bin 0x8000 partition-table.bin

echo.
echo 烧写完成！
pause
"""
    
    bat_file = output_dir / "run.bat"
    with open(bat_file, 'w', encoding='utf-8') as f:
        f.write(bat_content)
    print("✓ 已创建: run.bat")

if __name__ == "__main__":
    try:
        success = create_simple_production_firmware()
        if success:
            print("\n简化版量产固件包创建成功！")
            print("使用方法：")
            print("1. 进入 output 目录")
            print("2. 双击 run.bat 开始烧写")
        else:
            print("\n创建失败！")
    except Exception as e:
        print(f"\n错误: {e}")
