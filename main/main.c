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

#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "lv_port_fs.h"

#include <mmc56x3.h>
#include "SI523_App.h"


// 包含 LVGL demos（如果启用了的话）
#if LV_USE_DEMO_WIDGETS
    #include "demos/lv_demos.h"
#endif

#define I2C0_MASTER_PORT               I2C_NUM_0
#define I2C0_MASTER_SDA_IO             GPIO_NUM_9 // blue
#define I2C0_MASTER_SCL_IO             GPIO_NUM_8 // yellow

#define LED_4 6
#define LED_5 6
#define LOW_LEVEL 0
#define HIGH_LEVEL 1
static bool g_task_run = false;

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

_Noreturn void BlinkLed(void *params) {
  (void) params;
  uint8_t level = LOW_LEVEL;
  gpio_reset_pin(LED_4);
  gpio_set_direction(LED_4, GPIO_MODE_OUTPUT); // Set the GPIO as a push/pull output
  gpio_reset_pin(LED_5);
  gpio_set_direction(LED_5, GPIO_MODE_OUTPUT); // Set the GPIO as a push/pull output
  ESP_LOGI("BlinkLed", "LED configuration completed.");

  while (true) {
    gpio_set_level(LED_4, level);
    ESP_LOGI("BlinkLed", "LED_4: %s!", level == HIGH_LEVEL ? "ON" : "OFF");
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    level = !level;

    gpio_set_level(LED_5, level);
    ESP_LOGI("BlinkLed", "LED_5: %s!", level == HIGH_LEVEL ? "ON" : "OFF");
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
    unsigned char data[16];
    
    SI523_Init(i2c0_bus_hdl);

        // init device
    mmc56x3_init(i2c0_bus_hdl, &dev_cfg, &dev_hdl);
    if (dev_hdl == NULL) {
        ESP_LOGE(MMC_TAG, "mmc56x3 handle init failed");
        vTaskDelete(NULL);
        return;
    }
    //mmc56x3_set_measure_mode(i2c0_bus_hdl, dev_hdl, false);


  while (1)
  {
    g_task_run = false;
    while (!g_task_run) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    if(SI523_CheckVer() != 0){
      PCD_SI523_TypeA_Init();
      //PCD_SI523_TypeA();
      if(SI523_TypeA_GetUID(carduid)==1){
        if(SI523_read_NTAG(12, data) == MI_OK){
          ESP_LOGI(MMC_TAG, "NTAG: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);
        }
      //SI523_write_YURIDATA(void)== MI_OK
      //SI523_write_NTAG(unsigned char page, unsigned char *buffer) == MI_OK

    }}


    status = 10;
    // task loop entry point - 只执行一次测量
    for(int i = 0; i < status; i++) {
        //ESP_LOGI(MMC_TAG, "######################## MMC56X3 - START #########################");
        
        // 磁力测量复位校准
        ESP_LOGI(MMC_TAG, "Performing MMC56X3 calibration...");
        mmc56x3_magnetic_set_reset(dev_hdl);
        vTaskDelay(1000 / portTICK_PERIOD_MS); // 等待校准完成
        ESP_LOGI(MMC_TAG, "MMC56X3 calibration completed");
        // handle sensor
        mmc56x3_magnetic_axes_data_t magnetic_axes;
        esp_err_t result = mmc56x3_get_magnetic_axes(dev_hdl, &magnetic_axes);
        if(result != ESP_OK) {
            ESP_LOGE(MMC_TAG, "mmc56x3 device read failed (%s)", esp_err_to_name(result));
        } else {
            ESP_LOGI(MMC_TAG, "Compass X-Axis:  %f mG", magnetic_axes.x_axis);
            ESP_LOGI(MMC_TAG, "Compass Y-Axis:  %f mG", magnetic_axes.y_axis);
            ESP_LOGI(MMC_TAG, "Compass Z-Axis:  %f mG", magnetic_axes.z_axis);
            ESP_LOGI(MMC_TAG, "Compass Heading: %f °", mmc56x3_convert_to_heading(magnetic_axes));
            ESP_LOGI(MMC_TAG, "True Heading:    %f °", mmc56x3_convert_to_true_heading(dev_hdl->dev_config.declination, magnetic_axes));
            // 成功读取一次数据后退出循环
            break;
        }
        //
        //ESP_LOGI(MMC_TAG, "######################## MMC56X3 - END ###########################");
        // pause between attempts
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
  }
    //
    // free resources
    mmc56x3_delete( dev_hdl );
    ESP_LOGI(MMC_TAG, "Task i2c0_mmc56x3_task completed");
    vTaskDelete( NULL );
}





#define GPIO_INTERRUPT_PIN             GPIO_NUM_21
#define GPIO_INTERRUPT_TAG             "GPIO_ISR"

// GPIO interrupt handler
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint32_t gpio_num = (uint32_t) arg;
    
    // Send notification to task (optional, for debouncing or complex handling)
    // For simple logging, we can directly log here
    ESP_EARLY_LOGI(GPIO_INTERRUPT_TAG, "GPIO %ld interrupt triggered!", gpio_num);
    g_task_run = true;
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





























































_Noreturn void app_main(void) {

  IIC_init();
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
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }











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
        lv_label_set_text_fmt(time_label, "Uptime: %lus", uptime_seconds);
    }
    lv_task_handler();
  }


  free(buf1);
  free(buf2);
  vTaskDelete(NULL);
}




