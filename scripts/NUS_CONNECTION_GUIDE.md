# Nordic UART Service (NUS) 连接指南

## 重要说明

### NUS特征说明

你的ESP32-C3设备使用 **Nordic UART Service (NUS)** 协议，包含两个特征:

| 特征 | UUID | 方向 | 属性 | 用途 |
|------|------|------|--------|------|
| **TX** (FFE1) | `0000ffe1-0000-1000-8000-00805f9b34fb` | 设备 → Python | 通知 | Python接收来自设备的数据 |
| **RX** (FFE2) | `0000ffe2-0000-1000-8000-00805f9b34fb` | Python → 设备 | 写入 | Python发送数据给设备 |

### ⚠️ 重要: 使用FFE1接收数据!

**错误的配置:**
```python
# ❌ 错误: 尝试启用FFE2的通知
await client.start_notify("0000ffe2-...", handler)
# 错误: characteristic does not support notifications
```

**正确的配置:**
```python
# ✅ 正确: 启用FFE1的通知
await client.start_notify("0000ffe1-...", handler)
# 成功: 可以接收来自ESP32-C3的数据
```

## 工作流程

### 数据接收流程

```
ESP32-C3设备
    ↓ (发送数据)
FFE1 (TX特征) - 通知属性
    ↓
Python脚本 - start_notify(FFE1)
    ↓
接收并显示数据
```

### 数据发送流程 (可选)

```
Python脚本
    ↓ (写入数据)
FFE2 (RX特征) - 写入属性
    ↓
ESP32-C3设备
    ↓
ESP_HIDD_EVENT_NUS_UART_RX_EVT事件触发
    ↓
process_protocol_data()处理
```

## 使用Python脚本

### 连接并接收数据

运行主脚本:
```bash
python ble_connect.py
```

**步骤:**
1. 选择 `2` - 连接到目标设备
2. 等待连接完成
3. **FFE1通知会自动启用**
4. 开始接收来自ESP32-C3的数据

### 成功连接的输出

```
============================================================
配置Nordic UART Service (NUS)
============================================================
✓ 找到NUS服务!
  服务UUID: 0000ffe0-0000-1000-8000-00805f9b34fb

  ✓ 找到TX特征 (FFE1): 0000ffe1-0000-1000-8000-00805f9b34fb
  ✓ 找到RX特征 (FFE2): 0000ffe2-0000-1000-8000-00805f9b34fb

正在启用TX特征通知(接收来自设备的数据)...
  TX特征支持: 通知
✓ 已启用TX特征的通知!

现在可以接收来自ESP32-C3的数据了:
  - 设备发送数据 → FFE1(TX) → Python接收

RX特征(FFE2)可用: 可以向ESP32-C3发送数据
```

### 接收数据示例

当ESP32-C3发送数据时，你会看到:

```
[收到通知] UUID: 0000ffe1-0000-1000-8000-00805f9b34fb
  数据长度: 10 字节
  数据内容(hex): 48656c6c6f576f726c64
  数据内容(text): HelloWorld
```

## 发送数据到ESP32-C3

如果你想向ESP32-C3发送数据,可以添加以下函数:

```python
async def send_to_esp32(self, data):
    """向ESP32-C3发送数据"""
    if not self.client or not self.client.is_connected:
        print("✗ 未连接到设备")
        return False

    try:
        # 将数据转换为bytes
        if isinstance(data, str):
            data_bytes = data.encode('utf-8')
        else:
            data_bytes = bytes(data)

        # 通过FFE2(RX)特征发送
        await self.client.write_gatt_char(
            par_rx_characteristic,
            data_bytes
        )
        print(f"✓ 已发送数据: {data_bytes.hex()}")
        return True
    except Exception as e:
        print(f"✗ 发送失败: {e}")
        return False
```

使用示例:
```python
# 发送文本
await manager.send_to_esp32("Hello ESP32!")

# 发送十六进制数据
await manager.send_to_esp32(bytes([0x01, 0x02, 0x03]))
```

## ESP32-C3端配置

确保ESP32-C3的NUS服务正确配置:

### 服务和特征定义

```c
// NUS服务UUID
#define BLE_NUS_SVC_UUID                  0x0000FFE0

// NUS特征UUID
#define BLE_NUS_CHAR_TX_UUID              0x0000FFE1  // TX - 通知
#define BLE_NUS_CHAR_RX_UUID              0x0000FFE2  // RX - 写入
```

