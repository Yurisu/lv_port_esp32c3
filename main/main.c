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
#include "nvs_flash.h"

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
// HID报告配置
#define BATTERY_REPORT_ID 0x02
#define BATTERY_REPORT_SIZE 1
#define HIDD_DEVICE_NAME      "VSchess-"
char blerename[32];
uint8_t macAddr[6]; //蓝牙地址

static uint16_t hid_conn_id = 0;
static bool sec_conn = false;
static bool send_volum_up = false;
#define CHAR_DECLARATION_SIZE (sizeof(uint8_t))





static esp_ble_adv_data_t hidd_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x40, // slave connection min interval, Time = min_interval * 1.25 msec
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
    .adv_int_min = 0x40, //0.625,140=200ms
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

const char* hidden_msg = "专业软硬件开发,方案可出何必破解,价格合理,合作愉快,期待共赢, yuri_su@163.com , +8613580387577  ";
const char* hidden_msg2 = "Professional hardware and software solutions available for purchase.[yuri_su@163.com,+8613580387577] No need to break in — let's save the effort and share the rewards. Reasonable prices, pleasant cooperation, and mutual success await.";


// 包含 LVGL demos（如果启用了的话）
#if LV_USE_DEMO_WIDGETS
    #include "demos/lv_demos.h"
#endif

#define I2C0_MASTER_PORT               I2C_NUM_0
#define I2C0_MASTER_SDA_IO             GPIO_NUM_9
#define I2C0_MASTER_SCL_IO             GPIO_NUM_8 
#define NPD_EN_GPIO                    GPIO_NUM_10
#define LED_BG_GPIO                    GPIO_NUM_6


#define GPIO_INTERRUPT_PIN             GPIO_NUM_21
#define GPIO_INTERRUPT_TAG             "GPIO_ISR"

void NPD_EN(int state);
void BG_EN(int state);

#define LOW_LEVEL 0
#define HIGH_LEVEL 1
volatile bool g_task_run = false;

void lv_tick_task(void *arg) {
  (void) arg;
  lv_tick_inc(100);
}

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

// _Noreturn void BlinkLed(void *params) {
//   (void) params;
//   uint8_t level = LOW_LEVEL;
//   gpio_reset_pin(LED_4);
//   gpio_set_direction(LED_4, GPIO_MODE_OUTPUT); // Set the GPIO as a push/pull output
//   gpio_reset_pin(LED_5);
//   gpio_set_direction(LED_5, GPIO_MODE_OUTPUT); // Set the GPIO as a push/pull output
//   ESP_LOGI("BlinkLed", "LED configuration completed.");

//   while (true) {
//     gpio_set_level(LED_4, level);
//     ESP_LOGI("BlinkLed", "LED_4: %s!", level == HIGH_LEVEL ? "ON" : "OFF");
//     vTaskDelay(1000 / portTICK_PERIOD_MS);

//     level = !level;

//     gpio_set_level(LED_5, level);
//     ESP_LOGI("BlinkLed", "LED_5: %s!", level == HIGH_LEVEL ? "ON" : "OFF");
//     vTaskDelay(1000 / portTICK_PERIOD_MS);
//   }
// }














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




#define TSK_MINIMAL_STACK_SIZE         (1024)

#define MMC_TASK_NAME                 "mmc_task"
#define MMC_TASK_SAMPLING_RATE        (10000) 
#define MMC_TASK_STACK_SIZE           (TSK_MINIMAL_STACK_SIZE * 8)
#define MMC_TASK_PRIORITY             (tskIDLE_PRIORITY + 2)

#define MMC_TAG                         "MMC[APP]"