/*
void GC9A01_init(void)
{
	lcd_init_cmd_t GC_init_cmds[]={
////////////////////////////////////////////
		{0xEF, {0}, 0},
		{0xEB, {0x14}, 1},

		{0xFE, {0}, 0},
		{0xEF, {0}, 0},

		{0xB0, {0xC0}, 1},
		{0x84, {0x40}, 1},
		{0x85, {0xFF}, 1},
		{0x86, {0xFF}, 1},
		{0x87, {0xFF}, 1},
		{0x88, {0x0A}, 1},
		{0x89, {0x21}, 1},
		{0x8A, {0x00}, 1},
		{0x8B, {0x80}, 1},
		{0x8C, {0x01}, 1},
		{0x8D, {0x01}, 1},
		{0x8E, {0xFF}, 1},
		{0x8F, {0xFF}, 1},
		{0xB6, {0x00, 0x20}, 2},
		//call orientation
		{0x3A, {0x05}, 1},
		{0x90, {0x08, 0x08, 0X08, 0X08}, 4},
		{0xBD, {0x06}, 1},
		{0xBC, {0x00}, 1},
		{0xFF, {0x60, 0x01, 0x04}, 3},
		{0xC3, {0x13}, 1},
		{0xC4, {0x13}, 1},
		{0xC9, {0x22}, 1},
		{0xBE, {0x11}, 1},
		{0xE1, {0x10, 0x0E}, 2},
		{0xDF, {0x21, 0x0C, 0x02}, 3},
		{0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6},
		{0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6},
		{0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6},
		{0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6},
		{0xED, {0x1B, 0x0B}, 2},
		{0xAE, {0x77}, 1},
		{0xCD, {0x63}, 1},
		{0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0X08, 0x03}, 9},
		{0xE8, {0x34}, 1},
		{0x62, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0X0F, 0x71, 0xEF, 0x70, 0x70}, 12},
		{0x63, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0X13, 0x71, 0xF3, 0x70, 0x70}, 12},
		{0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7},
		{0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0X00, 0x00, 0x00}, 10},
		{0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0X10, 0x32, 0x98}, 10},
		{0x74, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7},
		{0x98, {0x3E, 0x07}, 2},
		{0x35, {0}, 0},
		{0x21, {0}, 0x80},
		{0x11, {0}, 0x80},	//0x80 delay flag
		{0x29, {0}, 0x80},	//0x80 delay flag
		{0, {0}, 0xff},		//init end flag
////////////////////////////////////////////
*/