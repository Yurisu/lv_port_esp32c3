#!/usr/bin/env python3
"""
Inner LittleFS 文件系统构建脚本 - 兼容LittleFS 2.10
基于官方littlefs-python v0.13.X API示例
"""

import os
import sys
import argparse
from pathlib import Path

try:
    import littlefs
    print(f"✅ 使用 littlefs-python")
except ImportError:
    print("❌ 错误: 需要安装 littlefs-python v0.13.X")
    print("请运行: pip install littlefs-python==0.13.*")
    sys.exit(1)

def build_filesystem(source_dir, output_file, flash_size_kb=1024):
    """构建文件系统"""
    flash_size_bytes = flash_size_kb * 1024
    block_size = 4096  # 4KB per block
    block_count = flash_size_bytes // block_size
    
    print(f"🚀 构建 LittleFS 文件系统")
    print(f"📊 配置: {block_size} bytes/block, {block_count} blocks, {flash_size_kb}KB total")
    
    # 创建文件系统实例
    fs = littlefs.LittleFS(block_size=block_size, block_count=block_count)
    
    source_path = Path(source_dir)
    print(f"📂 源目录: {source_path}")
    if not source_path.exists():
        print(f"❌ 源目录不存在: {source_path}")
        return False
    
    # 遍历源目录，按官方示例的方式处理
    for item in source_path.rglob('*'):
        if item.is_file():
            # 计算相对路径
            rel_path = item.relative_to(source_path)
            fs_path = str(rel_path).replace('\\', '/')  # Windows兼容
            
            # 确保父目录存在
            parent_dirs = []
            current_path = ""
            for part in rel_path.parts[:-1]:  # 除了文件名的所有部分
                current_path = current_path + "/" + part if current_path else part
                parent_dirs.append(current_path)
            
            # 创建所有必要的目录
            for dir_path in parent_dirs:
                try:
                    fs.mkdir(dir_path)
                    print(f"📁 创建目录: {dir_path}")
                except Exception:
                    pass  # 目录可能已存在
            
            # 添加文件
            print(f"📄 添加文件: {item} -> {fs_path}")
            try:
                with open(item, 'rb') as local_file:
                    data = local_file.read()
                
                with fs.open(fs_path, 'wb') as fs_file:
                    fs_file.write(data)
                    
                print(f"✅ 添加成功: {fs_path} ({len(data)} bytes)")
            except Exception as e:
                print(f"❌ 添加失败: {fs_path} - {e}")
                return False
    
    # 验证文件系统内容
    print(f"\n📋 文件系统内容:")
    try:
        def show_dir(path, level=0):
            indent = "  " * level
            items = fs.listdir(path)
            for item in sorted(items):
                item_path = path.rstrip('/') + '/' + item
                if item_path.startswith('//'):
                    item_path = item_path[1:]
                
                try:
                    stat = fs.stat(item_path)
                    if stat.type == 2:  # 目录
                        print(f"{indent}📁 {item}/")
                        show_dir(item_path, level + 1)
                    else:  # 文件
                        print(f"{indent}📄 {item} ({stat.size} bytes)")
                except Exception as e:
                    print(f"{indent}❌ {item} - {e}")
        
        show_dir('/')
    except Exception as e:
        print(f"⚠️  验证失败: {e}")
    
    # 写入输出文件
    print(f"\n💾 写入文件系统镜像: {output_file}")
    try:
        with open(output_file, 'wb') as f:
            f.write(fs.context.buffer)
        
        file_size = os.path.getsize(output_file)
        print(f"✅ 文件系统构建完成! 输出文件大小: {file_size} bytes")
        return True
    except Exception as e:
        print(f"❌ 写入失败: {e}")
        return False

def main():
    parser = argparse.ArgumentParser(description="构建  LittleFS 文件系统")
    parser.add_argument("--source", "-s", 
                       default="littlefs",
                       help="源文件夹路径")
    parser.add_argument("--output", "-o",
                       default="scripts/lfs.bin", 
                       help="输出文件名")
    parser.add_argument("--flash-size", "-f",
                       type=int, default=1*1024,
                       help="Flash 大小 (KB)")
    
    args = parser.parse_args()
    
    script_dir = Path(__file__).parent
    project_dir = script_dir.parent
    
    source_path = project_dir / args.source
    output_path = project_dir / args.output
    
    print(f"🔧 Inner LittleFS 文件系统构建器")
    print(f"📂 源目录: {source_path}")
    print(f"💾 输出文件: {output_path}")
    
    success = build_filesystem(source_path, output_path, args.flash_size)
    return 0 if success else 1

if __name__ == "__main__":
    sys.exit(main())