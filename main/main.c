#include <sys/cdefs.h>
/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_sleep.h"
#include "sys/time.h"
#include "nvs_flash.h"
#include "driver/temperature_sensor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "lv_port_fs.h"


//bluetooth
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_gattc_api.h"
#include "esp_gatt_common_api.h" // Include for esp_ble_gatt_set_local_mtu
#include "esp_mac.h" // 标准MAC API
#include "esp_bt_device.h"

//hid
#include "hidd_le_prf_int.h"
#include "esp_hidd_prf_api.h"
#include "hid_dev.h"
#include "esp_hidd_api.h"

// 图片传输协议相关定义
#define CMD_INIT_FRAME          0xD1  // 初始化帧
#define CMD_DATA_FRAME          0xD2  // 数据帧
#define CMD_DELETE_FRAME        0xD4  // 删除文件帧
#define CMD_WRITE_CARD_FRAME    0xB1  // 写卡帧
// 系统命令定义（多字节命令）
#define CMD_SYS_PREFIX          0xEE  // 系统命令前缀
#define CMD_FORMAT_LEN           7     // 格式化命令长度
#define CMD_DIR_LEN             4     // 列出目录命令长度
#define CMD_RESET_LEN           6     // 重启命令长度
#define CMD_NFC_LEN             4     // NFC触发命令长度
// 系统参数命令定义
#define CMD_SET_PARAM          0xA1  // 设置系统参数
#define CMD_GET_PARAM          0xA2  // 获取系统参数
#define MAX_FILENAME_LEN        20
#define MAX_FILE_SIZE           1024 * 1024  // 最大1MB

// 回传协议应用ID定义（从0x01开始）
#define APP_ID_STATUS           0x01  // 状态信息（MAC地址、运行时间等）
#define APP_ID_DELETE           0xD4  // 删除文件响应
#define APP_ID_SYSTEM           0xEE  // 系统命令响应（格式化、列出目录、重启等）
#define APP_ID_SYS_PARAM        0xA0  // 系统参数响应
#define APP_ID_WRITE_CARD       0xB1  // 写卡响应

// 系统参数键定义
#define NVS_NAMESPACE           "SYS"   // 系统参数命名空间
#define NVS_KEY_COMP_OFFSET    "comp_offset"       // 指南针偏移
#define NVS_KEY_RUN_INTERVAL    "run_interval"     // 运行间隙
#define NVS_KEY_BACKLIGHT       "backlight"         // 背光开关
#define NVS_KEY_BG_MODE         "bg_mode"           // 底图模式
#define NVS_KEY_SHOW_MAC        "show_mac"          // 显示mac
#define NVS_KEY_POS_LABEL_EN    "pos_label_en"      // 位置标签开关
#define NVS_KEY_POS_LABEL_X    "pos_label_x"       // 位置标签x
#define NVS_KEY_POS_LABEL_Y    "pos_label_y"       // 位置标签y
#define NVS_KEY_HEARTBEAT      "heartbeat"         // 心跳包开关
#define NVS_KEY_IMG1_FILE      "img1_file"        // 图1文件名
#define NVS_KEY_IMG2_FILE      "img2_file"        // 图2文件名
#define NVS_KEY_ROLE_NAME      "role_name"         // 角色名称
#define NVS_KEY_ROLE_TYPE      "role_type"         // 角色类型
#define NVS_KEY_ROLE_ACTION    "role_action"       // 行动类型

#define BATTERY_REPORT_ID 0x02
#define BATTERY_REPORT_SIZE 1
#define HIDD_DEVICE_NAME      "VSchess-"


#define I2C0_MASTER_PORT               I2C_NUM_0
#define I2C0_MASTER_SDA_IO             GPIO_NUM_9
#define I2C0_MASTER_SCL_IO             GPIO_NUM_8 
#define NPD_EN_GPIO                    GPIO_NUM_10
#define LED_BG_GPIO                    GPIO_NUM_6
#define BATTERY_ADC                    GPIO_NUM_3

#define GPIO_INTERRUPT_PIN             GPIO_NUM_21

// 协议状态枚举
typedef enum {
    PROTOCOL_STATE_IDLE = 0,        // 空闲状态
    PROTOCOL_STATE_RECEIVING_INIT,  // 接收初始化帧
    PROTOCOL_STATE_RECEIVING_DATA   // 接收数据帧
} protocol_state_t;

// 协议数据结构
typedef struct {
    protocol_state_t state;
    uint8_t filename[MAX_FILENAME_LEN];
    uint32_t file_size;
    uint32_t received_size;
    lv_fs_file_t file_handle;       // 文件句柄
    bool file_open;                // 文件是否已打开
    uint32_t max_packet_num;        // 最大接收包号（用于去重）
    bool transfer_error;            // 传输错误标志
} image_transfer_protocol_t;

// 图片传输协议全局变量
static image_transfer_protocol_t g_img_protocol = {
    .state = PROTOCOL_STATE_IDLE,
    .file_size = 0,
    .received_size = 0,
    .file_open = false,
    .max_packet_num = 0,
    .transfer_error = false
};

// 写卡数据全局变量（16字节）
static uint8_t g_write_card_data[16] = {0};

// 指南针响应标记位（非阻塞模式）
static bool g_compending_response = false;  // 是否有待发送的指南针响应

// 系统参数结构
typedef struct {
    uint16_t comp_offset;          // 指南针偏移：0~360
    uint16_t run_interval;        // 运行间隙（秒），范围1~600
    uint8_t backlight_enable;     // 背光开关：0=关闭, 1=开启
    uint8_t bg_image_mode;       // 底图模式：1=1张128*128, 0=2张128*64
    uint8_t show_mac;            // 显示mac：0=关闭, 1=开启
    uint8_t pos_label_enable;     // 位置标签：0=关闭, 1=开启
    uint8_t pos_label_x;         // 位置标签x：0~128
    uint8_t pos_label_y;         // 位置标签y：0~128
    uint8_t heartbeat_enable;      // 心跳包：0=关闭, 1=开启
    char img1_file[20];         // 图1显示图片文件名（长度<20
    char img2_file[20];         // 图2显示图片文件名（长度<20）
    char role_name[20];         // 角色名称（长度<20）
    uint8_t role_type;          // 角色类型：0~255
    uint8_t role_action;        // 行动类型：0~255
} system_params_t;

// 系统参数全局变量（默认值）
static system_params_t g_sys_params = {
    .comp_offset = 0,              // 默认0度偏移
    .run_interval = 1000,           // 默认1秒
    .backlight_enable = 1,        // 默认开启
    .bg_image_mode = 1,           // 默认1张128*128
    .show_mac = 1,              // 默认显示
    .pos_label_enable = 1,     // 默认1
    .pos_label_x = 36,            // 默认
    .pos_label_y = 100,           // 默认
    .heartbeat_enable = 1,          // 默认开启心跳包
    .img1_file = "t-3.jpg",      // 默认图1文件名
    .img2_file = "b-3.jpg",       // 默认图2文件名
    .role_name = "default",        // 默认角色名称
    .role_type = 0,              // 默认角色类型
    .role_action = 0             // 默认行动类型
};



// Checksum16 计算函数 (大端序)
static uint16_t checksum16(const uint8_t *data, uint16_t len) {
    uint16_t sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

// 大端序转uint32_t
static uint32_t be_to_u32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}
// 大端序转uint16_t
static uint16_t be_to_u16(const uint8_t *data) {
    return ((uint16_t)data[0] << 8) | (uint16_t)data[1];
}
// 数据处理函数
static void process_protocol_data(const uint8_t *data, uint16_t length);

// 协议帧处理函数
static esp_err_t handle_init_frame(const uint8_t *data, uint16_t length);
static esp_err_t handle_data_frame(const uint8_t *data, uint16_t length);
static esp_err_t handle_delete_frame(const uint8_t *data, uint16_t length);
static esp_err_t handle_set_param_frame(const uint8_t *data, uint16_t length);
static esp_err_t handle_get_param_frame(const uint8_t *data, uint16_t length);
static esp_err_t handle_write_card_frame(const uint8_t *data, uint16_t length);

// 系统参数管理函数
static void load_system_params_from_nvs(void);
static void save_system_params_to_nvs(void);

// 触发测量任务函数
static esp_err_t start_measure_task(void);
static esp_err_t start_write_card_task(void);

// 回传协议函数
static esp_err_t send_upload_response(uint8_t app_id, const uint8_t *payload, uint16_t payload_len);
static esp_err_t send_upload_response_fragmented(uint8_t app_id, const uint8_t *payload, uint16_t payload_len);

void i2c0_mmc56x3_task( void *pvParameters );
void key_task( void *pvParameters );
void writecard_task( void *pvParameters );

// NFC数据处理函数
static void process_nfc_data(unsigned char  *card_uid, unsigned char  *card_data);

// HID报告配置

char blerename[32];
uint8_t macAddr[6]; //蓝牙地址
static esp_bd_addr_t remote_bda = {0};  // 存储远端设备地址

static uint16_t hid_conn_id = 0;
static bool sec_conn = false;
static bool send_volum_up = false;
#define CHAR_DECLARATION_SIZE (sizeof(uint8_t))

static esp_ble_adv_data_t hidd_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x20, // slave connection min interval, Time = min_interval * 1.25 msec
    .max_interval = 0x0320, // slave connection max interval, Time = max_interval * 1.25 msec
    .appearance = 0x03c1,   // 0x41, 鼠标    // 0x03c0,   HID Generic,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,//sizeof(hidd_service_uuid128),
    .p_service_uuid = NULL,//hidd_service_uuid128,
    .flag = 0x6,
};

static esp_ble_adv_params_t hidd_adv_params = {
    .adv_int_min = 0x20, //0.625,140=200ms
    .adv_int_max = 0x320, //640=1s,320=0.5s
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    //.peer_addr            =
    //.peer_addr_type       =
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

//nfc
#include "mmc56x3.h"
#include "SI523_App.h"
int Compass_Heading = 0;
int True_Heading = 0;

//ws2812
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#define RMT_LED_STRIP_GPIO_NUM      6
#define LEDS_COUNT                  1
uint8_t led_strip_pixels[LEDS_COUNT] = {0};
uint8_t led_brightness= 100;


//adc
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static adc_oneshot_unit_handle_t adc1_handle;

struct capacity {
  int capacity;
  int minx;
  int maxx;
};
#define battery_capacity_tables_size 12
static struct capacity battery_capacity_tables[] = {
  /*  capacity, minx, maxx  */
  {0, 3306, 3426},//0red
  {1, 3427, 3638},//0
  {10, 3639, 3697}, //0
  {20, 3698, 3729}, //1
  {30, 3730, 3748}, //1
  {40, 3749, 3776}, //2
  {50, 3777, 3827}, //2
  {60, 3828, 3895}, //2
  {70, 3896, 3954}, //3
  {80, 3955, 4050}, //3
  {90, 4051, 4119}, //3
  {100, 4120, 4240}, //3
};

#define NO_OF_SAMPLES 64  // 多次采样取平均值
#define ADC_ATTEN_DB 11    // 11dB衰减，最大输入电压约3.9V，配合1/2分压可测最大7.8V
#define BATTERY_DIVIDER_RATIO 2.0  // 电池分压比，1/2分压电路
static void adc_init(void)
{
    // ADC init BATTERY_ADC gpio3
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    adc_oneshot_chan_cfg_t configa = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,  // 使用默认位宽（通常为12位）
        .atten = ADC_ATTEN_DB_12,  // 11dB衰减，最大输入约3.9V
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, BATTERY_ADC, &configa));
}

