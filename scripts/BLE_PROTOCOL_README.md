
# 蓝牙通信协议文档

## 一、图片传输协议（主机 → ESP32）

| 命令 | 说明 | 帧结构 | 字段说明 |
|--------|------|----------|----------|
| **0xD1** | 初始化文件传输 | 命令(1) + checksum(2) + 文件大小(4) + 长度(1) + 文件名(N) | checksum校验从文件大小字段开始 |
| **0xD2** | 发送文件数据 | 命令(1) + checksum(2) + 包号(4) + 长度(1) + 数据(N) | 包序从1开始，最大255字节/包 |
| **0xD4** | 删除文件 | 命令(1) + checksum(2) + 长度(1) + 文件名(N) | checksum校验从长度字段开始 |
| **0xB1** | 写卡 | 命令(1) + checksum(2) + 长度(1) + 数据(16) | checksum校验从长度字段开始，写入16字节数据到NFC卡并回读验证 |

**校验规则**：checksum16(校验字段) = 所有字节相加 & 0xFFFF（大端序）

---

## 二、系统命令（主机 → ESP32）

| 命令 | 说明 | 帧结构 | 备注 |
|--------|------|----------|------|
| **0xEE** | 格式化文件系统 | 0xEE 0x66 0x6F 0x72 0x6D 0x61 0x74 | ASCII: "format" |
| **0xEE** | 列出目录 | 0xEE 0x64 0x69 0x72 | ASCII: "dir" |
| **0xEE** | 重启MCU | 0xEE 0x72 0x65 0x73 0x65 0x74 | ASCII: "reset" |
| **0xEE** | NFC触发测量任务 | 0xEE 0x4E 0x46 0x43 | ASCII: "NFC"，等同于按键中断功能 |


---

## 三、系统参数协议（主机 → ESP32）

| 命令 | 说明 | 帧结构 | 参数格式 |
|--------|------|----------|----------|
| **0xA1** | 设置系统参数 | 命令(1) + checksum(2) + 长度(1) + 参数(N) | "key=value" |
| **0xA2** | 获取系统参数 | 命令(1) + checksum(2) + 长度(1) + 参数(N) | "key?" |

**支持的系统参数**：

| 参数名 | 数据类型 | 取值范围 | 说明 |
|--------|----------|----------|------|
| heartbeat | uint8_t | 0或1 | 心跳包：0=关闭, 1=开启, 2=跳一次 |*
| run_interval | uint16_t | 10~60000 | 运行间隙（秒） |*
| show_mac | uint8_t | 0或1 | 显示MAC：0=关闭, 1=开启 |*
| backlight | uint8_t | 0或1 | 背光开关：0=关闭, 1=开启 |*
| bg_mode | uint8_t | 0或1 | 底图模式：1=1张128*128, 0=2张128*64 |*
| img1_file | string | 长度<20 | 图1显示图片文件名 |*
| img2_file | string | 长度<20 | 图2显示图片文件名 |*
| pos_label | uint8_t | 0或1 | 显示位置标签：0=关闭, 1=开启 |
| pos_label_x | uint8_t | 0~128 | 位置标签X坐标 |
| pos_label_y | uint8_t | 0~128 | 位置标签Y坐标 |
| role_name | string | 长度<20 | 角色名称 |
| role_name_label | uint8_t | 0或1 | 显示角色名称标签：0=关闭, 1=开启 |
| role_name_label_x | uint8_t | 0~128 | 角色名称标签X坐标 |
| role_name_label_y | uint8_t | 0~128 | 角色名称标签Y坐标 |
| role_type | string | 长度<20 | 角色类型 |
| role_action | string | 长度<20 | 行动类型 |
| Comp | uint16_t | 0~360 | 指南针方向（只读）同时返回Comp+True_Head |
| True_Head | uint16_t | 0~360 | 指南针真北（只读）同时返回Comp+True_Head |
| Comp_offset | uint16_t | 0~360 | 指南针偏移 |

**使用示例**：
- 设置参数：`0xA1 + checksum + len(14) + "backlight=1"`
- 获取参数：`0xA2 + checksum + len(10) + "run_interval?"`

通过A0命令返回系统参数响应帧
---

## 四、回传协议（ESP32 → 主机）

### 基础帧格式

| 字段 | 大小 | 说明 |
|--------|------|------|
| 协议头 | 1 | 固定 0xFE |
| 应用ID | 1 | 0x01=状态, 0xD4=删除, 0xEE=系统响应, 0xA0=系统参数, 0xCC=角色|
| Checksum | 2 | checksum16(长度+数据)，大端序 |
| 数据长度 | 1 | 数据负载字节数（1~255） |
| 数据负载 | N | 应用数据 |

### 应用ID定义

| APP_ID | 用途 | 说明 |
|---------|--------|------|
| **0x01** | 状态信息 | MAC地址、运行时间、电池电压、电池百分比、温度 |   macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5],uptime_seconds, battery_voltage_mv, battery_capacity, tsens_out |
| **0xD4** | 删除响应 | 文件删除结果 |  Delete: OK - %s", filename // Delete: Failed - %s", filename |
| **0xEE** | 系统响应 | 格式化、目录、重启、NFC触发等命令响应 |
| **0xA0** | 系统参数响应 | 系统参数设置/获取响应 |
| **0xCC** | 角色参数 | 1.角色名称,2.角色类型(设置图片名),3.行动类型(设置图片名),4.六边形坐标xyz,5.指南针真北(无效传-1) |
| **0xB1** | 写卡响应 | 写卡并回读数据 | 返回格式: Read: XXXXXXXX XXXXXXXX XXXXXXXX XXXXXXXX (16字节的十六进制数据) |