### 发送数据 (ESP32 → Python)

```c
// 通过TX特征发送数据
esp_ble_gatts_send_indicate(
    conn_id,
    tx_char_handle,
    data_length,
    data,
    false
);
```

Python会通过FFE1的通知接收:

```python
def notification_handler(characteristic, data):
    print(f"收到数据: {data.hex()}")
    # 解析你的协议数据
```

### 接收数据 (Python → ESP32)

ESP32-C3在`ESP_HIDD_EVENT_NUS_UART_RX_EVT`事件中处理:

```c
case ESP_HIDD_EVENT_NUS_UART_RX_EVT:
    ESP_LOGI("HIDevent", "收到数据: len=%d", param->nus_uart_rx.length);
    ESP_LOG_BUFFER_HEX("HIDevent", param->nus_uart_rx.data, param->nus_uart_rx.length);

    // 处理接收到的数据
    process_protocol_data(param->nus_uart_rx.data, param->nus_uart_rx.length);
    break;
```

## 常见问题

### Q1: 为什么之前连接失败?

**A:** 之前尝试启用FFE2的通知，但FFE2只支持写入。现在改为启用FFE1的通知，可以正确接收数据了。

### Q2: FFE1和FFE2的区别是什么?

**A:**
- **FFE1 (TX)**: Peripheral(ESP32)发送数据给Central(Python)
  - 属性: 通知/指示
  - Python通过`start_notify(FFE1)`接收

- **FFE2 (RX)**: Central(Python)发送数据给Peripheral(ESP32)
  - 属性: 写入
  - Python通过`write_gatt_char(FFE2, data)`发送

### Q3: 我可以同时发送和接收数据吗?

**A:** 是的!
- 启用FFE1的通知来接收数据
- 使用FFE2的写入来发送数据
- 双向通信都可以正常工作

### Q4: 为什么叫TX和RX?

**A:** 这是Nordic命名规范:
- **TX (Transmit)**: 从Peripheral的角度,设备"发送"数据
- **RX (Receive)**: 从Peripheral的角度,设备"接收"数据

但在实际使用中:
- FFE1 (TX) → Python接收
- FFE2 (RX) → Python发送

## 协议数据格式

根据你的ESP32-C3代码,使用自定义图片传输协议:

### 初始化帧 (0xD1)
```
[命令1字节] [校验和2字节] [文件大小4字节] [长度1字节] [文件名N字节]
```

### 数据帧 (0xD2)
```
[命令1字节] [校验和2字节] [包号4字节] [长度1字节] [数据N字节]
```

Python会自动接收并显示这些数据,你可以在`notification_handler`中添加解析逻辑:

```python
def notification_handler(self, characteristic, data):
    print(f"\n[收到通知] UUID: {characteristic.uuid}")
    print(f"  数据长度: {len(data)} 字节")
    print(f"  数据内容(hex): {data.hex()}")

    # 解析协议
    if len(data) > 0:
        cmd = data[0]
        if cmd == 0xD1:  # 初始化帧
            print("  → 初始化帧")
        elif cmd == 0xD2:  # 数据帧
            print("  → 数据帧")
```

## 测试建议

### 1. 测试连接
```bash
python ble_connect.py
# 选择 2 - 连接到目标设备
```

### 2. 测试数据接收
在ESP32-C3中触发发送数据,例如:
```c
// 在main.c的循环中
if (notifyEN()) {
    const char* test_data = "Test Data";
    esp_err_t ret = nus_uart_send_data(hid_conn_id,
        (const uint8_t*)test_data,
        strlen(test_data));
    if (ret == ESP_OK) {
        ESP_LOGI("HIDtask", "NUS data sent successfully");
    }
}
```

Python应该能接收到:
```
[收到通知] UUID: 0000ffe1-0000-1000-8000-00805f9b34fb
  数据长度: 9 字节
  数据内容(hex): 546573742044617461
  数据内容(text): Test Data
```

### 3. 测试双向通信(可选)
如果需要发送数据到ESP32-C3,可以修改脚本添加发送功能。

## 总结

✅ **接收数据**: 使用FFE1 (TX特征) + start_notify()
✅ **发送数据**: 使用FFE2 (RX特征) + write_gatt_char()
✅ **关键区别**: FFE1用于通知,FFE2用于写入
✅ **通信方向**: FFE1是设备→Python,FFE2是Python→设备

现在运行 `python ble_connect.py` 应该能成功接收ESP32-C3的数据了! 🎉