static uint32_t read_battery_voltage(void)
{
    int raw = 0;
    uint32_t adc_reading = 0;
    esp_err_t ret = ESP_OK;

    // 多次采样取平均值，提高准确性
    for (int i = 0; i < NO_OF_SAMPLES; i++)
    {
        ret = adc_oneshot_read(adc1_handle, BATTERY_ADC, &raw);
        if (ret == ESP_OK)
        {
            adc_reading += raw;
        }
        else
        {
            ESP_LOGE("ADC", "ADC read failed: %s", esp_err_to_name(ret));
            i = 0;
            adc_reading = 0;
        }
    }
    adc_reading /= NO_OF_SAMPLES;

    // ESP32C3 ADC转换公式（11dB衰减）
    // 实际电压(mV) = ADC原始值 * 分压系数 * 1000 / (2^12 - 1)
    // 分压系数 = 2.0（因为1/2分压电路）
    // 2^12 = 4096
    uint32_t adc_voltage_mv = (uint32_t)(adc_reading * 2520.0 / 3495 * BATTERY_DIVIDER_RATIO);

    ESP_LOGD("ADC", "ADC raw=%lu, battery_voltage=%lumV", adc_reading, adc_voltage_mv);

    return adc_voltage_mv;
}
static int battery_calculate_capacity(int battery_voltage)
{
    int mid = 0;
    int high = battery_capacity_tables_size - 1;
    int low = 0;
    int offset = 0;
    int cap = 0;
    int v0 = 0;
    int c0 = 0;
    int v1 = 0;
    int c1 = 0;
    int deviation = 0;

    while (high >= low)
    {
        mid = (high + low) / 2;
        // offset = (compensated ? tables[mid].offset : 0);
        if ((battery_voltage + offset) < battery_capacity_tables[mid].minx)
            high = mid - 1;
        else if ((battery_voltage + offset) > battery_capacity_tables[mid].maxx)
            low = mid + 1;
        else
            break;
    }

    cap = battery_capacity_tables[mid].capacity;
    if ((0 < mid) && (mid < battery_capacity_tables_size - 1))
    {
        v0 = battery_capacity_tables[mid].minx;
        c0 = battery_capacity_tables[mid].capacity;
        v1 = battery_capacity_tables[mid + 1].minx;
        c1 = battery_capacity_tables[mid + 1].capacity;

        deviation = ((c1 - c0) * (battery_voltage - v0) * 10) / (v1 - v0);
        cap = c0 + (deviation / 10 + ((deviation % 10) >= 5 ? 1 : 0));
        if (cap < c0)
            cap = c0;
        else if (cap > c1)
            cap = c1;
    }

    return cap;
}

const char* hidden_msg = "专业软硬件开发,方案可出何必破解,价格合理,合作愉快,期待共赢, yuri_su@163.com , +8613580387577  ";
const char* hidden_msg2 = "Professional hardware and software solutions available for purchase.[yuri_su@163.com,+8613580387577] No need to break in — let's save the effort and share the rewards. Reasonable prices, pleasant cooperation, and mutual success await.";


// LVGL 
#if LV_USE_DEMO_WIDGETS
    #include "demos/lv_demos.h"
#endif

#define GPIO_INTERRUPT_TAG             "GPIO_ISR"


#define TSK_MINIMAL_STACK_SIZE         (1024)
#define MMC_TASK_NAME                 "mmc_task"
#define MMC_TASK_SAMPLING_RATE        (10000) 
#define MMC_TASK_STACK_SIZE           (TSK_MINIMAL_STACK_SIZE * 2)
#define MMC_TASK_PRIORITY             (tskIDLE_PRIORITY + 2)
#define MMC_TAG                       "MMC"

#define KEY_TASK_NAME                 "key_task"
#define KEY_TASK_STACK_SIZE           (TSK_MINIMAL_STACK_SIZE * 2)
#define KEY_TASK_PRIORITY             (tskIDLE_PRIORITY + 3)
#define KEY_TAG                       "KEY"

void NPD_EN(int state);
void BG_EN(int state);

#define LOW_LEVEL 0
#define HIGH_LEVEL 1

static volatile uint8_t g_task_running = 0;  // 任务运行标志，防止重复创建

static lv_obj_t *pos_label;
static lv_obj_t *bg_img;
static lv_obj_t *bom_img;
static lv_obj_t *gif_obj;

// void lv_tick_task(void *arg) {
//   (void) arg;
//   lv_tick_inc(100);
// }

_Noreturn void PrintChipInfo(void *params) {
  (void) params;
  /* Print chip information */
  esp_chip_info_t chip_info;
  uint32_t flash_size;
  esp_chip_info(&chip_info);
  printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
         CONFIG_IDF_TARGET,
         chip_info.cores,
         (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
         (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
         (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
         (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

  unsigned major_rev = chip_info.revision / 100;
  unsigned minor_rev = chip_info.revision % 100;
  printf("silicon revision v%d.%d, ", major_rev, minor_rev);
  if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
    printf("Get flash size failed");
  }

  printf("%"
         PRIu32
         "MB %s flash\n", flash_size / (uint32_t) (1024 * 1024),
         (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

  printf("Minimum free heap size: %"
         PRIu32
         " bytes\n", esp_get_minimum_free_heap_size());

  while (true) {
    // 任务禁止主动返回
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}


#define I2C0_MASTER_CONFIG_DEFAULT {                                \
        .clk_source                     = I2C_CLK_SRC_DEFAULT,      \
        .i2c_port                       = I2C0_MASTER_PORT,         \
        .scl_io_num                     = I2C0_MASTER_SCL_IO,       \
        .sda_io_num                     = I2C0_MASTER_SDA_IO,       \
        .glitch_ignore_cnt              = 7,                        \
        .flags.enable_internal_pullup   = true, }


// initialize master i2c 0 bus configuration
i2c_master_bus_config_t  i2c0_bus_cfg = I2C0_MASTER_CONFIG_DEFAULT;
i2c_master_bus_handle_t  i2c0_bus_hdl = NULL;

void IIC_init(void) {
    /* instantiate i2c master bus 0 */
    ESP_ERROR_CHECK( i2c_new_master_bus(&i2c0_bus_cfg, &i2c0_bus_hdl) );

    /* check i2c master bus handle instance */
    if (i2c0_bus_hdl == NULL) {
        ESP_LOGE("IIC", "i2c master bus handle init failed");
        assert(i2c0_bus_hdl);
    }
}



// GPIO interrupt handler
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    //BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint32_t gpio_num = (uint32_t) arg;

    ESP_EARLY_LOGI(GPIO_INTERRUPT_TAG, "GPIO %ld interrupt triggered!", gpio_num);

    // 检查任务是否正在运行，防止重复创建
    if (g_task_running == 0) {
        g_task_running = 100;
        // 直接创建一次性任务来处理按键事件
        xTaskCreatePinnedToCore(key_task, KEY_TASK_NAME, KEY_TASK_STACK_SIZE, NULL, KEY_TASK_PRIORITY, NULL, 0);
    } 

    // 清除中断状态
    gpio_intr_disable(gpio_num);
    //ets_delay_us(10); // Simple debounce
    gpio_intr_enable(gpio_num);
}


// NPD enable function
void NPD_EN(int state)
{
    gpio_set_level(NPD_EN_GPIO, state ? HIGH_LEVEL : LOW_LEVEL);
}
void BG_EN(int state)
{
    gpio_set_level(LED_BG_GPIO, state ? HIGH_LEVEL : LOW_LEVEL);
}
// Initialize NPD_EN GPIO10 as output with low level
static void npd_gpio_init(void)
{
    gpio_reset_pin(NPD_EN_GPIO);
    gpio_set_direction(NPD_EN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(NPD_EN_GPIO, LOW_LEVEL);
    
    gpio_reset_pin(LED_BG_GPIO);
    gpio_set_direction(LED_BG_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_BG_GPIO, LOW_LEVEL);
    
}

// Initialize GPIO interrupt
void gpio_interrupt_init(void)
{
    // Configure GPIO pin
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_POSEDGE;      // Falling edge interrupt
    io_conf.pin_bit_mask = (1ULL << GPIO_INTERRUPT_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;    // Enable pull-up resistor
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io_conf);

    // Install GPIO ISR service
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);

    // Hook ISR handler for specific GPIO pin
    gpio_isr_handler_add(GPIO_INTERRUPT_PIN, gpio_isr_handler, (void*) GPIO_INTERRUPT_PIN);

    ESP_LOGI(GPIO_INTERRUPT_TAG, "GPIO %d falling edge interrupt initialized", GPIO_INTERRUPT_PIN);
}

// 重置协议状态
static void reset_protocol_state(void) {
    if (g_img_protocol.file_open) {
        lv_fs_close(&g_img_protocol.file_handle);
        g_img_protocol.file_open = false;
        ESP_LOGI("CMDp", "File closed");
    }
    memset(&g_img_protocol, 0, sizeof(image_transfer_protocol_t));
    g_img_protocol.state = PROTOCOL_STATE_IDLE;
    g_img_protocol.max_packet_num = 0;
    ESP_LOGI("CMDp", "Protocol state reset");
}

// 从NVS加载系统参数
static void load_system_params_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    
    if (err == ESP_OK) {
        int val;

        // 指南针偏移
        if (nvs_get_i32(nvs_handle, NVS_KEY_COMP_OFFSET, &val) == ESP_OK) {
            if (val >= 0 && val <= 360) {
                g_sys_params.comp_offset = (uint16_t)val;
            } else {
                ESP_LOGW("SYS", "Invalid comp_offset from NVS: %d, using default", val);
            }
        }

        // 运行间隙
        if (nvs_get_i32(nvs_handle, NVS_KEY_RUN_INTERVAL, &val) == ESP_OK) {
            if (val >= 10 && val <= 60000) {
                g_sys_params.run_interval = (uint16_t)val;
            } else {
                ESP_LOGW("SYS", "Invalid run_interval from NVS: %d, using default", val);
            }
        }
        
        // 背光开关
        if (nvs_get_i32(nvs_handle, NVS_KEY_BACKLIGHT, &val) == ESP_OK) {
            g_sys_params.backlight_enable = (val != 0) ? 1 : 0;
        }
        
        // 底图模式
        if (nvs_get_i32(nvs_handle, NVS_KEY_BG_MODE, &val) == ESP_OK) {
            g_sys_params.bg_image_mode = (val == 1 || val == 0) ? (uint8_t)val : 1;
        }
        
        // 显示MAC
        if (nvs_get_i32(nvs_handle, NVS_KEY_SHOW_MAC, &val) == ESP_OK) {
            g_sys_params.show_mac = (val != 0) ? 1 : 0;
        }
        
        // 位置标签开关
        if (nvs_get_i32(nvs_handle, NVS_KEY_POS_LABEL_EN, &val) == ESP_OK) {
            g_sys_params.pos_label_enable = (val != 0) ? 1 : 0;
        }
        
        // 位置标签X
        if (nvs_get_i32(nvs_handle, NVS_KEY_POS_LABEL_X, &val) == ESP_OK) {
            if (val >= 0 && val <= 128) {
                g_sys_params.pos_label_x = (uint8_t)val;
            }
        }
        
        // 位置标签Y
        if (nvs_get_i32(nvs_handle, NVS_KEY_POS_LABEL_Y, &val) == ESP_OK) {
            if (val >= 0 && val <= 128) {
                g_sys_params.pos_label_y = (uint8_t)val;
            }
        }

        // 心跳包开关
        if (nvs_get_i32(nvs_handle, NVS_KEY_HEARTBEAT, &val) == ESP_OK) {
            g_sys_params.heartbeat_enable = (val != 0) ? 1 : 0;
        }

        // 图1文件名
        size_t required_size = 20;
        if (nvs_get_str(nvs_handle, NVS_KEY_IMG1_FILE, g_sys_params.img1_file, &required_size) != ESP_OK) {
            ESP_LOGW("SYS", "No img1_file in NVS, using default");
        } else {
            if (required_size >= 20) {
                ESP_LOGW("SYS", "img1_file too long, truncating");
                g_sys_params.img1_file[19] = '\0';
            }
        }

        // 图2文件名
        required_size = 20;
        if (nvs_get_str(nvs_handle, NVS_KEY_IMG2_FILE, g_sys_params.img2_file, &required_size) != ESP_OK) {
            ESP_LOGW("SYS", "No img2_file in NVS, using default");
        } else {
            if (required_size >= 20) {
                ESP_LOGW("SYS", "img2_file too long, truncating");
                g_sys_params.img2_file[19] = '\0';
            }
        }

        // 角色名称
        required_size = 20;
        if (nvs_get_str(nvs_handle, NVS_KEY_ROLE_NAME, g_sys_params.role_name, &required_size) != ESP_OK) {
            ESP_LOGW("SYS", "No role_name in NVS, using default");
        } else {
            if (required_size >= 20) {
                ESP_LOGW("SYS", "role_name too long, truncating");
                g_sys_params.role_name[19] = '\0';
            }
        }

        // 角色类型
        if (nvs_get_i32(nvs_handle, NVS_KEY_ROLE_TYPE, &val) == ESP_OK) {
            if (val >= 0 && val <= 255) {
                g_sys_params.role_type = (uint8_t)val;
            } else {
                ESP_LOGW("SYS", "Invalid role_type from NVS: %d, using default", val);
            }
        }

        // 行动类型
        if (nvs_get_i32(nvs_handle, NVS_KEY_ROLE_ACTION, &val) == ESP_OK) {
            if (val >= 0 && val <= 255) {
                g_sys_params.role_action = (uint8_t)val;
            } else {
                ESP_LOGW("SYS", "Invalid role_action from NVS: %d, using default", val);
            }
        }

        nvs_close(nvs_handle);
        ESP_LOGI("SYS", "System parameters loaded from NVS");
    } else {
        ESP_LOGW("SYS", "Failed to open NVS namespace, using defaults");
    }
}

// 保存系统参数到NVS
static void save_system_params_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);

    if (err == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_KEY_COMP_OFFSET, g_sys_params.comp_offset);
        nvs_set_i32(nvs_handle, NVS_KEY_RUN_INTERVAL, g_sys_params.run_interval);
        nvs_set_i32(nvs_handle, NVS_KEY_BACKLIGHT, g_sys_params.backlight_enable);
        nvs_set_i32(nvs_handle, NVS_KEY_BG_MODE, g_sys_params.bg_image_mode);
        nvs_set_i32(nvs_handle, NVS_KEY_SHOW_MAC, g_sys_params.show_mac);
        nvs_set_i32(nvs_handle, NVS_KEY_POS_LABEL_EN, g_sys_params.pos_label_enable);
        nvs_set_i32(nvs_handle, NVS_KEY_POS_LABEL_X, g_sys_params.pos_label_x);
        nvs_set_i32(nvs_handle, NVS_KEY_POS_LABEL_Y, g_sys_params.pos_label_y);
        nvs_set_i32(nvs_handle, NVS_KEY_HEARTBEAT, g_sys_params.heartbeat_enable);
        nvs_set_str(nvs_handle, NVS_KEY_IMG1_FILE, g_sys_params.img1_file);
        nvs_set_str(nvs_handle, NVS_KEY_IMG2_FILE, g_sys_params.img2_file);
        nvs_set_str(nvs_handle, NVS_KEY_ROLE_NAME, g_sys_params.role_name);
        nvs_set_i32(nvs_handle, NVS_KEY_ROLE_TYPE, g_sys_params.role_type);
        nvs_set_i32(nvs_handle, NVS_KEY_ROLE_ACTION, g_sys_params.role_action);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI("SYS", "System parameters saved to NVS");
    } else {
        ESP_LOGE("SYS", "Failed to open NVS namespace for writing");
    }
}