void i2c0_mmc56x3_task( void *pvParameters ) {
    // initialize the xLastWakeTime variable with the current time.
    TickType_t         last_wake_time  = xTaskGetTickCount ();
    // initialize i2c device configuration
    mmc56x3_config_t dev_cfg       = I2C_MMC56X3_CONFIG_DEFAULT;
    mmc56x3_handle_t dev_hdl;
    //
    int status = 10;
    unsigned char carduid[10];
    unsigned char cardpid[16];
    
    // 初始状态：保持下电
    NPD_EN(0);

  while (1)
  {
    g_task_run = false;
    while (!g_task_run) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    // 按键触发，开始上电初始化
    ESP_LOGI(MMC_TAG, "Button pressed, powering on devices...");
    
    // 上电并等待模块稳定
    NPD_EN(1);
    vTaskDelay(50 / portTICK_PERIOD_MS);  // 等待50ms让电源稳定
    
    // 初始化SI523
    SI523_Init(i2c0_bus_hdl);
    vTaskDelay(10 / portTICK_PERIOD_MS);  // 等待10ms让SI523初始化完成
    
    // 初始化MMC56X3
    mmc56x3_init(i2c0_bus_hdl, &dev_cfg, &dev_hdl);
    if (dev_hdl == NULL) {
        ESP_LOGE(MMC_TAG, "mmc56x3 handle init failed");
        NPD_EN(0);  // 初始化失败，下电
        assert(dev_hdl);
    }
    //mmc56x3_set_measure_mode(i2c0_bus_hdl, dev_hdl, false);

    if(SI523_CheckVer() != 0){
      PCD_SI523_TypeA_Init();
      //PCD_SI523_TypeA();
      if(PCD_SI523_TypeA_GetUID()==0){
        if(SI523_read_NTAG(12, cardpid) == MI_OK){
          ESP_LOGI(MMC_TAG, "NTAG: %02X %02X %02X %02X", cardpid[0], cardpid[1], cardpid[2], cardpid[3]);
        }
        // if(SI523_write_YURIDATA() == MI_OK){
        //   ESP_LOGI(MMC_TAG, "YURIDATA write successful");
        // }
        // memccpy(cardpid, "9876", 4, 4);
        // if(SI523_write_NTAG(12, cardpid) == MI_OK){
        //   ESP_LOGI(MMC_TAG, "NTAG write successful");
        // }

    }}


    status = 10;
    // task loop entry point - 只执行一次测量
    for(int i = 0; i < status; i++) {
        //ESP_LOGI(MMC_TAG, "######################## MMC56X3 - START #########################");
        mmc56x3_magnetic_axes_data_t magnetic_axes;
        esp_err_t result = mmc56x3_get_magnetic_axes(dev_hdl, &magnetic_axes);
        if(result != ESP_OK) {
            ESP_LOGE(MMC_TAG, "mmc56x3 device read failed (%s)", esp_err_to_name(result));
        } else {
            //ESP_LOGI(MMC_TAG, "Compass X-Axis:  %f mG", magnetic_axes.x_axis);
            //ESP_LOGI(MMC_TAG, "Compass Y-Axis:  %f mG", magnetic_axes.y_axis);
            //ESP_LOGI(MMC_TAG, "Compass Z-Axis:  %f mG", magnetic_axes.z_axis);
            //ESP_LOGI(MMC_TAG, "Compass Heading: %f °", mmc56x3_convert_to_heading(magnetic_axes));
            ESP_LOGI(MMC_TAG, "True Heading:    %d °", (int)(mmc56x3_convert_to_true_heading(dev_hdl->dev_config.declination, magnetic_axes)));
            // 成功读取一次数据后退出循环
            break;
        }
        //
        //ESP_LOGI(MMC_TAG, "######################## MMC56X3 - END ###########################");
        // pause between attempts
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
    
    // 测量完成，下电节省电量
    ESP_LOGI(MMC_TAG, "Measurement completed, powering off devices...");
    NPD_EN(0);
  }
    //
    // free resources
    mmc56x3_delete( dev_hdl );
    ESP_LOGI(MMC_TAG, "Task i2c0_mmc56x3_task completed");
    vTaskDelete( NULL );
}





// GPIO interrupt handler
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint32_t gpio_num = (uint32_t) arg;
    
    // Send notification to task (optional, for debouncing or complex handling)
    // For simple logging, we can directly log here
    if(!g_task_run) {
        ESP_EARLY_LOGI(GPIO_INTERRUPT_TAG, "GPIO %ld interrupt triggered!", gpio_num);
        g_task_run = true;
    }
    // 检查任务是否已存在，避免重复创建
    // TaskHandle_t task_handle = xTaskGetHandle(MMC_TASK_NAME);
    // if (task_handle == NULL) {
    //     xTaskCreatePinnedToCore(i2c0_mmc56x3_task, MMC_TASK_NAME, MMC_TASK_STACK_SIZE, NULL, MMC_TASK_PRIORITY, NULL, 0);
    // } else {
    //     ESP_EARLY_LOGI(GPIO_INTERRUPT_TAG, "Task %s already exists, skipping creation", MMC_TASK_NAME);
    // }
    
    // Clear the interrupt status
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


        esp_ble_conn_update_params_t conn_params = {0};
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        conn_params.latency = 0;
        conn_params.max_int = 0x100;//0x320;    // max_int = 0x20*1.25ms = 40ms
        conn_params.min_int = 0x20;    // min_int = 0x10*1.25ms = 20ms
        conn_params.timeout = 900;    // timeout = 400*10ms = 4000ms
        esp_ble_gap_update_conn_params(&conn_params);

        // 设置MTU为470
        esp_err_t mtu_ret = esp_ble_gatt_set_local_mtu(470);
        if (mtu_ret == ESP_OK) {
            ESP_LOGI("HIDevent", "MTU set to 470");
        } else {
            ESP_LOGE("HIDevent", "Failed to set MTU: %s", esp_err_to_name(mtu_ret));
        }

        break;
    }
    case ESP_HIDD_EVENT_BLE_DISCONNECT:
    {
        sec_conn = false;
        ESP_LOGI("HIDevent", "ESP_HIDD_EVENT_BLE_DISCONNECT");
        esp_ble_gap_start_advertising(&hidd_adv_params);
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
        // 协议解释：根据接收到的数据执行相应操作
        // 示例：第一个字节是命令类型
        if (param->nus_uart_rx.length >= 1) {
            uint8_t cmd = param->nus_uart_rx.data[0];
            ESP_LOGI("HIDevent", "NUS command: 0x%02X", cmd);
            
            // 根据命令类型处理数据
            switch(cmd) {
                case 0x01:  // 示例命令1
                    ESP_LOGI("HIDevent", "Command 0x01 received");
                    // 处理命令1
                    break;
                case 0x02:  // 示例命令2
                    ESP_LOGI("HIDevent", "Command 0x02 received");
                    // 处理命令2
                    break;
                default:
                    ESP_LOGW("HIDevent", "Unknown command: 0x%02X", cmd);
                    break;
            }
        }
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
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
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
    default:
        break;
    }
}