### NFC卡片数据格式

| byte1: index      | byte2: 0x00   | byte3: 0x00   | byte4: 0x00 | byte5: 0x00 | byte6: 0x00   | byte7: 0x00   | byte8: 0x00 |
| 0x31 六边形坐标   | text1         | text2         | text3 |...
| 0x32 角色名称     | text1         | text2         | text3 |...
| 0x33 角色类型     | text1         | text2         | text3 |...
| 0x34 行动类型     | text1         | text2         | text3 |...
| 0x11 设置指南针修正方向值 | 0x21(固定)      | 0x31(固定)        |{调用}



### 返回的应用数据

| 字段 | 大小 | 说明 |
|--------|------|------|
| MAC地址 | 6 | 设备MAC地址 |
| 运行时间 | 6 | 设备运行时间（秒） |
| 电池电压 | 4 | 电池电压（mV） |
| 电池百分比 | 4 | 电池（100） |
| 温度 | 4 | 设备温度（0.1°C） |

**状态信息数据格式**（APP_ID=0x01）：MAC地址 + 运行时间 + 电池电压 + 温度
0xfe + 0x01 + MAC地址（6字节）+ 空格0x20 + 运行时间（6字节）+ 空格0x20 + 电池电压（4字节）+ 空格0x20 + 温度（4字节）

**状态信息数据格式**（APP_ID=0xD4）：

**状态信息数据格式**（APP_ID=0xEE）：

**状态信息数据格式**（APP_ID=0xA0）：

**状态信息数据格式**（APP_ID=0xCC）：



**系统参数响应格式**：
- 设置成功：`Set OK: [key]`
- 获取响应：`[key]=[value]`
- 错误：`Error: [message]`

---

## 五、分包协议（ESP32 → 主机）

当数据长度 > 253字节时启用分包：

| 字段 | 大小 | 说明 |
|--------|------|------|
| 总包数 | 1 | 数据分多少包 |
| 包序号 | 1 | 从0开始，标识当前包 |
| 数据负载 | N-2 | 实际数据，每包最多253字节 |

**重组规则**：
- 按包序号0,1,2...顺序接收
- 接收完所有包后重组为完整数据
- 包序号不连续则重置重组器
















## 修改摘要

### 1. **添加 NVS 键定义** (main.c:74-91)
新增了以下 NVS 存储键：
- `NVS_KEY_ROLE_NAME` - 角色名称
- `NVS_KEY_ROLE_TYPE` - 角色类型
- `NVS_KEY_ROLE_ACTION` - 行动类型

### 2. **扩展系统参数结构** (main.c:117-134)
在 `system_params_t` 结构中新增字段：
```c
char role_name[20];      // 角色名称（长度<20）
uint8_t role_type;       // 角色类型：0~255
uint8_t role_action;     // 行动类型：0~255
```

### 3. **设置默认值** (main.c:133-147)
为新增参数设置默认值：
- `role_name` = "default"
- `role_type` = 0
- `role_action` = 0

### 4. **更新参数加载函数** (main.c:566-590)
在 `load_system_params_from_nvs()` 中添加从 NVS 加载三个新参数的逻辑，并包含数据验证。

### 5. **更新参数保存函数** (main.c:576-600)
在 `save_system_params_to_nvs()` 中添加保存三个新参数到 NVS 的逻辑。

### 6. **更新参数设置函数** (main.c:1073-1105)
在 `handle_set_param_frame()` 中添加对以下参数的处理：
- `role_name` - 验证长度<20
- `role_type` - 验证范围 0~255
- `role_action` - 验证范围 0~255

### 7. **更新参数获取函数** (main.c:1178-1192)
在 `handle_get_param_frame()` 中添加对以下参数的获取支持：
- `role_name`
- `role_type`
- `role_action`
- `Comp` (只读参数，返回当前指南针方向 `Compass_Heading`)

## 支持的完整系统参数列表

| 参数名 | 数据类型 | 取值范围 | 说明 |
|--------|----------|----------|------|
| run_interval | uint16_t | 10~60000 | 运行间隙（秒） |
| backlight | uint8_t | 0或1 | 背光开关 |
| bg_mode | uint8_t | 0或1 | 底图模式 |
| show_mac | uint8_t | 0或1 | 显示MAC |
| pos_label | uint8_t | 0或1 | 显示位置标签 |
| pos_label_x | uint8_t | 0~128 | 位置标签X坐标 |
| pos_label_y | uint8_t | 0~128 | 位置标签Y坐标 |
| heartbeat | uint8_t | 0或1 | 心跳包 |
| img1_file | string | 长度<20 | 图1显示图片文件名 |
| img2_file | string | 长度<20 | 图2显示图片文件名 |
| **role_name** | string | 长度<20 | **角色名称** ✓ 新增 |
| **role_type** | uint8_t | 0~255 | **角色类型** ✓ 新增 |
| **role_action** | uint8_t | 0~255 | **行动类型** ✓ 新增 |
| Comp | uint16_t | 0~360 | **指南针方向（只读）** ✓ 新增 |
| Comp_offset | uint16_t | 0~360 | 指南针偏移 |

所有修改已完成，代码通过编译检查，无错误和警告。