// 触发测量任务（等同于按键中断中的任务）
static esp_err_t start_measure_task(void)
{
    // 检查任务是否正在运行，防止重复创建
    if (g_task_running) {
        ESP_LOGW("CMDp", "Measure task already running");
        const char *warning_msg = "Error: Task already running";
        send_upload_response(APP_ID_SYSTEM, (uint8_t*)warning_msg, strlen(warning_msg));
        return ESP_ERR_INVALID_STATE;
    }

    // 创建一次性任务来处理按键事件（等同于按键中断）
    g_task_running = 100;
    BaseType_t ret = xTaskCreatePinnedToCore(key_task, KEY_TASK_NAME, KEY_TASK_STACK_SIZE, NULL, KEY_TASK_PRIORITY, NULL, 0);

    if (ret == pdPASS) {
        const char *success_msg = "Key task started";
        send_upload_response(APP_ID_SYSTEM, (uint8_t*)success_msg, strlen(success_msg));
        ESP_LOGI("CMDp", "Key task created successfully");
        return ESP_OK;
    } else {
        g_task_running = 0;  // 创建失败，重置标志
        const char *error_msg = "Error: Failed to create task";
        send_upload_response(APP_ID_SYSTEM, (uint8_t*)error_msg, strlen(error_msg));
        ESP_LOGE("CMDp", "Failed to create key task");
        return ESP_FAIL;
    }
}

