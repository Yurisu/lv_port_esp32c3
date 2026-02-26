#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_num.h"
#include "soc/gpio_reg.h"
/* Function used to tell the linker to include this file
 * with all its symbols.
 */
void bootloader_hooks_include(void){
}


void bootloader_before_init(void) {
    /* Keep in my mind that a lot of functions cannot be called from here
     * as system initialization has not been performed yet, including
     * BSS, SPI flash, or memory protection. */
    ESP_LOGI("BL", "Init bootloader");
    //拉低PW——EN
    // gpio_ll_output_enable(GPIO_NUM_20);
    // gpio_ll_set_level(GPIO_NUM_20, 0);   


    // 1. 使用 esp_rom_gpio 将 GPIO20 配置为通用 GPIO
    esp_rom_gpio_pad_select_gpio(GPIO_NUM_20);
    
    // 2. 使用 REG_WRITE 设置为输出模式
    REG_WRITE(GPIO_ENABLE_W1TS_REG, (1 << GPIO_NUM_20));
    
    // 3. 使用 REG_WRITE 设置低电平（拉低 PW_EN）
    REG_WRITE(GPIO_OUT_W1TC_REG, (1 << GPIO_NUM_20));







// // 设置方向
// REG_WRITE(GPIO_ENABLE_W1TS_REG, (1 << gpio_num));  // 设置为输出
// REG_WRITE(GPIO_ENABLE_W1TC_REG, (1 << gpio_num));  // 设置为输入

// // 设置电平
// REG_WRITE(GPIO_OUT_W1TS_REG, (1 << gpio_num));     // 设置高电平
// REG_WRITE(GPIO_OUT_W1TC_REG, (1 << gpio_num));     // 设置低电平

// // 读取输入电平
// uint32_t level = REG_READ(GPIO_IN_REG);
// bool pin_level = (level >> gpio_num) & 0x01;

}
void bootloader_after_init(void) {
    ESP_LOGI("BL", "bootloader finished");
}