void hid_demo_task(void *pvParameters)
{
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    while (1)
    {
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        // 通过NUS发送数据到FFE1 (TX特征)
            
        if (sec_conn)
        {
            sec_conn=0;
            ESP_LOGI("HIDtask", "Send the volume");
            send_volum_up = true;
            // uint8_t key_vaule = {HID_KEY_A};
            // esp_hidd_send_keyboard_value(hid_conn_id, 0, &key_vaule, 1);
            esp_hidd_send_consumer_value(hid_conn_id, HID_CONSUMER_VOLUME_UP, true);
            
            
            
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            if (send_volum_up)
            {
                send_volum_up = false;
                esp_hidd_send_consumer_value(hid_conn_id, HID_CONSUMER_VOLUME_UP, false);
                esp_hidd_send_consumer_value(hid_conn_id, HID_CONSUMER_VOLUME_DOWN, true);
                vTaskDelay(3000 / portTICK_PERIOD_MS);
                esp_hidd_send_consumer_value(hid_conn_id, HID_CONSUMER_VOLUME_DOWN, false);
            }
        }
    }
}























































_Noreturn void app_main(void) {

  IIC_init();
  npd_gpio_init();
  gpio_interrupt_init();

  // 自动创建任务，按键触发
  xTaskCreatePinnedToCore(i2c0_mmc56x3_task,MMC_TASK_NAME,MMC_TASK_STACK_SIZE,NULL,MMC_TASK_PRIORITY,NULL,0);

  xTaskCreate(PrintChipInfo, "PrintChipInfo", 1024 * 4, NULL, 1, NULL);
  //xTaskCreate(BlinkLed, "BlinkLed", 1024 * 4, NULL, 1, NULL);
  fflush(stdout);
  {
    TaskHandle_t print_chip_info_handle = xTaskGetHandle("PrintChipInfo");
    if (print_chip_info_handle != NULL) {
      vTaskDelete(print_chip_info_handle);
      ESP_LOGI("app_main", "Task PrintChipInfo delete.");
    }
  }
  // while (1) {
     vTaskDelay(pdMS_TO_TICKS(1000));
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
      esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND; // bonding with peer device after authentication
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

      xTaskCreate(&hid_demo_task, "hid_task", 4096, NULL, 5, NULL);
    }






     vTaskDelay(pdMS_TO_TICKS(1000));





  /**
   * \brief Start LVGL demo.
   */
  //BG_EN(1);
  lv_init();
  lvgl_driver_init();
  lv_port_fs_init();

  lv_color_t *buf1 =
      heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
  assert(buf1 != NULL);
  /* Use double buffered when not working with monochrome displays */
