# 回传协议（0xFE协议）使用说明

## 协议概述

回传协议用于设备向客户端发送数据，如状态信息、操作结果、图片数据等。

## 应用ID定义

在 `main.c` 文件开头定义了以下应用ID（从0x01开始）：

```c
// 回传协议应用ID定义（从0x01开始）
#define APP_ID_STATUS           0x01  // 状态信息（MAC地址、运行时间等）
#define APP_ID_DELETE           0xD4  // 删除文件响应
#define APP_ID_SYSTEM           0xEE  // 系统命令响应（格式化、列出目录、重启等）
```

**应用ID分配说明**：
- `0x01`: 状态信息，用于发送MAC地址、运行时间等实时数据
- `0xD4`: 删除文件响应，与命令CMD_DELETE_FRAME对应
- `0xEE`: 系统命令响应，与命令CMD_SYS_PREFIX对应，包括格式化、列出目录、重启等

## 帧结构

```
| 字段          | 字节数 | 说明                                    |
|---------------|--------|-----------------------------------------|
| 协议头        | 1      | 固定值 0xFE                             |
| 应用ID        | 1      | 由应用赋值，从 0x01 开始               |
| Checksum校验位| 2      | checksum16(长度+数据)，大端序          |
| 数据长度位    | 1      | N（N为负载字节数），范围 1~255         |
| 数据负载      | N      | 应用数据，N ≤ 255                      |
```

**帧总长度**: 1 + 1 + 2 + 1 + N = 5 + N 字节

## Checksum计算规则

Checksum16 校验所有数据，包括：
- 数据长度位（1字节）
- 数据负载（N字节）

**计算方法**: 将上述所有字节相加，取低16位

**字节序**: 大端序（Big-Endian）

## ESP32端实现

### 函数原型

```c
/**
 * @brief 发送回传协议响应（0xFE协议）
 *
 * @param app_id      应用ID（从0x01开始）
 * @param payload     数据负载（应用数据）
 * @param payload_len 负载长度（1~255）
 * @return ESP_OK 成功，其他失败
 */
static esp_err_t send_upload_response(uint8_t app_id, const uint8_t *payload, uint16_t payload_len);
```

### 使用示例

```c
// 示例1：发送文本数据
const char *status_msg = "Transfer complete!";
send_upload_response(APP_ID_STATUS, (uint8_t*)status_msg, strlen(status_msg));

// 示例2：发送删除结果
char response[128];
snprintf(response, sizeof(response), "Delete: OK - %s", filename);
send_upload_response(APP_ID_DELETE, (uint8_t*)response, strlen(response));

// 示例3：发送目录列表
char *dir_content = lv_port_fs_get_dir_content("/");
if (dir_content) {
    send_upload_response(APP_ID_SYSTEM, (uint8_t*)dir_content, strlen(dir_content));
    free(dir_content);
}
```

## 客户端解析（Python）

### 解析工具

使用 `parse_upload_response.py` 工具来解析和验证协议帧：

```bash
python parse_upload_response.py
```

### Python解析代码

```python
import struct


def checksum16(data: bytes) -> int:
    """计算校验和"""
    return sum(data) & 0xFFFF


def parse_upload_frame(data: bytes) -> dict:
    """解析回传协议帧"""
    if len(data) < 6:
        raise ValueError("Frame too short")

    # 1. 协议头
    protocol_header = data[0]
    if protocol_header != 0xFE:
        raise ValueError(f"Invalid protocol header: 0x{protocol_header:02X}")

    # 2. 应用ID
    app_id = data[1]

    # 3. Checksum校验位（大端序）
    checksum_received = struct.unpack('>H', data[2:4])[0]

    # 4. 数据长度位
    data_length = data[4]

    # 5. 验证总长度
    expected_length = 1 + 1 + 2 + 1 + data_length
    if len(data) != expected_length:
        raise ValueError(f"Length mismatch")

    # 6. 解析数据负载
    payload = data[5:]

    # 7. 验证checksum
    checksum_data = data[4:]  # 从长度位开始到结束
    checksum_calculated = checksum16(checksum_data)

    if checksum_received != checksum_calculated:
        raise ValueError("Checksum mismatch")

    return {
        'app_id': app_id,
        'data_length': data_length,
        'payload': payload,
        'checksum_valid': True
    }


# 在BLE通知回调中使用
def notification_handler(characteristic, data: bytearray):
    """处理接收到的数据"""
    print(f"收到数据: {data.hex()}")

    try:
        result = parse_upload_frame(data)
        print(f"应用ID: 0x{result['app_id']:02X}")
        print(f"数据负载: {result['payload']}")
        print(f"负载长度: {result['data_length']} 字节")

        # 尝试解码为文本
        try:
            text = result['payload'].decode('utf-8')
            print(f"文本内容: {text}")
        except:
            pass

    except Exception as e:
        print(f"解析失败: {e}")
```

