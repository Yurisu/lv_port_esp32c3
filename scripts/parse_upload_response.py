#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
回传协议解析工具
用于解析和验证0xFE回传协议的帧结构
"""

import struct


def checksum16(data: bytes) -> int:
    """计算校验和（所有字节相加后取低16位）"""
    checksum = sum(data) & 0xFFFF
    return checksum


def parse_upload_frame(data: bytes) -> dict:
    """
    解析回传协议帧

    帧结构：
    1. 协议头：1字节，固定 0xFE
    2. 应用ID：1字节，由应用赋值，从 0x01 开始
    3. Checksum校验位：2字节，checksum16(长度+数据)，包含长度位和以后的所有字段
    4. 数据长度位：1字节，N（N为负载字节数），范围1~255，随负载变化
    5. 数据负载：N字节，应用数据，具体要求由应用来定，N≤255，尾帧可不足255字节

    返回解析结果字典
    """
    result = {
        'valid': False,
        'error': None,
        'protocol_header': None,
        'app_id': None,
        'checksum_received': None,
        'checksum_calculated': None,
        'checksum_valid': False,
        'data_length': None,
        'payload': None,
        'total_length': len(data)
    }

    # 最小帧长度检查：协议头(1) + 应用ID(1) + checksum(2) + 长度(1) + 负载(至少1) = 6字节
    if len(data) < 6:
        result['error'] = f'Frame too short: {len(data)} bytes (minimum 6 bytes required)'
        return result

    # 1. 解析协议头
    protocol_header = data[0]
    result['protocol_header'] = f'0x{protocol_header:02X}'

    if protocol_header != 0xFE:
        result['error'] = f'Invalid protocol header: 0x{protocol_header:02X} (expected 0xFE)'
        return result

    # 2. 解析应用ID
    app_id = data[1]
    result['app_id'] = f'0x{app_id:02X} ({app_id})'

    # 3. 解析Checksum校验位（大端序）
    checksum_received = struct.unpack('>H', data[2:4])[0]
    result['checksum_received'] = f'0x{checksum_received:04X}'

    # 4. 解析数据长度位
    data_length = data[4]
    result['data_length'] = data_length

    # 检查数据长度是否合法
    if data_length == 0 or data_length > 255:
        result['error'] = f'Invalid data length: {data_length} (must be 1-255)'
        return result

    # 5. 检查总长度是否匹配
    expected_length = 1 + 1 + 2 + 1 + data_length
    if len(data) != expected_length:
        result['error'] = f'Length mismatch: received {len(data)} bytes, expected {expected_length} bytes'
        return result

    # 6. 解析数据负载
    payload = data[5:]
    result['payload'] = payload

    # 7. 计算并验证checksum
    # Checksum校验范围：数据长度位 + 数据负载
    checksum_data = data[4:]  # 从长度位开始到结束
    checksum_calculated = checksum16(checksum_data)
    result['checksum_calculated'] = f'0x{checksum_calculated:04X}'
    result['checksum_valid'] = (checksum_received == checksum_calculated)

    if not result['checksum_valid']:
        result['error'] = f'Checksum mismatch: received 0x{checksum_received:04X}, calculated 0x{checksum_calculated:04X}'
        return result

    result['valid'] = True
    return result


def format_parse_result(result: dict) -> str:
    """格式化解析结果为可读字符串"""
    lines = []
    lines.append("=" * 70)
    lines.append("回传协议帧解析结果")
    lines.append("=" * 70)

    lines.append(f"总长度: {result['total_length']} 字节")
    lines.append(f"有效性: {'✓ 有效' if result['valid'] else '✗ 无效'}")

    if result['error']:
        lines.append(f"错误: {result['error']}")

    lines.append(f"\n帧结构:")
    lines.append(f"  1. 协议头: {result['protocol_header']}")
    lines.append(f"  2. 应用ID: {result['app_id']}")
    lines.append(f"  3. Checksum校验位: {result['checksum_received']}")
    lines.append(f"  4. 数据长度位: {result['data_length']} 字节")

    if result['payload']:
        lines.append(f"  5. 数据负载: {len(result['payload'])} 字节")
        lines.append(f"     Hex: {result['payload'].hex()}")
        try:
            text = result['payload'].decode('utf-8', errors='ignore')
            lines.append(f"     Text: {text}")
        except:
            pass

    lines.append(f"\nChecksum验证:")
    lines.append(f"  接收的Checksum: {result['checksum_received']}")
    lines.append(f"  计算的Checksum: {result['checksum_calculated']}")
    lines.append(f"  校验结果: {'✓ 通过' if result['checksum_valid'] else '✗ 失败'}")

    lines.append("=" * 70)
    return "\n".join(lines)


def build_upload_frame(app_id: int, payload: bytes) -> bytes:
    """
    构建回传协议帧

    Args:
        app_id: 应用ID（从0x01开始）
        payload: 数据负载（1~255字节）

    Returns:
        完整的协议帧
    """
    if len(payload) == 0 or len(payload) > 255:
        raise ValueError(f"Payload length must be 1-255 bytes, got {len(payload)}")

    data_length = len(payload)

    # 构建checksum校验数据：数据长度位 + 数据负载
    checksum_data = bytes([data_length]) + payload
    checksum = checksum16(checksum_data)

    # 构建帧：协议头(1) + 应用ID(1) + checksum(2) + 长度(1) + 负载(N)
    frame = bytes([0xFE, app_id]) + checksum.to_bytes(2, byteorder='big') + bytes([data_length]) + payload

    return frame


def main():
    """主函数：演示协议解析和构建"""
    print("=" * 70)
    print("回传协议（0xFE）测试工具")
    print("=" * 70)

    # 示例1：构建一个简单的帧
    print("\n【示例1】构建并解析一个简单的帧")
    print("-" * 70)

    app_id = 0x01
    payload = b"Hello World!"
    frame = build_upload_frame(app_id, payload)

    print(f"应用ID: 0x{app_id:02X}")
    print(f"数据负载: {payload}")
    print(f"负载长度: {len(payload)} 字节")
    print(f"\n构建的帧 (Hex): {frame.hex()}")
    print(f"帧长度: {len(frame)} 字节")

    # 解析帧
    result = parse_upload_frame(frame)
    print("\n" + format_parse_result(result))

    # 示例2：从HEX字符串解析
    print("\n\n【示例2】从HEX字符串解析帧")
    print("-" * 70)

    hex_string = "FE0112340B48656C6C6F20576F726C6421"
    frame_data = bytes.fromhex(hex_string)
    print(f"HEX字符串: {hex_string}")
    print(f"帧长度: {len(frame_data)} 字节")

    result = parse_upload_frame(frame_data)
    print("\n" + format_parse_result(result))

    # 示例3：无效帧测试
    print("\n\n【示例3】测试无效帧（错误的checksum）")
    print("-" * 70)

    invalid_frame = bytes.fromhex("FE0112340B48656C6C6F20576F726C6420")  # 最后一个字节错误
    print(f"HEX字符串: {invalid_frame.hex()}")

    result = parse_upload_frame(invalid_frame)
    print("\n" + format_parse_result(result))

    # 示例4：用户输入
    print("\n\n【示例4】自定义帧测试")
    print("-" * 70)

    while True:
        try:
            user_input = input("\n请输入HEX格式的帧数据（或按Enter退出）: ").strip()
            if not user_input:
                break

            frame_bytes = bytes.fromhex(user_input)
            result = parse_upload_frame(frame_bytes)
            print("\n" + format_parse_result(result))

        except ValueError as e:
            print(f"✗ 无效的HEX格式: {e}")
        except KeyboardInterrupt:
            break

    print("\n" + "=" * 70)
    print("测试完成")
    print("=" * 70)


if __name__ == "__main__":
    main()