// 处理初始化帧（0xD1命令）
// 帧结构：命令(1) | checksum(2) | 文件大小(4) | 长度(1) | 文件名(N)
static esp_err_t handle_init_frame(const uint8_t *data, uint16_t length) {
    ESP_LOGI("CMDp", "Handling init frame, length=%d", length);

    // 最小帧长度：命令(1) + checksum(2) + 文件大小(4) + 长度(1) + 文件名(1) = 9字节
    if (length < 9) {
        ESP_LOGE("CMDp", "Init frame too short: %d < 9", length);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t cmd = data[0];
    uint16_t received_checksum = be_to_u16(&data[1]);
    uint32_t file_size = be_to_u32(&data[3]);
    uint8_t filename_len = data[7];

    // 检查命令是否为0xD1
    if (cmd != CMD_INIT_FRAME) {
        ESP_LOGE("CMDp", "Invalid init frame command: 0x%02X", cmd);
        return ESP_ERR_INVALID_ARG;
    }

    // 检查文件名长度是否合法
    if (filename_len == 0 || filename_len >= MAX_FILENAME_LEN) {
        ESP_LOGE("CMDp", "Invalid filename length: %d", filename_len);
        return ESP_ERR_INVALID_ARG;
    }

    // 检查总长度是否匹配
    uint16_t expected_length = 1 + 2 + 4 + 1 + filename_len; // 命令 + checksum + 文件大小 + 长度 + 文件名
    if (length != expected_length) {
        ESP_LOGE("CMDp", "Length mismatch: expected %d, got %d", expected_length, length);
        return ESP_ERR_INVALID_SIZE;
    }

    // 验证checksum（校验数据：文件大小+长度+文件名）
    uint16_t calc_checksum = checksum16(&data[3], length - 3); // 从文件大小字段开始校验
    if (received_checksum != calc_checksum) {
        ESP_LOGE("CMDp", "Checksum error: received=0x%04X, calculated=0x%04X",
                 received_checksum, calc_checksum);
        g_img_protocol.transfer_error = true;
        return ESP_ERR_INVALID_CRC;
    }

    // 检查文件大小是否合理
    if (file_size == 0 || file_size > MAX_FILE_SIZE) {
        ESP_LOGE("CMDp", "Invalid file size: %lu", file_size);
        return ESP_ERR_INVALID_ARG;
    }

    // 复制文件名
    memset(g_img_protocol.filename, 0, MAX_FILENAME_LEN);
    memcpy(g_img_protocol.filename, &data[8], filename_len);
    g_img_protocol.filename[filename_len] = '\0';

    ESP_LOGI("CMDp", "Init frame valid - File: %s, Size: %lu bytes",
             g_img_protocol.filename, file_size);

    // 重置之前的传输状态
    if (g_img_protocol.file_open) {
        lv_fs_close(&g_img_protocol.file_handle);
        g_img_protocol.file_open = false;
    }

    // 构建完整文件路径
    char filepath[MAX_FILENAME_LEN + 8];
    snprintf(filepath, sizeof(filepath), "A:/%s", g_img_protocol.filename);

    // 打开文件准备写入
    if (lv_fs_open(&g_img_protocol.file_handle, filepath, LV_FS_MODE_WR) != LV_FS_RES_OK) {
        ESP_LOGE("CMDp", "Failed to open file: %s", filepath);
        reset_protocol_state();
        g_img_protocol.transfer_error = true;
        return ESP_ERR_INVALID_STATE;
    }

    // 初始化传输参数
    g_img_protocol.file_size = file_size;
    g_img_protocol.received_size = 0;
    g_img_protocol.max_packet_num = 0;
    g_img_protocol.file_open = true;
    g_img_protocol.transfer_error = false;
    g_img_protocol.state = PROTOCOL_STATE_RECEIVING_DATA;

    ESP_LOGI("CMDp", "File opened for writing - Path: %s, Total size: %lu bytes", filepath, file_size);

    return ESP_OK;
}

// 处理删除文件帧（0xD4命令）
// 帧结构：命令(1) | checksum(2) | 长度(1) | 文件名(N)
static esp_err_t handle_delete_frame(const uint8_t *data, uint16_t length) {
    ESP_LOGI("CMDp", "Handling delete frame, length=%d", length);

    // 最小帧长度：命令(1) + checksum(2) + 长度(1) + 文件名(1) = 5字节
    if (length < 5) {
        ESP_LOGE("CMDp", "Delete frame too short: %d < 5", length);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t cmd = data[0];
    uint16_t received_checksum = be_to_u16(&data[1]);
    uint8_t filename_len = data[3];

    // 检查命令是否为0xD4
    if (cmd != CMD_DELETE_FRAME) {
        ESP_LOGE("CMDp", "Invalid delete frame command: 0x%02X", cmd);
        return ESP_ERR_INVALID_ARG;
    }

    // 检查文件名长度是否合法
    if (filename_len == 0 || filename_len >= MAX_FILENAME_LEN) {
        ESP_LOGE("CMDp", "Invalid filename length: %d", filename_len);
        return ESP_ERR_INVALID_ARG;
    }

    // 检查总长度是否匹配
    uint16_t expected_length = 1 + 2 + 1 + filename_len; // 命令 + checksum + 长度 + 文件名
    if (length != expected_length) {
        ESP_LOGE("CMDp", "Length mismatch: expected %d, got %d", expected_length, length);
        return ESP_ERR_INVALID_SIZE;
    }

    // 验证checksum（校验数据：长度+文件名）
    uint16_t calc_checksum = checksum16(&data[3], length - 3); // 从长度字段开始校验
    if (received_checksum != calc_checksum) {
        ESP_LOGE("CMDp", "Checksum error: received=0x%04X, calculated=0x%04X",
                 received_checksum, calc_checksum);
        return ESP_ERR_INVALID_CRC;
    }

    // 复制文件名
    char filename[MAX_FILENAME_LEN];
    memset(filename, 0, MAX_FILENAME_LEN);
    memcpy(filename, &data[4], filename_len);
    filename[filename_len] = '\0';

    ESP_LOGI("CMDp", "Delete frame valid - File: %s", filename);

    // 构建完整文件路径
    char filepath[MAX_FILENAME_LEN + 8];
    snprintf(filepath, sizeof(filepath), "A:/%s", filename);

    // 删除文件
    lv_fs_res_t ret = lv_port_fs_remove(filepath);

    // 发送响应
    char response[64];
    if (ret == LV_FS_RES_OK) {
        snprintf(response, sizeof(response), "Delete: OK - %s", filename);
        ESP_LOGI("CMDp", "File deleted successfully: %s", filename);
    } else {
        snprintf(response, sizeof(response), "Delete: Failed - %s", filename);
        ESP_LOGE("CMDp", "Failed to delete file: %s", filename);
    }
        //nus_uart_send_data(hid_conn_id, (uint8_t*)response, strlen(response));
        send_upload_response(APP_ID_DELETE, (uint8_t*)response, strlen(response));
    return (ret == LV_FS_RES_OK) ? ESP_OK : ESP_FAIL;
}

// 处理数据帧（0xD2命令）
// 帧结构：命令(1) | checksum(2) | 包号(4) | 长度(1) | 数据负载(N)
static esp_err_t handle_data_frame(const uint8_t *data, uint16_t length) {
    ESP_LOGI("CMDp", "Handling data frame, length=%d", length);

    // 如果之前有错误，拒绝接收数据帧
    if (g_img_protocol.transfer_error) {
        ESP_LOGW("CMDp", "Transfer error flag set, ignoring data frame");
        return ESP_ERR_INVALID_STATE;
    }

    // 检查是否处于接收数据状态
    if (g_img_protocol.state != PROTOCOL_STATE_RECEIVING_DATA) {
        ESP_LOGW("CMDp", "Not in receiving data state, ignoring frame");
        return ESP_ERR_INVALID_STATE;
    }

    // 最小帧长度：命令(1) + checksum(2) + 包号(4) + 长度(1) = 8字节
    if (length < 8) {
        ESP_LOGE("CMDp", "Data frame too short: %d < 8", length);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t cmd = data[0];
    uint16_t received_checksum = be_to_u16(&data[1]);
    uint32_t packet_num = be_to_u32(&data[3]);
    uint8_t payload_len = data[7];

    // 检查命令是否为0xD2
    if (cmd != CMD_DATA_FRAME) {
        ESP_LOGE("CMDp", "Invalid data frame command: 0x%02X", cmd);
        return ESP_ERR_INVALID_ARG;
    }

    // 检查总长度是否匹配
    uint16_t expected_length = 1 + 2 + 4 + 1 + payload_len; // 命令 + checksum + 包号 + 长度 + 负载
    if (length != expected_length) {
        ESP_LOGE("CMDp", "Length mismatch: expected %d, got %d", expected_length, length);
        return ESP_ERR_INVALID_SIZE;
    }

    // 验证checksum（校验数据：包号+长度+负载）
    uint16_t calc_checksum = checksum16(&data[3], length - 3); // 从包号字段开始校验
    if (received_checksum != calc_checksum) {
        ESP_LOGE("CMDp", "Checksum error: received=0x%04X, calculated=0x%04X",
                 received_checksum, calc_checksum);
        g_img_protocol.transfer_error = true;
        reset_protocol_state();
        return ESP_ERR_INVALID_CRC;
    }

    // 检查包号是否重复（去重）
    if (packet_num <= g_img_protocol.max_packet_num) {
        ESP_LOGW("CMDp", "Duplicate packet: %lu (max=%lu), dropping",
                 packet_num, g_img_protocol.max_packet_num);
        return ESP_OK; // 重复包不算错误，只是丢弃
    }

    // 检查是否有跳包
    if (packet_num != g_img_protocol.max_packet_num + 1 && g_img_protocol.max_packet_num != 0) {
        ESP_LOGW("CMDp", "Packet gap detected: expected %lu, got %lu",
                 g_img_protocol.max_packet_num + 1, packet_num);
        // 不报错，继续处理，可能是有意跳过某些包
    }

    // 检查接收数据是否会超出缓冲区
    if (g_img_protocol.received_size + payload_len > g_img_protocol.file_size) {
        ESP_LOGE("CMDp", "Data overflow: received %lu + payload %d > total %lu",
                 g_img_protocol.received_size, payload_len, g_img_protocol.file_size);
        g_img_protocol.transfer_error = true;
        reset_protocol_state();
        return ESP_ERR_INVALID_SIZE;
    }

    // 流式写入数据到文件
    if (payload_len > 0) {
        uint32_t written;
        lv_fs_res_t write_result = lv_fs_write(&g_img_protocol.file_handle, &data[8], payload_len, &written);

        if (write_result != LV_FS_RES_OK || written != payload_len) {
            ESP_LOGE("CMDp", "File write failed: expected %d bytes, written %lu bytes, result: %d",
                     payload_len, written, write_result);
            g_img_protocol.transfer_error = true;
            reset_protocol_state();
            return ESP_ERR_INVALID_STATE;
        }

        g_img_protocol.received_size += payload_len;
        g_img_protocol.max_packet_num = packet_num;

        ESP_LOGI("CMDp", "Data packet %lu received, payload %d bytes, total %lu/%lu bytes (%.1f%%)",
                 packet_num, payload_len, g_img_protocol.received_size, g_img_protocol.file_size,
                 (g_img_protocol.received_size * 100.0) / g_img_protocol.file_size);
    }

    // 检查是否接收完成
    if (g_img_protocol.received_size >= g_img_protocol.file_size) {
        ESP_LOGI("CMDp", "Transfer complete! File: %s, Total: %lu bytes",
                 g_img_protocol.filename, g_img_protocol.file_size);

        // 关闭文件
        if (g_img_protocol.file_open) {
            lv_fs_close(&g_img_protocol.file_handle);
            g_img_protocol.file_open = false;
            ESP_LOGI("CMDp", "File closed successfully");
        }

        // 重置状态
        g_img_protocol.state = PROTOCOL_STATE_IDLE;
    }

    return ESP_OK;
}

// 处理设置参数帧（0xA1命令）
// 帧结构：命令(1) | checksum(2) | 长度(1) | 参数(N)
static esp_err_t handle_set_param_frame(const uint8_t *data, uint16_t length) {
    ESP_LOGI("CMDp", "Handling set param frame, length=%d", length);
    
    // 最小帧长度：命令(1) + checksum(2) + 长度(1) = 4字节
    if (length < 4) {
        ESP_LOGE("CMDp", "Set param frame too short: %d < 4", length);
        return ESP_ERR_INVALID_SIZE;
    }
    
    uint8_t cmd = data[0];
    uint16_t received_checksum = be_to_u16(&data[1]);
    uint8_t param_len = data[3];
    
    // 检查命令是否为0xA1
    if (cmd != CMD_SET_PARAM) {
        ESP_LOGE("CMDp", "Invalid set param command: 0x%02X", cmd);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查总长度是否匹配
    uint16_t expected_length = 1 + 2 + 1 + param_len;
    if (length != expected_length) {
        ESP_LOGE("CMDp", "Length mismatch: expected %d, got %d", expected_length, length);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // 验证checksum（校验数据：长度+参数）
    uint16_t calc_checksum = checksum16(&data[3], length - 3);
    if (received_checksum != calc_checksum) {
        ESP_LOGE("CMDp", "Checksum error: received=0x%04X, calculated=0x%04X",
                 received_checksum, calc_checksum);
        return ESP_ERR_INVALID_CRC;
    }
    
    // 提取参数字符串
    char param_str[MAX_FILENAME_LEN];
    if (param_len >= MAX_FILENAME_LEN) {
        param_len = MAX_FILENAME_LEN - 1;
    }
    memcpy(param_str, &data[4], param_len);
    param_str[param_len] = '\0';
    
    ESP_LOGI("CMDp", "Set param string: %s", param_str);
    
    // 解析参数：key=value
    char *equal_pos = strchr(param_str, '=');
    if (equal_pos == NULL) {
        ESP_LOGE("CMDp", "Invalid param format, missing '='");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 分离key和value
    *equal_pos = '\0';  // 分割字符串
    char *key = param_str;
    char *value = equal_pos + 1;
    
    bool param_changed = false;

    // 根据key设置对应的参数
    if (strcmp(key, "comp_offset") == 0) {
        int val = atoi(value);
        if (val >= 0 && val <= 360) {
            g_sys_params.comp_offset = (uint16_t)val;
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid comp_offset value: %d", val);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "run_interval") == 0) {
        int val = atoi(value);
        if (val >= 10 && val <= 60000) {
            g_sys_params.run_interval = (uint16_t)val;
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid run_interval value: %d", val);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "backlight") == 0) {
        int val = atoi(value);
        if (val == 0 || val == 1) {
            g_sys_params.backlight_enable = (uint8_t)val;
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid backlight value: %d", val);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "bg_mode") == 0) {
        int val = atoi(value);
        if (val == 0 || val == 1) {
            g_sys_params.bg_image_mode = (uint8_t)val;
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid bg_mode value: %d", val);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "show_mac") == 0) {
        int val = atoi(value);
        if (val == 0 || val == 1) {
            g_sys_params.show_mac = (uint8_t)val;
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid show_mac value: %d", val);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "pos_label") == 0) {
        int val = atoi(value);
        if (val == 0 || val == 1) {
            g_sys_params.pos_label_enable = (uint8_t)val;
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid pos_label value: %d", val);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "pos_label_x") == 0) {
        int val = atoi(value);
        if (val >= 0 && val <= 128) {
            g_sys_params.pos_label_x = (uint8_t)val;
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid pos_label_x value: %d", val);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "pos_label_y") == 0) {
        int val = atoi(value);
        if (val >= 0 && val <= 128) {
            g_sys_params.pos_label_y = (uint8_t)val;
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid pos_label_y value: %d", val);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "heartbeat") == 0) {
        int val = atoi(value);
        if (val == 0 || val == 1) {
            g_sys_params.heartbeat_enable = (uint8_t)val;
            param_changed = true;
        } else if(val == 2) {
            g_sys_params.heartbeat_enable = (uint8_t)val;
        } else {
            ESP_LOGE("CMDp", "Invalid heartbeat value: %d", val);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "img1_file") == 0) {
        if (strlen(value) > 0 && strlen(value) < 20) {
            strncpy(g_sys_params.img1_file, value, 19);
            g_sys_params.img1_file[19] = '\0';
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid img1_file value: %s (length=%d)", value, (int)strlen(value));
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "img2_file") == 0) {
        if (strlen(value) > 0 && strlen(value) < 20) {
            strncpy(g_sys_params.img2_file, value, 19);
            g_sys_params.img2_file[19] = '\0';
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid img2_file value: %s (length=%d)", value, (int)strlen(value));
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "role_name") == 0) {
        if (strlen(value) > 0 && strlen(value) < 20) {
            strncpy(g_sys_params.role_name, value, 19);
            g_sys_params.role_name[19] = '\0';
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid role_name value: %s (length=%d)", value, (int)strlen(value));
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "role_type") == 0) {
        int val = atoi(value);
        if (val >= 0 && val <= 255) {
            g_sys_params.role_type = (uint8_t)val;
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid role_type value: %d", val);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(key, "role_action") == 0) {
        int val = atoi(value);
        if (val >= 0 && val <= 255) {
            g_sys_params.role_action = (uint8_t)val;
            param_changed = true;
        } else {
            ESP_LOGE("CMDp", "Invalid role_action value: %d", val);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else {
        ESP_LOGE("CMDp", "Unknown parameter key: %s", key);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (param_changed) {
        // 保存到NVS并应用
        save_system_params_to_nvs();

        // 发送成功响应
        char response[64];
        int key_len = strlen(key);
        int available_space = sizeof(response) - 8; // 减去 "Set OK: " 的长度
        if (key_len > available_space) {
            key_len = available_space;
        }
        snprintf(response, sizeof(response), "Set OK: %.*s", key_len, key);
        send_upload_response(APP_ID_SYS_PARAM, (uint8_t*)response, strlen(response));
        ESP_LOGI("CMDp", "Parameter set successfully: %s", key);
    }
    
    return ESP_OK;
}

// 处理获取参数帧（0xA2命令）
// 帧结构：命令(1) | checksum(2) | 长度(1) | 参数(N)
static esp_err_t handle_get_param_frame(const uint8_t *data, uint16_t length) {
    ESP_LOGI("CMDp", "Handling get param frame, length=%d", length);
    
    // 最小帧长度：命令(1) + checksum(2) + 长度(1) = 4字节
    if (length < 4) {
        ESP_LOGE("CMDp", "Get param frame too short: %d < 4", length);
        return ESP_ERR_INVALID_SIZE;
    }
    
    uint8_t cmd = data[0];
    uint16_t received_checksum = be_to_u16(&data[1]);
    uint8_t param_len = data[3];
    
    // 检查命令是否为0xA2
    if (cmd != CMD_GET_PARAM) {
        ESP_LOGE("CMDp", "Invalid get param command: 0x%02X", cmd);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查总长度是否匹配
    uint16_t expected_length = 1 + 2 + 1 + param_len;
    if (length != expected_length) {
        ESP_LOGE("CMDp", "Length mismatch: expected %d, got %d", expected_length, length);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // 验证checksum（校验数据：长度+参数）
    uint16_t calc_checksum = checksum16(&data[3], length - 3);
    if (received_checksum != calc_checksum) {
        ESP_LOGE("CMDp", "Checksum error: received=0x%04X, calculated=0x%04X",
                 received_checksum, calc_checksum);
        return ESP_ERR_INVALID_CRC;
    }
    
    // 提取参数key
    char key_str[32];
    if (param_len >= sizeof(key_str)) {
        param_len = sizeof(key_str) - 1;
    }
    memcpy(key_str, &data[4], param_len);
    key_str[param_len] = '\0';
    
    ESP_LOGI("CMDp", "Get param key: %s", key_str);
    
    // 构建响应值
    char response[64];
    const char *value = "";
    
    // 根据key获取对应的值
    if (strcmp(key_str, "run_interval") == 0) {
        snprintf(response, sizeof(response), "run_interval=%d", g_sys_params.run_interval);
    }
    else if (strcmp(key_str, "backlight") == 0) {
        snprintf(response, sizeof(response), "backlight=%d", g_sys_params.backlight_enable);
    }
    else if (strcmp(key_str, "bg_mode") == 0) {
        snprintf(response, sizeof(response), "bg_mode=%d", g_sys_params.bg_image_mode);
    }
    else if (strcmp(key_str, "show_mac") == 0) {
        snprintf(response, sizeof(response), "show_mac=%d", g_sys_params.show_mac);
    }
    else if (strcmp(key_str, "pos_label") == 0) {
        snprintf(response, sizeof(response), "pos_label=%d", g_sys_params.pos_label_enable);
    }
    else if (strcmp(key_str, "pos_label_x") == 0) {
        snprintf(response, sizeof(response), "pos_label_x=%d", g_sys_params.pos_label_x);
    }
    else if (strcmp(key_str, "pos_label_y") == 0) {
        snprintf(response, sizeof(response), "pos_label_y=%d", g_sys_params.pos_label_y);
    }
    else if (strcmp(key_str, "heartbeat") == 0) {
        snprintf(response, sizeof(response), "heartbeat=%d", g_sys_params.heartbeat_enable);
    }
    else if (strcmp(key_str, "img1_file") == 0) {
        snprintf(response, sizeof(response), "img1_file=%s", g_sys_params.img1_file);
    }
    else if (strcmp(key_str, "img2_file") == 0) {
        snprintf(response, sizeof(response), "img2_file=%s", g_sys_params.img2_file);
    }
    else if (strcmp(key_str, "role_name") == 0) {
        snprintf(response, sizeof(response), "role_name=%s", g_sys_params.role_name);
    }
    else if (strcmp(key_str, "role_type") == 0) {
        snprintf(response, sizeof(response), "role_type=%d", g_sys_params.role_type);
    }
    else if (strcmp(key_str, "role_action") == 0) {
        snprintf(response, sizeof(response), "role_action=%d", g_sys_params.role_action);
    }
    else if (strcmp(key_str, "Comp") == 0) {
        // Comp 是只读参数，返回当前指南针方向
        if(g_task_running == 0) {
            g_task_running=100;//超时
            xTaskCreatePinnedToCore(i2c0_mmc56x3_task,MMC_TASK_NAME,MMC_TASK_STACK_SIZE,NULL,MMC_TASK_PRIORITY,NULL,0);
            // 非阻塞方式：设置标记位，任务完成时自动发送响应
            g_compending_response = true;
            snprintf(response, sizeof(response), "Comp measuring...");
            ESP_LOGI("CMDp", "Compass measurement started, response will be sent after task completes");
        }
        else {
            snprintf(response, sizeof(response), "Comp busy, try later");
            ESP_LOGI("CMDp", "Compass measurement in progress, try later");
        }
    }
    else {
        snprintf(response, sizeof(response), "Error: Unknown parameter key: %s", key_str);
        ESP_LOGE("CMDp", "Unknown parameter key: %s", key_str);
        send_upload_response(APP_ID_SYS_PARAM, (uint8_t*)response, strlen(response));
        return ESP_ERR_INVALID_ARG;
    }
    
    // 发送响应
    send_upload_response(APP_ID_SYS_PARAM, (uint8_t*)response, strlen(response));
    ESP_LOGI("CMDp", "Get param response: %s", response);

    return ESP_OK;
}

// 处理写卡帧（0xB1命令）
// 帧结构：命令(1) | checksum(2) | 长度(1) | 数据(16)
static esp_err_t handle_write_card_frame(const uint8_t *data, uint16_t length) {
    ESP_LOGI("CMDp", "Handling write card frame, length=%d", length);

    // 最小帧长度：命令(1) + checksum(2) + 长度(1) + 数据(16) = 20字节
    if (length != 20) {
        ESP_LOGE("CMDp", "Write card frame length error: expected 20, got %d", length);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t cmd = data[0];
    uint16_t received_checksum = be_to_u16(&data[1]);
    uint8_t data_len = data[3];

    // 检查命令是否为0xB1
    if (cmd != CMD_WRITE_CARD_FRAME) {
        ESP_LOGE("CMDp", "Invalid write card frame command: 0x%02X", cmd);
        return ESP_ERR_INVALID_ARG;
    }

    // 检查数据长度是否为16
    if (data_len != 16) {
        ESP_LOGE("CMDp", "Invalid data length: expected 16, got %d", data_len);
        return ESP_ERR_INVALID_ARG;
    }

    // 验证checksum（校验数据：长度+数据）
    uint16_t calc_checksum = checksum16(&data[3], length - 3);
    if (received_checksum != calc_checksum) {
        ESP_LOGE("CMDp", "Checksum error: received=0x%04X, calculated=0x%04X",
                 received_checksum, calc_checksum);
        return ESP_ERR_INVALID_CRC;
    }

    // 保存数据到全局变量
    memcpy(g_write_card_data, &data[4], 16);

    ESP_LOGI("CMDp", "Write card data received:");
    ESP_LOGI("CMDp", "Data(hex): %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             g_write_card_data[0], g_write_card_data[1], g_write_card_data[2], g_write_card_data[3],
             g_write_card_data[4], g_write_card_data[5], g_write_card_data[6], g_write_card_data[7],
             g_write_card_data[8], g_write_card_data[9], g_write_card_data[10], g_write_card_data[11],
             g_write_card_data[12], g_write_card_data[13], g_write_card_data[14], g_write_card_data[15]);

    // 启动写卡任务
    esp_err_t ret = start_write_card_task();
    if (ret == ESP_OK) {
        const char *success_msg = "Write card task started";
        send_upload_response(APP_ID_SYSTEM, (uint8_t*)success_msg, strlen(success_msg));
        ESP_LOGI("CMDp", "Write card task created successfully");
    } else {
        const char *error_msg = "Error: Failed to create write card task";
        send_upload_response(APP_ID_SYSTEM, (uint8_t*)error_msg, strlen(error_msg));
        ESP_LOGE("CMDp", "Failed to create write card task");
    }

    return ESP_OK;
}

// 启动写卡任务
static esp_err_t start_write_card_task(void)
{
    // 检查任务是否正在运行，防止重复创建
    if (g_task_running) {
        ESP_LOGW("CMDp", "Write card task already running");
        const char *warning_msg = "Error: Task already running";
        send_upload_response(APP_ID_SYSTEM, (uint8_t*)warning_msg, strlen(warning_msg));
        return ESP_ERR_INVALID_STATE;
    }

    // 创建一次性任务来处理写卡事件
    g_task_running = 100;
    BaseType_t ret = xTaskCreatePinnedToCore(writecard_task, "write_card", KEY_TASK_STACK_SIZE * 2, NULL, KEY_TASK_PRIORITY, NULL, 0);

    if (ret == pdPASS) {
        ESP_LOGI("CMDp", "Write card task created successfully");
        return ESP_OK;
    } else {
        g_task_running = 0;  // 创建失败，重置标志
        ESP_LOGE("CMDp", "Failed to create write card task");
        return ESP_FAIL;
    }
}

// NFC数据处理函数
// 参数：
//       card_uid - 卡片UID（7字节）
//       card_data - NFC卡片数据（16字节）
static void process_nfc_data(unsigned char  *card_uid, unsigned char  *card_data)
{
    if (card_data == NULL) {
        ESP_LOGW("NFC", "Card data is NULL");
        return;
    }

    uint8_t index = card_data[0];

    ESP_LOGI("NFC", "Processing NFC data, index=0x%02X", index);

    switch (index) 
    {
        case 0x31:  // 六边形坐标
        {
            if (True_Heading >= 0 && True_Heading <= 360) {
                // 将指南针方向转换为六边形坐标格式
                int hex_coord = (True_Heading * 6) / 360;
                card_data[3] = (hex_coord >> 8) & 0xFF;  // x-high
                card_data[4] = hex_coord & 0xFF;           // x-low
                card_data[5] = (hex_coord >> 8) & 0xFF;  // y-high
                card_data[6] = hex_coord & 0xFF;           // y-low
                card_data[7] = (hex_coord >> 8) & 0xFF;  // z-high
                card_data[8] = hex_coord & 0xFF;           // z-low
                ESP_LOGI("NFC", "Hex coord set: %d, compass: %d", hex_coord, True_Heading);
            }
            break;
        }

        case 0x32:  // 角色名称
        {
            // 将角色名称写入card_data
            // card_data[2]开始，最多14字节
            memset(&card_data[2], 0, 14);
            strncpy((char*)&card_data[2], g_sys_params.role_name, 14);
            ESP_LOGI("NFC", "Role name written: %s", g_sys_params.role_name);
            break;
        }

        case 0x33:  // 角色类型
        {
            // 将角色类型写入card_data
            // card_data[2]开始，最多14字节
            memset(&card_data[2], 0, 14);
            snprintf((char*)&card_data[2], 14, "%d", g_sys_params.role_type);
            ESP_LOGI("NFC", "Role type written: %d", g_sys_params.role_type);
            break;
        }

        case 0x34:  // 行动类型
        {
            // 将行动类型写入card_data
            // card_data[2]开始，最多14字节
            memset(&card_data[2], 0, 14);
            snprintf((char*)&card_data[2], 14, "%d", g_sys_params.role_action);
            ESP_LOGI("NFC", "Role action written: %d", g_sys_params.role_action);
            break;
        }

        case 0x11:  // 设置指南针修正方向值
        {
            // 格式：card_data[0]=0x11, card_data[1]=0x21, card_data[2]=0x31, {调用}
            if(card_data[1] != 0x21 || card_data[2] != 0x31) {
                g_sys_params.comp_offset = Compass_Heading;
                save_system_params_to_nvs();
                ESP_LOGI("NFC", "Compass offset set to: %d degrees", g_sys_params.comp_offset);
            }
            break;
        }

        default:
            ESP_LOGW("NFC", "Unknown index: 0x%02X", index);
            break;
    }
}

static void hidd_event_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event)
    {
    case ESP_HIDD_EVENT_REG_FINISH:
    {
        if (param->init_finish.state == ESP_HIDD_INIT_OK)
        {
            // esp_bd_addr_t rand_addr = {0x04,0x11,0x11,0x11,0x11,0x05};
            esp_ble_gap_set_device_name(blerename);
            esp_ble_gap_config_adv_data(&hidd_adv_data);
        }
        break;
    }
    case ESP_BAT_EVENT_REG:
    {
        ESP_LOGI("HIDevent", "ESP_BAT_EVENT_REG");
        break;
    }
    case ESP_HIDD_EVENT_DEINIT_FINISH:
        break;
    case ESP_HIDD_EVENT_BLE_CONNECT:
    {
        ESP_LOGI("HIDevent", "ESP_HIDD_EVENT_BLE_CONNECT");
        hid_conn_id = param->connect.conn_id;
        sec_conn = true;

        // 保存远端设备地址
        memcpy(remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));

        // 等待连接稳定后再更新参数
        vTaskDelay(pdMS_TO_TICKS(100));

        // 读取RSSI值
        esp_err_t rssi_ret = esp_ble_gap_read_rssi(param->connect.remote_bda);
        if (rssi_ret == ESP_OK) {
            ESP_LOGI("HIDevent", "Reading RSSI...");
        } else {
            ESP_LOGE("HIDevent", "Failed to read RSSI: %s", esp_err_to_name(rssi_ret));
        }

        // 先设置MTU为470
        esp_err_t mtu_ret = esp_ble_gatt_set_local_mtu(470);
        if (mtu_ret == ESP_OK) {
            ESP_LOGI("HIDevent", "MTU set to 470");
        } else {
            ESP_LOGE("HIDevent", "Failed to set MTU: %s", esp_err_to_name(mtu_ret));
        }

        // 检查连接是否仍然有效
        if (hid_conn_id != 0) {
            esp_ble_conn_update_params_t conn_params = {0};
            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            conn_params.latency = 3;
            conn_params.max_int = 0x100;    // max_int = 0x20*1.25ms = 40ms
            conn_params.min_int = 0x10;    // min_int = 0x10*1.25ms = 20ms
            conn_params.timeout = 900;     // timeout = 500*10ms = 5000ms
            esp_err_t ret = esp_ble_gap_update_conn_params(&conn_params);
            if (ret != ESP_OK) {
                ESP_LOGE("HIDevent", "Failed to update conn params: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI("HIDevent", "Connection parameters updated");
            }
        }

        break;
    }
    case ESP_HIDD_EVENT_BLE_DISCONNECT:
    {
        sec_conn = false;
        hid_conn_id = 0;
        // 清空远端设备地址
        memset(remote_bda, 0, sizeof(esp_bd_addr_t));
        ESP_LOGI("HIDevent", "ESP_HIDD_EVENT_BLE_DISCONNECT");

        // 检查是否有正在进行的文件传输
        if (g_img_protocol.state != PROTOCOL_STATE_IDLE) {
            ESP_LOGW("HIDevent", "Connection lost during file transfer, cleaning up...");
            if (g_img_protocol.file_open) {
                lv_fs_close(&g_img_protocol.file_handle);
                g_img_protocol.file_open = false;
                ESP_LOGW("HIDevent", "File closed due to disconnection");
            }
            // 重置协议状态
            reset_protocol_state();
        }

        // 等待断开完全完成后重新广播
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_err_t ret = esp_ble_gap_start_advertising(&hidd_adv_params);
        if (ret == ESP_OK) {
            ESP_LOGI("HIDevent", "Advertising restarted successfully");
        } else {
            ESP_LOGE("HIDevent", "Failed to restart advertising: %s", esp_err_to_name(ret));
        }
        break;
    }
    case ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT:
    {
        ESP_LOGI("HIDevent", "%s, ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT", __func__);
        ESP_LOG_BUFFER_HEX("HIDevent", param->vendor_write.data, param->vendor_write.length);
        break;
    }
    case ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT:
    {
        ESP_LOGI("HIDevent", "ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT");
        ESP_LOG_BUFFER_HEX("HIDevent", param->led_write.data, param->led_write.length);
        break;
    }
    case ESP_HIDD_EVENT_NUS_UART_RX_EVT:
    {
        ESP_LOGI("HIDevent", "ESP_HIDD_EVENT_NUS_UART_RX_EVT, len=%d", param->nus_uart_rx.length);
        ESP_LOG_BUFFER_HEX("HIDevent", param->nus_uart_rx.data, param->nus_uart_rx.length);

        // 调用图片传输协议处理函数
        process_protocol_data(param->nus_uart_rx.data, param->nus_uart_rx.length);

        break;
    }
    default:
        break;
    }
    return;
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&hidd_adv_params);
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        for (int i = 0; i < ESP_BD_ADDR_LEN; i++)
        {
            ESP_LOGD("GAPevent", "%x:", param->ble_security.ble_req.bd_addr[i]);
        }
///        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        sec_conn = true;
        esp_bd_addr_t bd_addr;
        memcpy(bd_addr, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
        ESP_LOGI("GAPevent", "remote BD_ADDR: %08x%04x",
                 (bd_addr[0] << 24) + (bd_addr[1] << 16) + (bd_addr[2] << 8) + bd_addr[3],
                 (bd_addr[4] << 8) + bd_addr[5]);
        ESP_LOGI("GAPevent", "address type = %d", param->ble_security.auth_cmpl.addr_type);
        ESP_LOGI("GAPevent", "pair status = %s", param->ble_security.auth_cmpl.success ? "success" : "fail");
        if (!param->ble_security.auth_cmpl.success)
        {
            ESP_LOGE("GAPevent", "fail reason = 0x%x", param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
        ESP_LOGI("GAPevent", "RSSI = %d dBm", param->read_rssi_cmpl.rssi);
        break;
    default:
        break;
    }
}

/**
 * @brief 发送回传协议响应（0xFE协议）
 *
 * @inputs
 *  - app_id: 应用ID（从0x01开始）
 *  - payload: 数据负载（应用数据）
 *  - payload_len: 负载长度（1~255）
 * @outputs
 *  - 返回 ESP_OK 成功，其他失败
 *
 * 帧结构：
 * 1. 协议头：1字节，固定 0xFE
 * 2. 应用ID：1字节，由应用赋值，从 0x01 开始
 * 3. Checksum校验位：2字节，checksum16(长度+数据)，包含长度位和以后的所有字段
 * 4. 数据长度位：1字节，N（N为负载字节数），范围1~255，随负载变化
 * 5. 数据负载：N字节，应用数据，具体要求由应用来定，N≤255，尾帧可不足255字节
 */
static esp_err_t send_upload_response(uint8_t app_id, const uint8_t *payload, uint16_t payload_len)
{
    //ESP_LOGI("Upload", "Sending upload response - AppID: 0x%02X, Payload: %d bytes", app_id, payload_len);

    // 检查参数
    if (payload == NULL || payload_len == 0 || payload_len > 255) {
        ESP_LOGE("Upload", "Invalid payload parameters: payload=%p, len=%d", payload, payload_len);
        return ESP_ERR_INVALID_ARG;
    }

    // 检查 notify 是否已启用
    if (!notifyEN()) {
        ESP_LOGD("Upload", "Notify not enabled");
        return ESP_ERR_INVALID_STATE;
    }

    // 构建帧：协议头(1) + 应用ID(1) + checksum(2) + 长度(1) + 负载(N)
    uint16_t frame_size = 1 + 1 + 2 + 1 + payload_len;
    uint8_t *frame = heap_caps_malloc(frame_size, MALLOC_CAP_DMA);
    
    if (frame == NULL) {
        ESP_LOGE("Upload", "Failed to allocate memory for frame");
        return ESP_ERR_NO_MEM;
    }

    // 1. 协议头：0xFE
    frame[0] = 0xFE;

    // 2. 应用ID
    frame[1] = app_id;

    // 3. 数据长度位
    frame[4] = (uint8_t)payload_len;

    // 4. 数据负载
    if (payload_len > 0) {
        memcpy(&frame[5], payload, payload_len);
    }

    // 5. Checksum校验位（校验：数据长度位 + 数据负载）
    uint16_t checksum = 0;
    for (uint16_t i = 0; i < 1 + payload_len; i++) {
        checksum += frame[4 + i];  // 从数据长度位开始校验，共length(1) + payload(N)字节
    }
    checksum &= 0xFFFF;  // 取低16位

    // 写入checksum（大端序）
    frame[2] = (uint8_t)((checksum >> 8) & 0xFF);
    frame[3] = (uint8_t)(checksum & 0xFF);

    //ESP_LOG_BUFFER_HEX("Upload", frame, frame_size);

    // 发送数据
    esp_err_t ret = nus_uart_send_data(hid_conn_id, frame, frame_size);
    if (ret == ESP_OK) {
        ESP_LOGI("Upload", "Upload success, APPID: 0x%02X, size: %d bytes", app_id, frame_size);
    } else {
        ESP_LOGE("Upload", "Failed to send upload response: %s", esp_err_to_name(ret));
    }
    
    heap_caps_free(frame);
    return ret;
}

/**
 * @brief 分包发送回传协议响应（0xFE协议）
 *
 * @inputs
 *  - app_id: 应用ID（从0x01开始）
 *  - payload: 数据负载（应用数据）
 *  - payload_len: 负载长度（可超过255）
 * @outputs
 *  - 返回 ESP_OK 成功，其他失败
 *
 * 帧结构：
 * 1. 协议头：1字节，固定 0xFE
 * 2. 应用ID：1字节，由应用赋值，从 0x01 开始
 * 3. Checksum校验位：2字节，checksum16(长度+数据)，包含长度位和以后的所有字段
 * 4. 数据长度位：1字节，N（N为负载字节数+2，包含总包数和当前包序号），范围1~255
 * 5. 总包数：1字节，表示数据总共有多少个包
 * 6. 当前包序号：1字节，从0开始，标识当前是第几个包
 * 7. 数据负载：N-2字节，应用数据，尾帧可不足N-2字节
 */
static esp_err_t send_upload_response_fragmented(uint8_t app_id, const uint8_t *payload, uint16_t payload_len)
{
    // 检查参数
    if (payload == NULL || payload_len == 0) {
        ESP_LOGE("Upload", "Invalid payload parameters: payload=%p, len=%d", payload, payload_len);
        return ESP_ERR_INVALID_ARG;
    }

    // 检查 notify 是否已启用
    if (!notifyEN()) {
        ESP_LOGE("Upload", "Notify not enabled");
        return ESP_ERR_INVALID_STATE;
    }

    // 如果数据长度不超过253字节（需要给总包数和当前包序号预留2字节），直接发送
    if (payload_len <= 253) {
        ESP_LOGI("Upload", "Data size %d <= 253, sending directly", payload_len);
        return send_upload_response(app_id, payload, payload_len);
    }

    // 计算分包信息
    const uint16_t max_payload_per_packet = 253; // 每包最大数据量（255 - 2字节包头）
    uint16_t remaining = payload_len;
    uint16_t offset = 0;
    uint8_t total_packets = (payload_len + max_payload_per_packet - 1) / max_payload_per_packet;

    ESP_LOGI("Upload", "Fragmented send - Total: %d bytes, Packets: %d, Max per packet: %d",
             payload_len, total_packets, max_payload_per_packet);

    // 逐包发送
    for (uint8_t packet_num = 0; packet_num < total_packets; packet_num++) {
        uint16_t current_payload_size = (remaining > max_payload_per_packet) ?
                                       max_payload_per_packet : remaining;

        // 构建临时payload：总包数 + 当前包序号 + 数据负载
        uint16_t temp_payload_len = 2 + current_payload_size;
        uint8_t *temp_payload = heap_caps_malloc(temp_payload_len, MALLOC_CAP_DMA);

        if (temp_payload == NULL) {
            ESP_LOGE("Upload", "Failed to allocate memory for packet %d", packet_num);
            return ESP_ERR_NO_MEM;
        }

        temp_payload[0] = total_packets;      // 总包数
        temp_payload[1] = packet_num;         // 当前包序号
        memcpy(&temp_payload[2], &payload[offset], current_payload_size);

        // 构建帧：协议头(1) + 应用ID(1) + checksum(2) + 长度(1) + temp_payload(N)
        uint16_t frame_size = 1 + 1 + 2 + 1 + temp_payload_len;
        uint8_t *frame = heap_caps_malloc(frame_size, MALLOC_CAP_DMA);

        if (frame == NULL) {
            ESP_LOGE("Upload", "Failed to allocate memory for frame %d", packet_num);
            heap_caps_free(temp_payload);
            return ESP_ERR_NO_MEM;
        }

        // 1. 协议头：0xFE
        frame[0] = 0xFE;

        // 2. 应用ID
        frame[1] = app_id;

        // 3. 数据长度位（包含总包数和当前包序号）
        frame[4] = (uint8_t)temp_payload_len;

        // 4. 数据负载（总包数 + 当前包序号 + 实际数据）
        memcpy(&frame[5], temp_payload, temp_payload_len);

        // 5. Checksum校验位（校验：数据长度位 + 数据负载）
        uint16_t checksum = 0;
        for (uint16_t i = 0; i < 1 + temp_payload_len; i++) {
            checksum += frame[4 + i];  // 从数据长度位开始校验
        }
        checksum &= 0xFFFF;  // 取低16位

        // 写入checksum（大端序）
        frame[2] = (uint8_t)((checksum >> 8) & 0xFF);
        frame[3] = (uint8_t)(checksum & 0xFF);

        // 发送数据
        esp_err_t ret = nus_uart_send_data(hid_conn_id, frame, frame_size);
        if (ret == ESP_OK) {
            ESP_LOGI("Upload", "Packet %d/%d sent, size: %d bytes",
                     packet_num + 1, total_packets, frame_size);
        } else {
            ESP_LOGE("Upload", "Failed to send packet %d: %s",
                     packet_num, esp_err_to_name(ret));
            heap_caps_free(temp_payload);
            heap_caps_free(frame);
            return ret;
        }

        // 释放内存
        heap_caps_free(temp_payload);
        heap_caps_free(frame);

        // 更新偏移量和剩余长度
        offset += current_payload_size;
        remaining -= current_payload_size;

        // 包间延迟，避免发送过快导致丢包
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI("Upload", "All %d packets sent successfully", total_packets);
    return ESP_OK;
}

// 协议数据接收入口函数
static void process_protocol_data(const uint8_t *data, uint16_t length) {
    if (length == 0 || data == NULL) {
        ESP_LOGW("CMDp", "Empty data received");
        return;
    }

    uint8_t cmd = data[0];

    ESP_LOGI("CMDp", "Processing protocol data - Cmd: 0x%02X, Length: %d", cmd, length);

    switch (cmd) {
        //0xD1
        case CMD_INIT_FRAME:
            // 收到新的0xD1命令，重置状态并开始新的传输
            reset_protocol_state();
            if (handle_init_frame(data, length) != ESP_OK) {
                ESP_LOGE("CMDp", "Failed to handle init frame");
                reset_protocol_state();
            }
            break;
        //0xD2
        case CMD_DATA_FRAME:
            // 处理数据帧
            if (handle_data_frame(data, length) != ESP_OK) {
                ESP_LOGE("CMDp", "Failed to handle data frame");
                if (g_img_protocol.transfer_error) {
                    // 如果设置了错误标志，完全重置状态
                    reset_protocol_state();
                }
            }
            break;
        //0xD4
        case CMD_DELETE_FRAME:
            // 处理删除文件命令
            if (handle_delete_frame(data, length) != ESP_OK) {
                ESP_LOGE("CMDp", "Failed to handle delete frame");
            }
            break;
        //0xA1
        case CMD_SET_PARAM:
            // 处理设置系统参数命令
            if (handle_set_param_frame(data, length) != ESP_OK) {
                ESP_LOGE("CMDp", "Failed to handle set param frame");
            }
            break;
        //0xA2
        case CMD_GET_PARAM:
            // 处理获取系统参数命令
            if (handle_get_param_frame(data, length) != ESP_OK) {
                ESP_LOGE("CMDp", "Failed to handle get param frame");
            }
            break;
        //0xB1 - 写卡命令
        case CMD_WRITE_CARD_FRAME:
            // 处理写卡命令
            if (handle_write_card_frame(data, length) != ESP_OK) {
                ESP_LOGE("CMDp", "Failed to handle write card frame");
            }
            break;
        //0xEE - 系统命令
        case CMD_SYS_PREFIX:
            if (length >= 2) {
                // 检查多字节系统命令
                // 格式化命令：0xEE 0x66 0x6F 0x72 0x6D 0x61 0x74 ("format")
                if (length == CMD_FORMAT_LEN &&
                    data[1] == 0x66 && data[2] == 0x6F && data[3] == 0x72 &&
                    data[4] == 0x6D && data[5] == 0x61 && data[6] == 0x74) {
                    ESP_LOGI("CMDp", "Format command received");

                    lv_fs_res_t ret = lv_port_fs_format();
                    char response[24];
                    snprintf(response, sizeof(response), "Format: %s",
                             (ret == LV_FS_RES_OK) ? "OK" : "Failed");
                        //nus_uart_send_data(hid_conn_id, (uint8_t*)response, strlen(response));
                        send_upload_response(APP_ID_SYSTEM, (uint8_t*)response, strlen(response));
                }
                // 列出目录命令：0xEE 0x64 0x69 0x72 ("dir")
                else if (length == CMD_DIR_LEN &&
                         data[1] == 0x64 && data[2] == 0x69 && data[3] == 0x72) {
                    ESP_LOGI("CMDp", "List directory command received");

                    char *dir_content = lv_port_fs_get_dir_content("/");
                    if (dir_content) {
                            //nus_uart_send_data(hid_conn_id, (uint8_t*)dir_content, strlen(dir_content));
                            send_upload_response_fragmented(APP_ID_SYSTEM, (uint8_t*)dir_content, strlen(dir_content));
                        free(dir_content);
                    } else {
                        const char *error_msg = "Error: Failed to get directory content";
                            // nus_uart_send_data(hid_conn_id, (uint8_t*)error_msg, strlen(error_msg));
                            send_upload_response(APP_ID_SYSTEM, (uint8_t*)error_msg, strlen(error_msg));
                    }
                }
                // 重启命令：0xEE 0x72 0x65 0x73 0x65 0x74 ("reset")
                else if (length == CMD_RESET_LEN &&
                         data[1] == 0x72 && data[2] == 0x65 && data[3] == 0x65 &&
                         data[4] == 0x74) {
                    ESP_LOGI("CMDp", "Reset command received");

                        const char *msg = "Resetting MCU...";
                        //nus_uart_send_data(hid_conn_id, (uint8_t*)msg, strlen(msg));
                        send_upload_response(APP_ID_SYSTEM, (uint8_t*)msg, strlen(msg));
                        vTaskDelay(pdMS_TO_TICKS(500));  // 等待消息发送

                    esp_restart();
                }
                // NFC触发命令：0xEE 0x4E 0x46 0x43 ("NFC")
                else if (length == CMD_NFC_LEN &&
                         data[1] == 0x4E && data[2] == 0x46 && data[3] == 0x43) {
                    ESP_LOGI("CMDp", "NFC trigger command received");
                    start_measure_task();
                }
                else {
                    ESP_LOGW("CMDp", "Unknown system command, length=%d", length);
                }
            } else {
                ESP_LOGW("CMDp", "System command too short, length=%d", length);
            }
            break;

        default:
            ESP_LOGW("CMDp", "Unknown command: 0x%02X", cmd);
            break;
    }
}
































































_Noreturn void app_main(void) {

  IIC_init();
  npd_gpio_init();
  gpio_interrupt_init();
  adc_init();  // 初始化ADC，用于读取电池电压
  BG_EN(0);


  // 自动创建任务，按键触发
  xTaskCreatePinnedToCore(i2c0_mmc56x3_task,MMC_TASK_NAME,MMC_TASK_STACK_SIZE,NULL,MMC_TASK_PRIORITY,NULL,0);

  xTaskCreate(PrintChipInfo, "PrintChipInfo", 1024 * 4, NULL, 1, NULL);
  fflush(stdout);
  {
    TaskHandle_t print_chip_info_handle = xTaskGetHandle("PrintChipInfo");
    if (print_chip_info_handle != NULL) {
      vTaskDelete(print_chip_info_handle);
      ESP_LOGI("app_main", "Task PrintChipInfo delete.");
    }
  }
  // while (1) {
     vTaskDelay(pdMS_TO_TICKS(100));
  // }

    esp_err_t ret;

// Initialize NVS.
        ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);

        nvs_handle_t handle;
        int32_t  startcounter;
        ret = nvs_open("VS", NVS_READWRITE, &handle);

        if (ret == ESP_OK)
        {
            // 读取
            // int32_t val = 0;
            nvs_get_i32(handle, "start", &startcounter);
            ESP_LOGI("nvs", "start: %d ", (int)startcounter);
            startcounter++;
            // 写入
            nvs_set_i32(handle, "start", startcounter);
            nvs_commit(handle);

            // uint8_t bdAddr[6];
            // const uint8_t *add= esp_bt_dev_get_address();
            // memcpy(bdAddr,add,sizeof(esp_bd_addr_t));
            // ESP_LOGI(TAG, "Bluetooth Address is  %X:%X:%X:%X:%X:%X ",bdAddr[0],bdAddr[1],bdAddr[2],bdAddr[3],bdAddr[4],bdAddr[5]);

            uint8_t fmac[6] = {72, 49, 183, 93, 232, 58};
            // 定义macAddr为uint8_t类型的数组，这个数组含有6个元素。
            esp_read_mac(&macAddr, ESP_MAC_BT); // MAC地址会储存在这个macAddr数组里面

            ESP_LOGI("nvs", "Bluetooth Address is %02X:%02X:%02X:%02X:%02X:%02X ", macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
            // int8_t temp_value[6] = 0;
            size_t len = 6;
            if (nvs_get_blob(handle, "fces", &fmac, &len) != ESP_OK) //== ESP_ERR_NVS_NOT_FOUND)
            {
                nvs_set_blob(handle, "fces", macAddr, len);
                nvs_commit(handle);
            }
            else
            {
                // fmac[0] = 0;
                ESP_LOGI("nvs", "%03d%03d%03d%09d%03d%03d%03d\n", macAddr[0], macAddr[1], macAddr[2], memcmp(macAddr, fmac, 6), macAddr[3], macAddr[4], macAddr[5]);
                // 072-049-183--00000032-093-232-058
                // 判断内置MAC与芯片MAC是否有差异
                // 打印的信息是mac地址的10进制，每位16进制转换位3位数，前9位和后9位，中间9位长度不定，是设定值与实际的差。
                //uint32_t diff = 100; // 100s
                if (memcmp(macAddr, fmac, 6))
                {
                    // MAC地址有差异，执行相应的处理
                    ESP_LOGI("nvs", "MAC detected");
                }
            }
        }
          // 加载系统参数
    load_system_params_from_nvs();





vTaskDelay(pdMS_TO_TICKS(100));

//BLE
  if(1)
  {
      sprintf(blerename, "%s%02X%02X%02X", HIDD_DEVICE_NAME, macAddr[3], macAddr[4], macAddr[5]);
      ESP_LOGI("BLEinit", "Device name: %s", blerename);

      ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

      esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
      ret = esp_bt_controller_init(&bt_cfg);
      if (ret)
      {
          ESP_LOGE("BLEinit", "%s initialize controller failed", __func__);
          return;
      }

      ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
      if (ret)
      {
          ESP_LOGE("BLEinit", "%s enable controller failed", __func__);
          return;
      }

      ret = esp_bluedroid_init();
      if (ret)
      {
          ESP_LOGE("BLEinit", "%s init bluedroid failed", __func__);
          return;
      }

      ret = esp_bluedroid_enable();
      if (ret)
      {
          ESP_LOGE("BLEinit", "%s init bluedroid failed", __func__);
          return;
      }

      if ((ret = esp_hidd_profile_init()) != ESP_OK)
      {
          ESP_LOGE("BLEinit", "%s init bluedroid failed", __func__);
      }
    vTaskDelay(pdMS_TO_TICKS(100));

      /// register the callback function to the gap module
      esp_ble_gap_register_callback(gap_event_handler);
      esp_hidd_register_callbacks(hidd_event_callback); // set name

      /* set the security iocap & auth_req & key size & init key response key parameters to the stack*/
      //esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND; // bonding with peer device after authentication  ESP_LE_AUTH_NO_BOND
      esp_ble_auth_req_t auth_req = ESP_LE_AUTH_NO_BOND; // bonding with peer device after authentication  
      esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;       // set the IO capability to No output No input
      uint8_t key_size = 16;                          // the key size should be 7~16 bytes
      uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
      uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
      esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
      esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
      esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
      /* If your BLE device act as a Slave, the init_key means you hope which types of key of the master should distribute to you,
      and the response key means which key you can distribute to the Master;
      If your BLE device act as a master, the response key means you hope which types of key of the slave should distribute to you,
      and the init key means which key you can distribute to the slave. */
      esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
      esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    }

     vTaskDelay(pdMS_TO_TICKS(100));





  /**
   * \brief Start LVGL demo.
   */
  lv_init();
  lvgl_driver_init();
  lv_port_fs_init();

  lv_color_t *buf1 =
      heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
  assert(buf1 != NULL);
  /* Use double buffered when not working with monochrome displays */
// #ifndef CONFIG_LV_TFT_DISPLAY_MONOCHROME
//   lv_color_t *buf2 = heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
//   assert(buf2 != NULL);
// #else
  static lv_color_t *buf2 = NULL;
// #endif
  static lv_disp_draw_buf_t disp_buf;
  uint32_t size_in_px = DISP_BUF_SIZE;
  lv_disp_draw_buf_init(&disp_buf, buf1, buf2, size_in_px);
  lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = CONFIG_LV_HOR_RES_MAX;
  disp_drv.ver_res = CONFIG_LV_VER_RES_MAX;
  disp_drv.flush_cb = disp_driver_flush;
  disp_drv.rotated = 1;
  disp_drv.draw_buf = &disp_buf;
  lv_disp_drv_register(&disp_drv);


  /* Register an input device when enabled on the menuconfig */
#if CONFIG_LV_TOUCH_CONTROLLER != TOUCH_CONTROLLER_NONE
  ESP_LOGI(TAG, "TOUCH CONTROLLER");
  lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.read_cb = touch_driver_read;
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  lv_indev_drv_register(&indev_drv);
#endif

//   const esp_timer_create_args_t periodic_timer_args = {
//       .callback = &lv_tick_task, .name = "screen"};
//   esp_timer_handle_t periodic_timer;
//   ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
//   ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 1000));



  // 启动 LVGL widgets demo
  // #if LV_USE_DEMO_WIDGETS
  //   ESP_LOGI(__FILENAME__, "Starting LVGL Widgets Demo");
  //   lv_demo_widgets();
  // #else
  //   // 如果没有启用 demo widgets，显示简单的 Hello world
  //   lv_obj_t *label = lv_label_create(lv_scr_act());
  //   if (NULL != label) {
  //     lv_label_set_text(label, "Hello world\nLV_USE_DEMO_WIDGETS is disabled.\nEnable it in menuconfig.");
  //     lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
  //   }
  // #endif



    
    ESP_LOGI("SYS", "Run interval: %d seconds", g_sys_params.run_interval);



    // 创建全屏背景图片 (128*128)
    bg_img = lv_img_create(lv_scr_act());
    char bg_img_path[35];
    snprintf(bg_img_path, sizeof(bg_img_path), "A:/%s", g_sys_params.img1_file);
    lv_img_set_src(bg_img, bg_img_path);
    lv_obj_set_size(bg_img, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(bg_img);

        // 创建下层图片 (64*128) 显示
        bom_img = lv_img_create(lv_scr_act());
        char bom_img_path[35];
        snprintf(bom_img_path, sizeof(bom_img_path), "A:/%s", g_sys_params.img2_file);
        lv_img_set_src(bom_img, bom_img_path);
        lv_obj_set_size(bom_img, 128, 64);
        lv_obj_align(bom_img, LV_ALIGN_TOP_LEFT, 0, 64);

        // // 创建100w.gif动画显示
        // gif_obj = lv_gif_create(lv_scr_act());
        // lv_gif_set_src(gif_obj, "A:/100w.gif");
        // //lv_obj_set_size(gif_obj, 80, 80);
        // lv_obj_align(gif_obj, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    

    // 创建top标签
    static lv_obj_t *top_label;
    top_label = lv_label_create(lv_scr_act());
    lv_label_set_text(top_label, "102030405060 100%% 00000");
    lv_obj_set_style_text_color(top_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(top_label, &lv_font_montserrat_10, 0);
    lv_obj_align(top_label, LV_ALIGN_TOP_MID, 0, 0);
    

    // 创建信息标签
    pos_label = lv_label_create(lv_scr_act());
    lv_label_set_text(pos_label, "4444");
    lv_obj_set_style_text_color(pos_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(pos_label, &lv_font_montserrat_14, 0);
    if(g_sys_params.pos_label_enable) {
        lv_obj_align(pos_label, LV_ALIGN_TOP_MID, g_sys_params.pos_label_x, g_sys_params.pos_label_y);
        ESP_LOGI("SYS", "Pos label: %s at (%d, %d)", 
             g_sys_params.pos_label_enable ? "ON" : "OFF",
             g_sys_params.pos_label_x, g_sys_params.pos_label_y);
    } else {
        lv_obj_align(pos_label, LV_ALIGN_TOP_MID, g_sys_params.pos_label_x+128, g_sys_params.pos_label_y);
    }


    temperature_sensor_handle_t temp_handle = NULL;
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 50);
    ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_handle));



  // 记录上次使用的图片文件名，用于检测变化
  static char last_img1_file[20] = "";
  static char last_img2_file[20] = "";
  static uint8_t last_bg_image_mode = 2;  // 记录上次的显示模式，初始值为2（无效值，确保首次检查）

  while (1) {
    ESP_LOGI("app_main", "Free Heap Size: %lu", esp_get_minimum_free_heap_size());

    // 背光控制
    BG_EN(g_sys_params.backlight_enable);

    // 检查图片1文件名是否变化
    if (strcmp(last_img1_file, g_sys_params.img1_file) != 0) {
        strncpy(last_img1_file, g_sys_params.img1_file, 19);
        last_img1_file[19] = '\0';
        char bg_img_path[35];
        snprintf(bg_img_path, sizeof(bg_img_path), "A:/%s", g_sys_params.img1_file);
        lv_img_set_src(bg_img, bg_img_path);
        ESP_LOGI("app_main", "Background image updated: %s", bg_img_path);
    }

    // 检查图片2文件名是否变化
    if (strcmp(last_img2_file, g_sys_params.img2_file) != 0) {
        strncpy(last_img2_file, g_sys_params.img2_file, 19);
        last_img2_file[19] = '\0';
        char bom_img_path[35];
        snprintf(bom_img_path, sizeof(bom_img_path), "A:/%s", g_sys_params.img2_file);
        lv_img_set_src(bom_img, bom_img_path);
        ESP_LOGI("app_main", "Foreground image updated: %s", bom_img_path);
    }

    // 检查图片模式是否变化
    if (last_bg_image_mode != g_sys_params.bg_image_mode) {
        last_bg_image_mode = g_sys_params.bg_image_mode;
        if (g_sys_params.bg_image_mode == 0) {
            // 显示图片2（上层图片）
            lv_obj_clear_flag(bom_img, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI("app_main", "Foreground image shown (mode=0)");
        } else {
            // 隐藏图片2（上层图片）
            lv_obj_add_flag(bom_img, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI("app_main", "Foreground image hidden (mode=1)");
        }
    }

    // 更新位置标签显示
    lv_label_set_text(pos_label, g_sys_params.role_name);
    lv_obj_set_style_text_color(pos_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(pos_label, &lv_font_montserrat_14, 0);
    if(g_sys_params.pos_label_enable) {
        lv_obj_align(pos_label, LV_ALIGN_TOP_MID, g_sys_params.pos_label_x, g_sys_params.pos_label_y);
        lv_obj_clear_flag(pos_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pos_label, LV_OBJ_FLAG_HIDDEN);
    }


    
    // 启用温度传感器
    ESP_ERROR_CHECK(temperature_sensor_enable(temp_handle));
    float tsens_out;
    ESP_ERROR_CHECK(temperature_sensor_get_celsius(temp_handle, &tsens_out));
    //ESP_LOGI("app_main", "Free Heap Size: %f", tsens_out);
    // 温度传感器使用完毕后，禁用温度传感器，节约功耗
    ESP_ERROR_CHECK(temperature_sensor_disable(temp_handle));

    // 读取电池电压并计算电量
    uint32_t battery_voltage_mv = read_battery_voltage();
    int battery_capacity = battery_calculate_capacity(battery_voltage_mv);

    if(g_sys_params.show_mac || g_sys_params.heartbeat_enable) {
        if( g_sys_params.heartbeat_enable == 2)  g_sys_params.heartbeat_enable = 0;
        // 更新开机时间显示（使用RTC时间，深度睡眠时仍然运行）
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        uint32_t uptime_seconds = (uint32_t)tv_now.tv_sec;
        char nus_data[64] = "";  // 增加缓冲区大小以包含电池信息
            snprintf((char*)nus_data, sizeof(nus_data), "%02X%02X%02X%02X%02X%02X %lu",
                    macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5],
                    uptime_seconds);
        if(g_sys_params.show_mac) {
            lv_label_set_text_fmt(top_label, nus_data);
            lv_obj_clear_flag(top_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(top_label, LV_OBJ_FLAG_HIDDEN);
        }
        // 根据heartbeat参数决定是否发送心跳包
        if (g_sys_params.heartbeat_enable) {
            snprintf((char*)nus_data, sizeof(nus_data), "%02X:%02X:%02X:%02X:%02X:%02X %lu %lu %d%% %dC",
                macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5],
                uptime_seconds, battery_voltage_mv, battery_capacity, (int)tsens_out);
            send_upload_response(APP_ID_STATUS, (uint8_t*)nus_data, strlen((char*)nus_data));
        }
    }
    
    lv_tick_inc(100);
    lv_task_handler();
    vTaskDelay(pdMS_TO_TICKS(g_sys_params.run_interval));


  }


  free(buf1);
  // buf2 is NULL, no need to free
  vTaskDelete(NULL);
}



















void writecard_task( void *pvParameters )
{
    (void)pvParameters;
    int status = 5;
    unsigned char carduid[10];
    unsigned char cardpid[16];

    ESP_LOGI(MMC_TAG, "Write card task, powering on devices...");

    // 上电并等待模块稳定
    NPD_EN(1);
    vTaskDelay(pdMS_TO_TICKS(100));
    // 初始化SI523
    SI523_Init(i2c0_bus_hdl);
    vTaskDelay(pdMS_TO_TICKS(10)); // 等待10ms让SI523初始化完成

    for(int i = 0; i < status; i++) {
        if(SI523_CheckVer() != 0){
        PCD_SI523_TypeA_Init();
        //PCD_SI523_TypeA();
        if(PCD_SI523_TypeA_GetUID(carduid)==0){

            if(SI523_write_YURIDATA() == MI_OK){
              ESP_LOGI(MMC_TAG, "YURIDATA write successful");
            }

            // 使用全局变量 g_write_card_data 中的16字节数据，分4次写入，每次4字节
            memcpy(cardpid, &g_write_card_data[0], 16);
            if(SI523_write_NTAG(12, cardpid) == MI_OK){
              ESP_LOGI(MMC_TAG, "NTAG write successful (bytes 0-3)");
            }

            // memcpy(cardpid, &g_write_card_data[4], 4);
            // if(SI523_write_NTAG(12+4, cardpid) == MI_OK){
            //   ESP_LOGI(MMC_TAG, "NTAG write successful (bytes 4-7)");
            // }

            // memcpy(cardpid, &g_write_card_data[8], 4);
            // if(SI523_write_NTAG(12+8, cardpid) == MI_OK){
            //   ESP_LOGI(MMC_TAG, "NTAG write successful (bytes 8-11)");
            // }

            // memcpy(cardpid, &g_write_card_data[12], 4);
            // if(SI523_write_NTAG(12+12, cardpid) == MI_OK){
            //   ESP_LOGI(MMC_TAG, "NTAG write successful (bytes 12-15)");
            // }

            if(SI523_read_NTAG(12, cardpid) == MI_OK){
            ESP_LOGI(MMC_TAG, "NTAG0: %02X %02X %02X %02X", cardpid[0], cardpid[1], cardpid[2], cardpid[3]);
            ESP_LOGI(MMC_TAG, "NTAG4: %02X %02X %02X %02X", cardpid[4], cardpid[5], cardpid[6], cardpid[7]);
            ESP_LOGI(MMC_TAG, "NTAG8: %02X %02X %02X %02X", cardpid[8], cardpid[9], cardpid[10], cardpid[11]);
            ESP_LOGI(MMC_TAG, "NTAGC: %02X %02X %02X %02X", cardpid[12], cardpid[13], cardpid[14], cardpid[15]);

            // 返回读取到的数据到上位机
            char response[64];
            snprintf(response, sizeof(response), "Read: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                     cardpid[0], cardpid[1], cardpid[2], cardpid[3],
                     cardpid[4], cardpid[5], cardpid[6], cardpid[7],
                     cardpid[8], cardpid[9], cardpid[10], cardpid[11],
                     cardpid[12], cardpid[13], cardpid[14], cardpid[15]);
            send_upload_response(APP_ID_WRITE_CARD, (uint8_t*)response, strlen(response));
            ESP_LOGI(MMC_TAG, "Card data sent to host");
            break;
            }
        }}
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // 测量完成，下电节省电量
    ESP_LOGI(MMC_TAG, "Measurement completed, powering off devices...");
    // 释放SI523设备资源
    SI523_Deinit();
    vTaskDelay(pdMS_TO_TICKS(10));
    NPD_EN(0);

    // 清除任务运行标志，允许创建新任务
    g_task_running = 0;

    vTaskDelete( NULL );
}

void i2c0_mmc56x3_task( void *pvParameters ) {
    // initialize i2c device configuration
    mmc56x3_config_t dev_cfg       = I2C_MMC56X3_CONFIG_DEFAULT;
    mmc56x3_handle_t dev_hdl;
    //
    int status = 5;
        for(int i = 0; i < status; i++) {
            // 初始化MMC56X3
            mmc56x3_init(i2c0_bus_hdl, &dev_cfg, &dev_hdl);
            if (dev_hdl == NULL) {
                ESP_LOGE(MMC_TAG, "mmc56x3 handle init failed");
                vTaskDelay(pdMS_TO_TICKS(200));
                //发送复位命令
                mmc56x3_reset(i2c0_bus_hdl);
                assert(dev_hdl);
            }
                //mmc56x3_set_measure_mode(i2c0_bus_hdl, dev_hdl, false);

                //ESP_LOGI(MMC_TAG, "######################## MMC56X3 - START #########################");
                mmc56x3_magnetic_axes_data_t magnetic_axes;
                esp_err_t result = mmc56x3_get_magnetic_axes(dev_hdl, &magnetic_axes);
                if(result != ESP_OK) {
                    ESP_LOGE(MMC_TAG, "mmc56x3 device read failed (%s)", esp_err_to_name(result));
                } else {
                    //ESP_LOGI(MMC_TAG, "Compass X-Axis:  %f mG", magnetic_axes.x_axis);
                    //ESP_LOGI(MMC_TAG, "Compass Y-Axis:  %f mG", magnetic_axes.y_axis);
                    //ESP_LOGI(MMC_TAG, "Compass Z-Axis:  %f mG", magnetic_axes.z_axis);
                    Compass_Heading = (int)(mmc56x3_convert_to_heading(magnetic_axes));
                    ESP_LOGI(MMC_TAG, "Compass Heading: %d °", Compass_Heading);
                    True_Heading = (int)(mmc56x3_convert_to_true_heading((float)g_sys_params.comp_offset, magnetic_axes));
                    ESP_LOGI(MMC_TAG, "True Heading:    %d °", True_Heading);
                    // 成功读取一次数据后退出循环
                    break;
                }
        //ESP_LOGI(MMC_TAG, "######################## MMC56X3 - END ###########################");
        vTaskDelay(pdMS_TO_TICKS(200));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    

    // 释放MMC56X3资源
    mmc56x3_delete( dev_hdl );
    ESP_LOGI(MMC_TAG, "Task i2c0_mmc56x3_task completed");

    // 检查是否有待发送的指南针响应
    if(g_compending_response) {
        g_compending_response = false;  // 清除标记位
        char response[64];
        snprintf(response, sizeof(response), "Comp=%d\nTrue_Head=%d", Compass_Heading, True_Heading);
        send_upload_response(APP_ID_SYS_PARAM, (uint8_t*)response, strlen(response));
        ESP_LOGI(MMC_TAG, "Compass response sent: %s", response);
    }

    g_task_running=3;
    vTaskDelete( NULL );
}

void key_task( void *pvParameters )
{
    (void)pvParameters;
    int status = 5;
    unsigned char carduid[10];
    unsigned char cardpid[16];

    // 按键触发，开始测量指南针
    g_task_running=100;//超时
    xTaskCreatePinnedToCore(i2c0_mmc56x3_task,MMC_TASK_NAME,MMC_TASK_STACK_SIZE,NULL,MMC_TASK_PRIORITY,NULL,0);
    while(g_task_running>10){
        vTaskDelay(pdMS_TO_TICKS(100));
        g_task_running--;
    }
    ESP_LOGI(MMC_TAG, "Button pressed, powering on devices...");
    NPD_EN(1);// 上电并等待模块稳定
    vTaskDelay(pdMS_TO_TICKS(100));
    SI523_Init(i2c0_bus_hdl);// 初始化SI523
    vTaskDelay(pdMS_TO_TICKS(10)); // 等待10ms让SI523初始化完成
    for(int i = 0; i < status; i++) {    
        if(SI523_CheckVer() != 0){
        PCD_SI523_TypeA_Init();
        //PCD_SI523_TypeA();
        if(PCD_SI523_TypeA_GetUID(carduid)==0){
            if(SI523_read_NTAG(12, cardpid) == MI_OK){
            ESP_LOGI(MMC_TAG, "NTAG0: %02X %02X %02X %02X", cardpid[0], cardpid[1], cardpid[2], cardpid[3]);
            ESP_LOGI(MMC_TAG, "NTAG4: %02X %02X %02X %02X", cardpid[4], cardpid[5], cardpid[6], cardpid[7]);
            ESP_LOGI(MMC_TAG, "NTAG8: %02X %02X %02X %02X", cardpid[8], cardpid[9], cardpid[10], cardpid[11]);
            ESP_LOGI(MMC_TAG, "NTAGC: %02X %02X %02X %02X", cardpid[12], cardpid[13], cardpid[14], cardpid[15]);
            break;
            }            
        }}
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // 测量完成，下电节省电量
    ESP_LOGI(MMC_TAG, "Measurement completed, powering off devices...");
    // 释放SI523设备资源
    SI523_Deinit();
    vTaskDelay(pdMS_TO_TICKS(10));
    NPD_EN(0);
    
    // Compass_Heading True_Heading cardpid[16]
    // 处理NFC卡片数据
    process_nfc_data(carduid, cardpid);






    // 清除任务运行标志，允许创建新任务
    g_task_running = 0;

    vTaskDelete( NULL );
}