## 实际应用示例

### 1. 文件删除结果

**ESP32端**:
```c
handle_delete_frame(const uint8_t *data, uint16_t length) {
    // ... 删除文件逻辑 ...

    char response[128];
    if (ret == LV_FS_RES_OK) {
        snprintf(response, sizeof(response), "Delete: OK - %s", filename);
    } else {
        snprintf(response, sizeof(response), "Delete: Failed - %s", filename);
    }

    send_upload_response(APP_ID_DELETE, (uint8_t*)response, strlen(response));
}
```

**客户端接收到的帧**:
```
FE D4 0068 10 44 65 6C 65 74 65 3A 20 4F 4B 20 2D 20 31 2E 73 6A 70 67
```
- 协议头: FE
- 应用ID: D4 (APP_ID_DELETE)
- Checksum: 0068
- 长度: 10 (16字节)
- 负载: "Delete: OK - 1.sjpg"

### 2. 目录列表

**ESP32端**:
```c
case CMD_DIR_LEN:
    char *dir_content = lv_port_fs_get_dir_content("/");
    if (dir_content) {
        send_upload_response(APP_ID_SYSTEM, (uint8_t*)dir_content, strlen(dir_content));
        free(dir_content);
    }
```

**客户端接收到的帧**:
```
FE EE 023A 28 2F 0A 74 2D 31 2E 73 6A 70 67 20 20 31 30 32 34 30 0A 62 2D 31 2E 73 6A 70 67 20 20 20 35 31 38 35 0A
```
- 协议头: FE
- 应用ID: EE (APP_ID_SYSTEM)
- Checksum: 023A
- 长度: 28 (40字节)
- 负载: "/\nt-1.sjpg  10240\nb-1.sjpg   5185\n"

### 3. 状态信息

**ESP32端**:
```c
char nus_data[30];
sprintf(nus_data, "%02X:%02X:%02X:%02X:%02X:%02X %lu",
        macAddr[0], macAddr[1], macAddr[2],
        macAddr[3], macAddr[4], macAddr[5],
        uptime_seconds);
send_upload_response(APP_ID_STATUS, (uint8_t*)nus_data, strlen(nus_data));
```

**客户端接收到的帧**:
```
FE 01 0456 13 30 3A 31 3A 30 3A 33 42 3A 44 31 3A 34 33 3A 33 36 20 31 32 33
```
- 协议头: FE
- 应用ID: 01 (APP_ID_STATUS)
- Checksum: 0456
- 长度: 13 (19字节)
- 负载: "0:1:0:3B:D1:43:36 123"

## 错误处理

### 常见错误

1. **帧太短**: 收到的字节数少于最小值（6字节）
2. **协议头错误**: 第一个字节不是 0xFE
3. **长度不匹配**: 实际长度与声明的数据长度不符
4. **Checksum错误**: 计算的校验和与接收的不一致

### 调试建议

1. 使用 `ESP_LOG_BUFFER_HEX` 在ESP32端打印发送的帧
2. 在Python端使用 `parse_upload_response.py` 验证帧结构
3. 确保MTU设置合理（建议470字节）
4. 检查BLE连接的稳定性

## 注意事项

1. **负载长度限制**: 1~255字节，超过255字节需要分帧发送
2. **内存分配**: ESP32端使用 `heap_caps_malloc` 分配DMA内存，发送后及时释放
3. **CheckSum范围**: 只校验"数据长度位 + 数据负载"，不包括协议头和应用ID
4. **应用ID**: 从 0x01 开始，根据不同应用场景分配不同的ID。建议使用定义的宏：
   - `APP_ID_STATUS` (0x01): 状态信息
   - `APP_ID_DELETE` (0xD4): 删除文件响应
   - `APP_ID_SYSTEM` (0xEE): 系统命令响应
5. **连接检查**: 发送前必须确认 `notifyEN()` 返回 true

## 测试工具

### 使用解析工具

```bash
cd scripts
python parse_upload_response.py
```

工具会提示输入HEX格式的帧数据，然后显示解析结果。

### 示例测试

输入以下HEX字符串测试：
```
FE0112340B48656C6C6F20576F726C6421
```

预期输出：
- 应用ID: 0x01
- 数据负载: "Hello World!"
- Checksum: ✓ 通过