#ifndef CONFIG_LV_TFT_DISPLAY_MONOCHROME
  lv_color_t *buf2 = heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
  assert(buf2 != NULL);
#else
  static lv_color_t *buf2 = NULL;
#endif
  static lv_disp_draw_buf_t disp_buf;
  uint32_t size_in_px = DISP_BUF_SIZE;
  lv_disp_draw_buf_init(&disp_buf, buf1, buf2, size_in_px);
  lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = CONFIG_LV_HOR_RES_MAX;
  disp_drv.ver_res = CONFIG_LV_VER_RES_MAX;
  disp_drv.flush_cb = disp_driver_flush;
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

  const esp_timer_create_args_t periodic_timer_args = {
      .callback = &lv_tick_task, .name = "screen"};
  esp_timer_handle_t periodic_timer;
  ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 1000));

  ESP_LOGI(__FILENAME__, "Free Heap Size: %lu", esp_get_minimum_free_heap_size());


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

    // 创建全屏背景图片
    lv_obj_t *bg_img = lv_img_create(lv_scr_act());

    // 从 LittleFS 加载背景图片
    lv_img_set_src(bg_img, "A:/1.sjpg");
    const void *src = lv_img_get_src(bg_img);
    lv_obj_set_size(bg_img, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(bg_img);

    // 创建显示开机时间的标签
    static lv_obj_t *time_label;
    time_label = lv_label_create(lv_scr_act());
    if (time_label != NULL) {
        lv_label_set_text(time_label, "Uptime: 0s");
        lv_obj_set_style_text_color(time_label, lv_color_black(), 0);
        lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);//14/26/38
        lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 20);
    }





  uint32_t uptime_seconds = 0;

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 更新开机时间显示
    uptime_seconds++;
    if (time_label != NULL) {

        uint8_t nus_data[15] = "";
        sprintf((char*)nus_data, "Uptime: %lu", uptime_seconds);
        // 检查notify是否已启用
        if (notifyEN()) {
        esp_err_t ret = nus_uart_send_data(hid_conn_id, nus_data, strlen((char*)nus_data));
            if (ret == ESP_OK) {
                ESP_LOGI("HIDtask", "NUS data sent successfully");
            } else {
                ESP_LOGE("HIDtask", "NUS data send failed: %s", esp_err_to_name(ret));
            }
        }
        lv_label_set_text_fmt(time_label, "Uptime: %lus", uptime_seconds);
    }
    lv_task_handler();
  }


  free(buf1);
  free(buf2);
  vTaskDelete(NULL);
}




// 将JPG数据写入文件系统
void write_jpg_to_fs(const uint8_t *data, size_t length) {
    lv_fs_file_t file;
    if (lv_fs_open(&file, "A:/received.jpg", LV_FS_MODE_WR) != LV_FS_RES_OK) {
        ESP_LOGE("FS", "无法打开文件进行写入");
        return;
    }

    uint32_t written;
    if (lv_fs_write(&file, data, length, &written) != LV_FS_RES_OK || written != length) {
        ESP_LOGE("FS", "文件写入失败或不完整");
    } else {
        ESP_LOGI("FS", "文件写入成功，总字节数: %ld", written);
    }

    lv_fs_close(&file);
}