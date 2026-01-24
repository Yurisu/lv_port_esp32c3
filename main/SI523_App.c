#include "SI523_App.h"
#include <math.h>
#include <esp_log.h>
#include <esp_check.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

//***********************************//修改新增内容

extern uint8_t PCD_IRQ_flagA ;
unsigned char ACDConfigRegK_Val ;
unsigned char ACDConfigRegC_Val ;




// External I2C bus handle from main.c
extern i2c_master_bus_handle_t i2c0_bus_hdl;

// SI523-specific delays (renamed from MMC56X3)
#define SI523_POWERUP_DELAY_MS          UINT16_C(50)
#define SI523_APPSTART_DELAY_MS         UINT16_C(10)           //!< SI523 I2C delay in milliseconds app-start
#define SI523_RESET_DELAY_MS            UINT16_C(50)           //!< SI523 I2C delay in milliseconds after reset
#define SI523_SETRESET_DELAY_MS         UINT16_C(1)            //!< SI523 I2C delay in milliseconds after set-reset transaction
#define SI523_WRITE_DELAY_MS            UINT16_C(1)            //!< SI523 I2C delay in milliseconds after write transaction
#define SI523_DATA_READY_DELAY_MS       UINT16_C(1)            //!< SI523 1ms when checking data ready in a loop
#define SI523_DATA_POLL_TIMEOUT_MS      UINT16_C(100)          //!< SI523 100ms timeout when making a measurement
#define SI523_CMD_DELAY_MS              UINT16_C(5)
#define SI523_TX_RX_DELAY_MS            UINT16_C(10)
#define I2C_XFR_TIMEOUT_MS      (500)          //!< I2C transaction timeout in milliseconds

// Global handle variable for SI523
typedef struct {
    i2c_master_dev_handle_t i2c_handle;
    SI523_config_t dev_config;
} SI523_internal_handle_t;

static SI523_internal_handle_t *g_si523_handle = NULL;


static const char *TAG = "SI523";




esp_err_t SI523_IIC_Init(i2c_master_bus_handle_t master_handle, const SI523_config_t *SI523_config, SI523_handle_t *SI523_handle) {
    /* validate arguments */
    if (!master_handle || !SI523_config) {
        return ESP_ERR_INVALID_ARG;
    }

    /* delay task before next i2c transaction */
    vTaskDelay(pdMS_TO_TICKS(10));
	ESP_LOGI(TAG, "Initializing SI523 handle");

    /* validate device exists on the master bus */
    esp_err_t ret = i2c_master_probe(master_handle, SI523_config->i2c_address, I2C_XFR_TIMEOUT_MS);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "device does not exist at address 0x%02x, SI523 device handle initialization failed", SI523_config->i2c_address);

    /* validate data rate if continuous mode is enabled */
    if(SI523_config->continuous_mode_enabled == true) {
        ESP_GOTO_ON_FALSE(SI523_config->data_rate > 0, ESP_ERR_INVALID_ARG, err, TAG, "data rate (odr) must be non-zero in continuous measurement mode, init failed");
    }

    /* validate memory availability for handle */
    SI523_internal_handle_t *out_handle;
    out_handle = (SI523_internal_handle_t *)calloc(1, sizeof(*out_handle));
    ESP_GOTO_ON_FALSE(out_handle, ESP_ERR_NO_MEM, err, TAG, "no memory for i2c SI523 device, init failed");

    /* copy configuration */
    out_handle->dev_config = *SI523_config;

    /* set device configuration */
    const i2c_device_config_t i2c_dev_conf = {
        .dev_addr_length    = I2C_ADDR_BIT_LEN_7,
        .device_address     = out_handle->dev_config.i2c_address,
        .scl_speed_hz       = out_handle->dev_config.i2c_clock_speed,
    };

    /* add device to I2C bus */
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(master_handle, &i2c_dev_conf, &out_handle->i2c_handle), err_handle, TAG, "i2c new bus for init failed");

    /* delay task before next i2c transaction */
    vTaskDelay(pdMS_TO_TICKS(SI523_CMD_DELAY_MS));

    /* store global handle */
    g_si523_handle = out_handle;
    
    /* set device handle */
    *SI523_handle = (SI523_handle_t)out_handle;

    /* delay task before next i2c transaction */
    vTaskDelay(pdMS_TO_TICKS(SI523_APPSTART_DELAY_MS));

    return ESP_OK;

err_handle:
    if (out_handle && out_handle->i2c_handle) {
        i2c_master_bus_rm_device(out_handle->i2c_handle);
    }
    free(out_handle);
err:
    return ret;
}





/**
 * @brief Write a byte to SI523 register
 * @param RegAddr Register address to write to
 * @param value Value to write
 */
void I_SI523_IO_Write(unsigned char RegAddr, unsigned char value)
{
    /* validate global handle */
    if (g_si523_handle == NULL) {
        ESP_LOGE(TAG, "SI523 handle not initialized");
        return;
    }

    /* prepare transmit buffer */
    uint8_t tx_data[2] = {RegAddr, value};

    /* attempt i2c write transaction */
    esp_err_t ret = i2c_master_transmit(g_si523_handle->i2c_handle, tx_data, 2, I2C_XFR_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed: reg=0x%02X, val=0x%02X", RegAddr, value);
    return;
}

    /* delay after write */
    //vTaskDelay(pdMS_TO_TICKS(SI523_WRITE_DELAY_MS));
    
}
/**
 * @brief Read a byte from SI523 register
 * @param RegAddr Register address to read from
 * @return Register value (0-255) on success, 0 on error
 */
unsigned char I_SI523_IO_Read(unsigned char RegAddr)
{
    /* validate global handle */
    if (g_si523_handle == NULL) {
        ESP_LOGE(TAG, "SI523 handle not initialized");
        return 0;
    }

    /* prepare buffers */
    uint8_t tx_data[1] = {RegAddr};
    uint8_t rx_data[1] = {0};

    /* attempt i2c transaction */
    esp_err_t ret = i2c_master_transmit_receive(g_si523_handle->i2c_handle, 
                                               tx_data, 1, 
                                               rx_data, 1, 
                                               I2C_XFR_TIMEOUT_MS);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed: reg=0x%02X", RegAddr);
        return 0;
    }
    
    /* delay after read */
    //vTaskDelay(pdMS_TO_TICKS(SI523_TX_RX_DELAY_MS));
	//ESP_LOGI(TAG, "I2C read success: reg=0x%02X, val=0x%02X", RegAddr, rx_data[0]);
    return rx_data[0];
}


/////////////////////////////////////////////////////////////////////
//功    能：初始化SI523设备
//参数说明: master_handle[IN]:I2C总线句柄
////////////////////////////////////////////////////////////////////
void SI523_Init(i2c_master_bus_handle_t master_handle)
{
    /* Initialize SI523 with default configuration */
    SI523_config_t si523_config = I2C_SI523_CONFIG_DEFAULT;
    SI523_handle_t si523_handle = NULL;

    esp_err_t ret = SI523_IIC_Init(master_handle, &si523_config, &si523_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SI523_IIC_Init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "SI523 initialized successfully at address 0x%02x", si523_config.i2c_address);
}

/////////////////////////////////////////////////////////////////////
//功    能：释放SI523设备资源
//返    回: ESP_OK on success
////////////////////////////////////////////////////////////////////
esp_err_t SI523_Deinit(void)
{
    /* validate global handle */
    if (g_si523_handle == NULL) {
        ESP_LOGE(TAG, "SI523 handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* remove device from i2c master bus */
    if (g_si523_handle->i2c_handle != NULL) {
        esp_err_t ret = i2c_master_bus_rm_device(g_si523_handle->i2c_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to remove SI523 from I2C bus: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* free handle memory */
    free(g_si523_handle);
    g_si523_handle = NULL;

    ESP_LOGI(TAG, "SI523 deinitialized successfully");

    return ESP_OK;
}








/////////////////////////////////////////////////////////////////////
//开启天线
//每次启动或关闭天险发射之间应至少有1ms的间隔
/////////////////////////////////////////////////////////////////////

void PcdAntennaOn(void)
{
    unsigned char i;
    i = I_SI523_IO_Read(TxControlReg);
    if (!(i & 0x03))
    {
        I_SI523_SetBitMask(TxControlReg, 0x03);
    }
}

/////////////////////////////////////////////////////////////////////
//关闭天线
/////////////////////////////////////////////////////////////////////
void PcdAntennaOff(void)
{
	I_SI523_ClearBitMask(TxControlReg, 0x03);
}

/////////////////////////////////////////////////////////////////////
//用MF522计算CRC16函数
/////////////////////////////////////////////////////////////////////
char CalulateCRC(unsigned char *pIndata, unsigned char len, unsigned char *pOutData)
{
	unsigned int i;
	unsigned char n;

	// 停止当前命令，清除中断
	I_SI523_IO_Write(CommandReg, PCD_IDLE);
	I_SI523_ClearBitMask(DivIrqReg, 0x04);
	I_SI523_SetBitMask(FIFOLevelReg, 0x80);

	// 写入数据到FIFO
	for (i = 0; i < len; i++)
	{
		I_SI523_IO_Write(FIFODataReg, *(pIndata + i));
	}

	// 开始CRC计算
	I_SI523_IO_Write(CommandReg, PCD_CALCCRC);

	// 带超时的等待循环（约50ms超时）
	for (i = 5000; i > 0; i--)
	{
		n = I_SI523_IO_Read(DivIrqReg);
		if (n & 0x04)
		{											// CRCIRq位设置，计算完成
			I_SI523_IO_Write(CommandReg, PCD_IDLE); // 停止计算
			pOutData[0] = I_SI523_IO_Read(CRCResultRegL);
			pOutData[1] = I_SI523_IO_Read(CRCResultRegH);
			ESP_LOGD(TAG, "CRC calculation success: CRCResultRegL=0x%02X, CRCResultRegH=0x%02X", pOutData[0], pOutData[1]);
			return MI_OK;
		}
	}

	// 超时处理
	ESP_LOGE(TAG, "CRC calculation timeout");
	return MI_ERR;
}

unsigned char aaa = 0;


/////////////////////////////////////////////////////////////////////
//功    能：通过RC522和ISO14443卡通讯
//参数说明：Command[IN]:RC522命令字
//          pInData[IN]:通过RC522发送到卡片的数据
//          InLenByte[IN]:发送数据的字节长度
//          pOutData[OUT]:接收到的卡片返回数据
//          *pOutLenBit[OUT]:返回数据的位长度
/////////////////////////////////////////////////////////////////////
//status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,1,ucComMF522Buf,&unLen);
char PcdComMF522(unsigned char Command, 
                 unsigned char *pInData, 
                 unsigned char InLenByte,
                 unsigned char *pOutData, 
                 unsigned int *pOutLenBit)
{
    char status = MI_ERR;
    unsigned char irqEn   = 0x00;
    unsigned char waitFor = 0x00;
    unsigned char lastBits;
    unsigned char n;
    unsigned int i;
    switch (Command)
    {
        case PCD_AUTHENT:
			irqEn   = 0x12;
			waitFor = 0x10;
			break;
		case PCD_TRANSCEIVE:
			irqEn   = 0x77;
			waitFor = 0x30;
			break;
		default:
			break;
    }
   
    //I_SI523_IO_Write(ComIEnReg,irqEn|0x80);
    I_SI523_ClearBitMask(ComIrqReg,0x80);
    I_SI523_IO_Write(CommandReg,PCD_IDLE);
    I_SI523_SetBitMask(FIFOLevelReg,0x80);
    
    for (i=0; i<InLenByte; i++)
    {   
		I_SI523_IO_Write(FIFODataReg, pInData[i]);    
	}
    I_SI523_IO_Write(CommandReg, Command);
   
    if (Command == PCD_TRANSCEIVE)
    {    
		I_SI523_SetBitMask(BitFramingReg,0x80);  
	}
    
    //i = 600;//根据时钟频率调整，操作M1卡最大等待时间25ms
	i = 2000;
    do 
    {
        n = I_SI523_IO_Read(ComIrqReg);
        i--;
    }
    while ((i!=0) && !(n&0x01) && !(n&waitFor));
    I_SI523_ClearBitMask(BitFramingReg,0x80);

    if (i!=0)
    {   
		aaa = I_SI523_IO_Read(ErrorReg);
		
        if(!(I_SI523_IO_Read(ErrorReg)&0x1B))
        {
            status = MI_OK;
            if (n & irqEn & 0x01)
            {   status = MI_NOTAGERR;   }
            if (Command == PCD_TRANSCEIVE)
            {
               	n = I_SI523_IO_Read(FIFOLevelReg);
              	lastBits = I_SI523_IO_Read(ControlReg) & 0x07;
                if (lastBits)
                {   
					*pOutLenBit = (n-1)*8 + lastBits;   
				}
                else
                {   
					*pOutLenBit = n*8;   
				}
                if (n == 0)
                {   
					n = 1;    
				}
                if (n > MAXRLEN)
                {   
					n = MAXRLEN;   
				}
                for (i=0; i<n; i++)
                {   
					pOutData[i] = I_SI523_IO_Read(FIFODataReg);    
				}
            }
        }
        else
        {   
			status = MI_ERR;   
		}
        
    }
   
    I_SI523_SetBitMask(ControlReg,0x80);           // stop timer now
    I_SI523_IO_Write(CommandReg,PCD_IDLE); 
	ESP_LOGD(TAG, "PcdComMF522: status=%d, ErrorReg=%02X", status, I_SI523_IO_Read(ErrorReg));
    return status;
}
                     
/////////////////////////////////////////////////////////////////////
//功    能：寻卡
//参数说明: req_code[IN]:寻卡方式
//                0x52 = 寻感应区内所有符合14443A标准的卡
//                0x26 = 寻未进入休眠状态的卡
//          pTagType[OUT]：卡片类型代码
//                0x4400 = Mifare_UltraLight
//                0x0400 = Mifare_One(S50)
//                0x0200 = Mifare_One(S70)
//                0x0800 = Mifare_Pro(X)
//                0x4403 = Mifare_DESFire
//返    回: 成功返回MI_OK
/////////////////////////////////////////////////////////////////////
char PcdRequest(unsigned char req_code,unsigned char *pTagType)
{
	char status;  
	unsigned int unLen;
	unsigned char ucComMF522Buf[MAXRLEN]; 

	I_SI523_ClearBitMask(Status2Reg,0x08);
	I_SI523_IO_Write(BitFramingReg,0x07);
	I_SI523_SetBitMask(TxControlReg,0x03);
 
	ucComMF522Buf[0] = req_code;

	status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,1,ucComMF522Buf,&unLen);
	if ((status == MI_OK) && (unLen == 0x10))
	{    
		*pTagType     = ucComMF522Buf[0];
		*(pTagType+1) = ucComMF522Buf[1];
        ESP_LOGD(TAG, "Card detected: ATQA=%02X%02X", ucComMF522Buf[0], ucComMF522Buf[1]);
	}
	else
	{   
		status = MI_ERR;   
        ESP_LOGD(TAG, "No card detected");
	}
   
	return status;
}


/////////////////////////////////////////////////////////////////////
//功    能：防冲撞
//参数说明: pSnr[OUT]:卡片序列号，4字节
//返    回: 成功返回MI_OK
/////////////////////////////////////////////////////////////////////  
char PcdAnticoll(unsigned char *pSnr, unsigned char anticollision_level)
{
    char status;
    unsigned char i,snr_check=0;
    unsigned int unLen;
    unsigned char ucComMF522Buf[MAXRLEN]; 
    

    I_SI523_ClearBitMask(Status2Reg,0x08);
    I_SI523_IO_Write(BitFramingReg,0x00);
    I_SI523_ClearBitMask(CollReg,0x80);
 
    ucComMF522Buf[0] = anticollision_level;
    ucComMF522Buf[1] = 0x20;

    status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,2,ucComMF522Buf,&unLen);

    if (status == MI_OK)
	{
		for (i=0; i<4; i++)
		{   
			*(pSnr+i)  = ucComMF522Buf[i];
			snr_check ^= ucComMF522Buf[i];
		}
		if (snr_check != ucComMF522Buf[i])
   		{   
			status = MI_ERR;   
            ESP_LOGE(TAG, "UID checksum error");
		}
        else
        {
            ESP_LOGD(TAG, "UID: %02X%02X%02X%02X", pSnr[0], pSnr[1], pSnr[2], pSnr[3]);
        }
    }
    else
    {
        ESP_LOGE(TAG, "Anticollision failed");
    }
    
    I_SI523_SetBitMask(CollReg,0x80);
    return status;
}


/////////////////////////////////////////////////////////////////////
//功    能：选定卡片
//参数说明: pSnr[IN]:卡片序列号，4字节
//返    回: 成功返回MI_OK
////////////////////////////////////////////////////////////////////
char PcdSelect (unsigned char * pSnr, unsigned char *sak)
{
    char status;
    unsigned char i;
    unsigned int unLen;
    unsigned char ucComMF522Buf[MAXRLEN]; 
    
    ucComMF522Buf[0] = PICC_ANTICOLL1;
    ucComMF522Buf[1] = 0x70;
    ucComMF522Buf[6] = 0;
    for (i=0; i<4; i++)
    {
    	ucComMF522Buf[i+2] = *(pSnr+i);
    	ucComMF522Buf[6]  ^= *(pSnr+i);
    }
    CalulateCRC(ucComMF522Buf,7,&ucComMF522Buf[7]);                                                                      
  
    I_SI523_ClearBitMask(Status2Reg,0x08);

    status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,9,ucComMF522Buf,&unLen);
    
    if ((status == MI_OK) && (unLen == 0x18))
    {   
		*sak = ucComMF522Buf[0];
		status = MI_OK;  
        ESP_LOGD(TAG, "Card selected: SAK=0x%02X", *sak);
	}
    else
    {   
		status = MI_ERR;   
        ESP_LOGE(TAG, "Card selection failed");
	}

    return status;
}

/////////////////////////////////////////////////////////////////////
//功    能：选定卡片（Level 1 - 4字节UID）
//参数说明: pSnr[IN]:卡片序列号，4字节
//          sak[OUT]:SAK字节，用于判断卡片类型
//返    回: 成功返回MI_OK
////////////////////////////////////////////////////////////////////
char PcdSelect1 (unsigned char * pSnr, unsigned char *sak)
{
    char status;
    unsigned char i;
    unsigned int unLen;
    unsigned char ucComMF522Buf[MAXRLEN]; 
    
    ucComMF522Buf[0] = PICC_ANTICOLL1;
    ucComMF522Buf[1] = 0x70;
    ucComMF522Buf[6] = 0;
    for (i=0; i<4; i++)
    {
    	ucComMF522Buf[i+2] = *(pSnr+i);
    	ucComMF522Buf[6]  ^= *(pSnr+i);
    }
    CalulateCRC(ucComMF522Buf,7,&ucComMF522Buf[7]);
  
    I_SI523_ClearBitMask(Status2Reg,0x08);

    status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,9,ucComMF522Buf,&unLen);
    
    if ((status == MI_OK) && (unLen == 0x18))
    {   
		*sak = ucComMF522Buf[0];
		status = MI_OK;  
        ESP_LOGD(TAG, "Card selected: SAK=0x%02X", *sak);
	}
    else
    {   
		status = MI_ERR;   
        ESP_LOGE(TAG, "Card selection failed");
	}

    return status;
}

/////////////////////////////////////////////////////////////////////
//功    能：选定卡片（Level 2 - 7字节UID）
//参数说明: pSnr[IN]:卡片序列号后4字节
//          sak[OUT]:SAK字节，用于判断卡片类型
//返    回: 成功返回MI_OK
////////////////////////////////////////////////////////////////////
char PcdSelect2 (unsigned char * pSnr, unsigned char *sak)
{
    char status;
    unsigned char i;
    unsigned int unLen;
    unsigned char ucComMF522Buf[MAXRLEN]; 
    
    ucComMF522Buf[0] = PICC_ANTICOLL2;
    ucComMF522Buf[1] = 0x70;
    ucComMF522Buf[6] = 0;
    for (i=0; i<4; i++)
    {
    	ucComMF522Buf[i+2] = *(pSnr+i);
    	ucComMF522Buf[6]  ^= *(pSnr+i);
    }
    CalulateCRC(ucComMF522Buf,7,&ucComMF522Buf[7]);
  
    I_SI523_ClearBitMask(Status2Reg,0x08);

    status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,9,ucComMF522Buf,&unLen);
    
    if ((status == MI_OK) && (unLen == 0x18))
    {   
		*sak = ucComMF522Buf[0];
		status = MI_OK;  
        ESP_LOGD(TAG, "Card selected: SAK=0x%02X", *sak);
	}
    else
    {   
		status = MI_ERR;   
        ESP_LOGE(TAG, "Card selection failed");
	}

    return status;
}

/////////////////////////////////////////////////////////////////////
//功    能：选定卡片（Level 3 - 10字节UID）
//参数说明: pSnr[IN]:卡片序列号后4字节
//          sak[OUT]:SAK字节，用于判断卡片类型
//返    回: 成功返回MI_OK
////////////////////////////////////////////////////////////////////
char PcdSelect3 (unsigned char * pSnr, unsigned char *sak)
{
    char status;
    unsigned char i;
    unsigned int unLen;
    unsigned char ucComMF522Buf[MAXRLEN]; 
    
    ucComMF522Buf[0] = PICC_ANTICOLL2;
    ucComMF522Buf[1] = 0x70;
    ucComMF522Buf[6] = 0;
    for (i=0; i<4; i++)
    {
    	ucComMF522Buf[i+2] = *(pSnr+i);
    	ucComMF522Buf[6]  ^= *(pSnr+i);
    }
    CalulateCRC(ucComMF522Buf,7,&ucComMF522Buf[7]);
  
    I_SI523_ClearBitMask(Status2Reg,0x08);

    status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,9,ucComMF522Buf,&unLen);
    
    if ((status == MI_OK) && (unLen == 0x18))
    {   
		*sak = ucComMF522Buf[0];
		status = MI_OK;  
        ESP_LOGD(TAG, "Card selected: SAK=0x%02X", *sak);
	}
    else
    {   
		status = MI_ERR;   
        ESP_LOGE(TAG, "Card selection failed");
	}

    return status;
}

/////////////////////////////////////////////////////////////////////
//功    能：命令卡片进入休眠状态
//返    回: 成功返回MI_OK
/////////////////////////////////////////////////////////////////////
char PcdHalt(void)
{
    char status;
    unsigned int unLen;
    unsigned char ucComMF522Buf[MAXRLEN]; 

    ucComMF522Buf[0] = PICC_HALT;
    ucComMF522Buf[1] = 0;
    CalulateCRC(ucComMF522Buf,2,&ucComMF522Buf[2]);
 
    status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,4,ucComMF522Buf,&unLen);

    return status;
}

/////////////////////////////////////////////////////////////////////
//功    能：验证卡片密码
//参数说明: auth_mode[IN]: 密码验证模式
//                 0x60 = 验证A密钥
//                 0x61 = 验证B密钥
//          addr[IN]：块地址
//          pKey[IN]：密码
//          pSnr[IN]：卡片序列号，4字节
//返    回: 成功返回MI_OK
/////////////////////////////////////////////////////////////////////               
char PcdAuthState(unsigned char auth_mode,unsigned char addr,unsigned char *pKey,unsigned char *pSnr)
{
    char status;
    unsigned int unLen;
    unsigned char ucComMF522Buf[MAXRLEN]; 

    ucComMF522Buf[0] = auth_mode;
    ucComMF522Buf[1] = addr;
	memcpy(&ucComMF522Buf[2], pKey, 6); 
	memcpy(&ucComMF522Buf[8], pSnr, 6); 
    
    status = PcdComMF522(PCD_AUTHENT,ucComMF522Buf,12,ucComMF522Buf,&unLen);
    if ((status != MI_OK) || (!(I_SI523_IO_Read(Status2Reg) & 0x08)))
    {
		status = MI_ERR;   
        ESP_LOGE(TAG, "Auth failed: mode=0x%02X, addr=%02X", auth_mode, addr);
	}
    else
    {
        ESP_LOGD(TAG, "Auth ok: mode=0x%02X, addr=%02X", auth_mode, addr);
    }
    
    return status;
}



/////////////////////////////////////////////////////////////////////
//功    能：读取M1卡一块数据
//参数说明: addr[IN]：块地址
//          pData[OUT]：读出的数据，16字节
//返    回: 成功返回MI_OK
///////////////////////////////////////////////////////////////////// 
char PcdRead(unsigned char addr,unsigned char *pData)
{
    char status;
    unsigned int unLen;
    unsigned char ucComMF522Buf[MAXRLEN]; 

    ucComMF522Buf[0] = PICC_READ;
    ucComMF522Buf[1] = addr;
    CalulateCRC(ucComMF522Buf,2,&ucComMF522Buf[2]);
   
    status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,4,ucComMF522Buf,&unLen);
    if ((status == MI_OK) && (unLen == 0x90))
   	{   
		memcpy(pData, ucComMF522Buf, 16);   
        ESP_LOGD(TAG, "Read block %02X: %02X%02X%02X%02X...", 
                 addr, pData[0], pData[1], pData[2], pData[3]);
	}
    else
    {   
		status = MI_ERR;   
        ESP_LOGE(TAG, "Read block %02X failed", addr);
	}
    
    return status;
}

/////////////////////////////////////////////////////////////////////
//功    能：写数据到M1卡一块
//参数说明: addr[IN]：块地址
//          pData[IN]：写入的数据，16字节
//返    回: 成功返回MI_OK
/////////////////////////////////////////////////////////////////////                  
char PcdWrite(unsigned char addr,unsigned char *pData)
{
    char status;
    unsigned int unLen;
    unsigned char ucComMF522Buf[MAXRLEN]; 
    
    ESP_LOGD(TAG, "Write block %02X: %02X%02X%02X%02X...", 
             addr, pData[0], pData[1], pData[2], pData[3]);
    
    ucComMF522Buf[0] = PICC_WRITE;
    ucComMF522Buf[1] = addr;
    CalulateCRC(ucComMF522Buf,2,&ucComMF522Buf[2]);
 
    status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,4,ucComMF522Buf,&unLen);

    if ((status != MI_OK) || (unLen != 4) || ((ucComMF522Buf[0] & 0x0F) != 0x0A))
    {   
		status = MI_ERR;   
        ESP_LOGE(TAG, "Write block %02X failed", addr);
	}
        
    if (status == MI_OK)
    {
        memcpy(ucComMF522Buf, pData, 16);
        CalulateCRC(ucComMF522Buf,16,&ucComMF522Buf[16]);

        status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,18,ucComMF522Buf,&unLen);
        if ((status != MI_OK) || (unLen != 4) || ((ucComMF522Buf[0] & 0x0F) != 0x0A))
        {   
			status = MI_ERR;   
		}
    }
    
    return status;
}



/*===============================
 函数功能：读A卡初始化配置

 ================================*/
void PCD_SI523_TypeA_Init(void)
{
	
	I_SI523_ClearBitMask(Status2Reg, 0x08);  
	// Reset baud rates
	I_SI523_IO_Write(TxModeReg, 0x00);
	I_SI523_IO_Write(RxModeReg, 0x00);
	// Reset ModWidthReg
	I_SI523_IO_Write(ModWidthReg, 0x26);
	// RxGain:110,43dB by default;
	I_SI523_IO_Write(RFCfgReg, RFCfgReg_Val);
	// When communicating with a PICC we need a timeout if something goes wrong.
	// f_timer = 13.56 MHz / (2*TPreScaler+1) where TPreScaler = [TPrescaler_Hi:TPrescaler_Lo].
	// TPrescaler_Hi are the four low bits in TModeReg. TPrescaler_Lo is TPrescalerReg.
	I_SI523_IO_Write(TModeReg, 0x80);// TAuto=1; timer starts automatically at the end of the transmission in all communication modes at all speeds
	I_SI523_IO_Write(TPrescalerReg, 0xa9);// TPreScaler = TModeReg[3..0]:TPrescalerReg
	I_SI523_IO_Write(TReloadRegH, 0x03); // Reload timer 
	I_SI523_IO_Write(TReloadRegL, 0xe8); // Reload timer 
	I_SI523_IO_Write(TxASKReg, 0x40);	// Default 0x00. Force a 100 % ASK modulation independent of the ModGsPReg register setting
	I_SI523_IO_Write(ModeReg, 0x3D);	// Default 0x3F. Set the preset value for the CRC coprocessor for the CalcCRC command to 0x6363 (ISO 14443-3 part 6.2.4)
	I_SI523_IO_Write(CommandReg, 0x00);  // Turn on the analog part of receiver   

	PcdAntennaOn();
}


/*===============================
 函数功能：读A卡

 ================================*/
char PCD_SI523_TypeA_GetUID(unsigned char *carduid)
{
	unsigned char ATQA[2];
	unsigned char UID[12];
	unsigned char SAK = 0;
	unsigned char UID_complate1 = 0;
	unsigned char UID_complate2 = 0;

//	printf("\r\nTest_SI523_GetUID");
	I_SI523_IO_Write(RFCfgReg, RFCfgReg_Val); //复位接收增益
	
	//寻卡
	if( PcdRequest( PICC_REQIDL, ATQA) != MI_OK )  //寻天线区内未进入休眠状态的卡，返回卡片类型 2字节
	{
		I_SI523_IO_Write(RFCfgReg, 0x48);
		if(PcdRequest( PICC_REQIDL, ATQA) != MI_OK)
		{
			I_SI523_IO_Write(RFCfgReg, 0x58);
			if(PcdRequest( PICC_REQIDL, ATQA) != MI_OK)
			{	
				ESP_LOGD(TAG, "Request failed");
				return 1;
			}
			else
			{
				ESP_LOGD(TAG, "Request1 ok: ATQA=%02X%02X", ATQA[0],ATQA[1]);
			}	
		}
		else
		{
			ESP_LOGD(TAG, "Request2 ok: ATQA=%02X%02X", ATQA[0],ATQA[1]);
		}		
	}
	else
	{
		ESP_LOGD(TAG, "Request3 ok: ATQA=%02X%02X", ATQA[0],ATQA[1]);
	}
	
	
//UID长度=4
	//Anticoll 冲突检测 level1
	if(PcdAnticoll(UID, PICC_ANTICOLL1)!= MI_OK) 
	{
		ESP_LOGD(TAG, "Anticoll1 failed");
		return 1;
	}
	else
	{
		//选定卡片
		if(PcdSelect1(UID,&SAK)!= MI_OK)
		{
			ESP_LOGD(TAG, "Select1 failed");
			return 1;
		}
		else
		{
			ESP_LOGD(TAG, "Select1 ok: SAK=%02X", SAK);
			if(SAK&0x04)                         
			{
				UID_complate1 = 0;
				
				//UID长度=7
				if(UID_complate1 == 0)    
				{
					//Anticoll 冲突检测 level2
					if(PcdAnticoll(UID+4, PICC_ANTICOLL2)!= MI_OK) 
					{
						ESP_LOGD(TAG, "Anticoll2 failed");
						return 1;
					}
					else
					{
						if(PcdSelect2(UID+4,&SAK)!= MI_OK)  
						{
							ESP_LOGD(TAG, "Select2 failed");
							return 1;
						}
						else
						{
							ESP_LOGD(TAG, "Select2 ok: SAK=%02X", SAK);
							if(SAK&0x04)                         
							{
								UID_complate2 = 0;
								
								//UID长度=10
								if(UID_complate2 == 0)     
								{
									//Anticoll 冲突检测 level3
									if(PcdAnticoll(UID+8, PICC_ANTICOLL3)!= MI_OK) 
									{
										ESP_LOGD(TAG, "Anticoll3 failed");
										return 1;
									}
									else
									{
										if(PcdSelect3(UID+8,&SAK)!= MI_OK)  
										{
											ESP_LOGD(TAG, "Select3 failed");
											return 1;
										}
										else
										{
											ESP_LOGD(TAG, "Select3 ok: SAK=%02X", SAK);
											if(SAK&0x04)                          
											{
												//UID_complate3
											}
											else 
											{
												ESP_LOGD(TAG, "GetUID: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
												UID[1],UID[2],UID[3],UID[5],UID[6],UID[7],UID[8],UID[9],UID[10],UID[11]);									
											}					
										}							
									}
								}
							}
							else 
							{
								UID_complate2 = 1;                  
								ESP_LOGD(TAG, "GetUID: %02X%02X%02X%02X%02X%02X%02X",
								UID[1],UID[2],UID[3],UID[4],UID[5],UID[6],UID[7]);
							}	
						}			
					}
				}
			}
			else 
			{
				UID_complate1 = 1;                   
				ESP_LOGD(TAG, "GetUID: %02X%02X%02X%02X", UID[0],UID[1],UID[2],UID[3]);
			}
		}		
	}
	//Halt
//	if(PcdHalt() != MI_OK)
//	{
//		printf("\r\nHalt:fail");
//		return 1;		
//	}
//	else
//	{
//		printf("\r\nHalt:ok");
//	}	
	
	//DelayUs(100);
	return 0;
}


/*===============================
 函数功能：读A卡扇区

 ================================*/
char PCD_SI523_TypeA_rw_block(void)
{
	unsigned char ATQA[2];
	unsigned char UID[12];
	unsigned char SAK = 0;
	unsigned char CardReadBuf[16] = {0};
	//unsigned char CardWriteBuf[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
	unsigned char DefaultKeyABuf[10] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

	// printf("\r\n\r\nTest_Si522_GetCard");
	ESP_LOGD(TAG, "Test_Si522_GetCard");
	
	//request 寻卡
	if( PcdRequest( PICC_REQIDL, ATQA) != MI_OK )  //寻天线区内未进入休眠状态的卡，返回卡片类型 2字节
	{
		// printf("\r\nRequest:fail");
		ESP_LOGI(TAG, "Request:fail");
		return 1;
	}
	else
	{
		// printf("\r\nRequest:ok  ATQA:%02x %02x",ATQA[0],ATQA[1]);
		ESP_LOGI(TAG, "Request:ok  ATQA=%02X %02X", ATQA[0], ATQA[1]);
	}
	

	//Anticoll 冲突检测
	if(PcdAnticoll(UID, PICC_ANTICOLL1)!= MI_OK)
	{
		// printf("\r\nAnticoll:fail");
		ESP_LOGI(TAG, "Anticoll:fail");
		return 1;
	}
	else
	{
		// printf("\r\nAnticoll:ok  UID:%02x %02x %02x %02x",UID[0],UID[1],UID[2],UID[3]);
		ESP_LOGI(TAG, "Anticoll:ok  UID=%02X %02X %02X %02X", UID[0], UID[1], UID[2], UID[3]);
	}
	
	//Select 选卡
	if(PcdSelect1(UID,&SAK)!= MI_OK)
	{
		// printf("\r\nSelect:fail");
		ESP_LOGI(TAG, "Select:fail");
		return 1;
	}
	else
	{
		// printf("\r\nSelect:ok  SAK:%02x",SAK);
		ESP_LOGI(TAG, "Select:ok  SAK=%02X", SAK);
	}

	//Authenticate 验证密码
	//unsigned char ackbuf[2] = {0};
	if(PcdAuthState( PICC_AUTHENT1B, 4, DefaultKeyABuf, UID ) != MI_OK )
	//if(PcdNTAG216_Auth(DefaultKeyABuf,ackbuf  ) != MI_OK )
	{
		// printf("\r\nAuthenticate:fail");
		ESP_LOGI(TAG, "Authenticate:fail");
		return 1;
	}
	else
	{
		// printf("\r\nAuthenticate:ok");
		ESP_LOGI(TAG, "Authenticate:ok");
	}

	//读BLOCK原始数据
	if( PcdRead( 4, CardReadBuf ) != MI_OK )
	{
		// printf("\r\nPcdRead:fail");
		ESP_LOGI(TAG, "PcdRead:fail");
		return 1;
	}
	else
	{
		// printf("\r\nPcdRead:ok  ");
		ESP_LOGI(TAG, "PcdRead:ok");
		for(unsigned char i=0;i<16;i++)
		{
			// printf(" %02x",CardReadBuf[i]);
		}
	}

	//产生随机数
	// for(unsigned char i=0;i<16;i++)
	// 	CardWriteBuf[i] = rand();

	// //写BLOCK 写入新的数据
	// if( PcdWrite( 4, CardWriteBuf ) != MI_OK )
	// {
	// 	// printf("\r\nPcdWrite:fail");
	// 	ESP_LOGI(TAG, "PcdWrite:fail");
	// 	return 1;
	// }
	// else
	// {
	// 	// printf("\r\nPcdWrite:ok  ");
	// 	ESP_LOGI(TAG, "PcdWrite:ok");
	// 	for(unsigned char i=0;i<16;i++)
	// 	{
	// 		// printf(" %02x",CardWriteBuf[i]);
	// 	}
	// }
		
	//读BLOCK 读出新写入的数据		
	PICC_DumpMifareUltralightToLog();
	if( PcdRead( 0, CardReadBuf ) != MI_OK )
	{
		// printf("\r\nPcdRead:fail");
		ESP_LOGI(TAG, "PcdRead:fail");
		return 1;
	}
	else
	{
		// printf("\r\nPcdRead:ok  ");
		ESP_LOGI(TAG, "PcdRead:ok");
		for(unsigned char i=0;i<16;i++)
		{
			// printf(" %02x",CardReadBuf[i]);
		}
	}

		
//	//Halt
//	if(PcdHalt() != MI_OK)
//	{
//		printf("\r\nHalt:fail");
//		return 1;		
//	}
//	else
//	{
//		printf("\r\nHalt:ok");
//	}	
	
	return 0;
}


/*===============================
 函数功能：读B卡初始化配置

 ================================*/
void PCD_SI523_TypeB_Init(void)
{
	
	I_SI523_ClearBitMask(Status2Reg, 0x08);
	I_SI523_IO_Write(ModeReg, 0x3F);  // For 0xFFFF crc
	I_SI523_IO_Write(TReloadRegL, 30);
	I_SI523_IO_Write(TReloadRegH, 0);
	I_SI523_IO_Write(TModeReg, 0x8D);
	I_SI523_IO_Write(TPrescalerReg, 0x3E); 
	I_SI523_IO_Write(TxASKReg, 0);  // Force 100ASK = 0//		DelayMs(100);
	I_SI523_IO_Write(GsNReg, 0xff);  // TX输出电导设置f8 fa N
	I_SI523_IO_Write(CWGsPReg, 0x3f);	 // P_改变1的幅度
	I_SI523_IO_Write(ModGsPReg, 0x07);  // 调制指数设置RegModGsp,, TYPEB ModConductance 0x1A P_改变0的幅度
	I_SI523_IO_Write(TxModeReg, 0x83);  // 编码器设置,106kbps,14443B 03
	I_SI523_IO_Write(BitFramingReg, 0x00);   // 调制脉宽,0x13->2.95us RegTypeBFraming ,,TYPEB
	I_SI523_IO_Write(AutoTestReg, 0x00);   
	// 低二位为接收增益，
	// 00,10,20,30,40,50,60,70
	// 18,23,18,23,33,38,43,48dB
	I_SI523_IO_Write(RFCfgReg, RFCfgReg_Val);          
	I_SI523_IO_Write(RxModeReg, 0x83);                 
	I_SI523_IO_Write(RxThresholdReg, 0x65);          
	I_SI523_ClearBitMask(RxSelReg,0x3F);
	I_SI523_SetBitMask(RxSelReg, 0x08);
	I_SI523_ClearBitMask(TxModeReg, 0x80);   // 无CRC,无奇偶校验
	I_SI523_ClearBitMask(RxModeReg, 0x80);
	I_SI523_ClearBitMask(Status2Reg, 0x08);   // MFCrypto1On =0			

	PcdAntennaOn();
}


/*=================================
 函数功能：循环读取A卡UID

=================================*/
void PCD_SI523_TypeA(void)
{
	unsigned char carduid[10];
	ESP_LOGI(TAG, "Starting TypeA card reading loop");
	while(1)
	{
	    unsigned char version = I_SI523_IO_Read(VersionReg);
	    ESP_LOGI(TAG, "IC Version: 0x%02X", version);
		PCD_SI523_TypeA_GetUID(carduid);//读取UID

	//产生随机数
	// for(unsigned char i=0;i<16;i++){
	// 	CardWriteBuf[i] = (unsigned char)(rand()&0x0A) + 0x30;
	// }

		PICC_DumpMifareUltralightToLog();
		vTaskDelay(pdMS_TO_TICKS(10000)); //500ms
	}
}
char SI523_read_NTAG(unsigned char page, unsigned char *buffer)
{
	ESP_LOGI(TAG, "Starting NTAG card reading");
	return PcdRead(page, buffer);
}

//每次写入一页,4字节,Yuri数据存12页
char SI523_write_NTAG(unsigned char page, unsigned char *buffer)
{
	return PcdWrite(page, buffer);
}
char SI523_write_YURIDATA(void)
{
	//unsigned char CardWriteBuf[16] = {1,3,0xa7,0xc,0x34,0x3,0xb,0xd1,0x0,0x7,0x54,0x2,0x7a,0x68,0x34,0x33};
	unsigned char CardWriteBuf1[32] = {\
		0x01,0x03,0xa7,0x0c,\
		0x34,0x03,0x14,0xd1,\
		0x01,0x10,0x55,0x00,\
		0x79,0x75,0x72,0x69,\
	//};
	//unsigned char CardWriteBuf2[16] = {
		0x5f,0x73,0x75,0x40,\
		0x31,0x36,0x33,0x2e,\
		0x63,0x6f,0x6d,0xfe,\
		0x6d,0xfe,0x00,0x00\
	};
	for(unsigned char i=0;i<8;i++){
		//写04 BLOCK 写入新的数据,每次4个字节
		if( PcdWrite( i+4, CardWriteBuf1+(i*4) ) != MI_OK )
		{
			ESP_LOGI(TAG, "PcdWrite:fail");
			return MI_ERR;
		}
	}
	return MI_OK;
}
/*================================
 函数功能：循环读取B卡UID

=================================*/
void PCD_SI523_TypeB(void)
{
	ESP_LOGI(TAG, "Starting TypeB card reading loop");
	while(1)
	{
		PCD_SI523_TypeB_GetUID();		//读B卡
		
		//PCD_SI523_IdentityCard_GetUID();		//读身份证

		
		vTaskDelay(pdMS_TO_TICKS(I2C_XFR_TIMEOUT_MS));//500ms
	}
}


/////////////////////////////////////////////////////////////////////
//功    能：读取Type B卡UID
//返    回: 成功返回0，失败返回1
////////////////////////////////////////////////////////////////////
char PCD_SI523_TypeB_GetUID(void)
{
	ESP_LOGI(TAG, "Test TypeB GetUID");
	
	//I_SI523_IO_Write(0x02, 0xa0); //打开接收中断,则读卡会产生中断
	// Enable external interrupt
	//EXTI->IMR |= 0x00000008;
	
	//request 寻B卡;返回卡号
	unsigned int    len1;
	unsigned char 	buf1[18] = {0x05,0x00,0x00,0x71,0xFF};
	
	if(PcdComMF522(PCD_TRANSCEIVE, buf1, 5, buf1, &len1) != MI_OK)
	{
		ESP_LOGI(TAG, "TypeB Request failed");
		return 1;		
	}
	else
	{	
		if( buf1[0] == 0x50 ) //判断是不是ATQB
		ESP_LOGI(TAG, "TypeB UID: %02X%02X%02X%02X", buf1[1],buf1[2],buf1[3],buf1[4]);
	}	
	
	return 0;
}



/////////////////////////////////////////////////////////////////////
//功    能：读取二代身份证UID
//返    回: 成功返回0，失败返回1
////////////////////////////////////////////////////////////////////
char PCD_SI523_IdentityCard_GetUID(void)
{
	ESP_LOGI(TAG, "Test IdentityCard GetUID");
	
	//request 寻B卡
    unsigned int 		len1;
	unsigned char 	buf1[18] = {0x05,0x00,0x00,0x71,0xFF};

	if(PcdComMF522(PCD_TRANSCEIVE, buf1, 5, buf1, &len1) != MI_OK)
	{
		ESP_LOGE(TAG, "IdentityCard Request failed");
		return 1;
	}
	
	//I_SI523_IO_Write(0x02, 0xa0); //打开接收中断,则读卡会产生中断
	// Enable external interrupt
	//EXTI->IMR |= 0x00000008;
	
	//发送二代证非标ATTRIB指令
	unsigned int 		len2;
	unsigned char 	buf2[18] = {0x1D,   0x00,0x00,0x00,0x00,   0x00,  0x08,  0x01,  0x08,  0xF3,  0x10};		
	if(PcdComMF522(PCD_TRANSCEIVE, buf2, 11, buf2, &len2) != MI_OK)
	{
		ESP_LOGE(TAG, "ATTRIB failed");
		return 1;		
	}	
		
	//获取UID
	unsigned int 		len3;
	unsigned char 	buf3[18] = {0x00,0x36,0x00,0x00, 0x08,0x57,0x44};	

	if(PcdComMF522(PCD_TRANSCEIVE, buf3, 7, buf3, &len3) != MI_OK)
	{
		ESP_LOGE(TAG, "UID failed");
		return 1;		
	}	
	else
	{	
		if( buf3[8] == 0x90||buf3[9] == 0x00 ) //判断是不是identitycard
			ESP_LOGI(TAG, "IdentityCard UID: %02X%02X%02X%02X%02X%02X%02X%02X",
								buf3[0],buf3[1],buf3[2],buf3[3],buf3[4],buf3[5],buf3[6],buf3[7]);
	}
	
	return 0;
}




//***********************************//修改新增内容

/*
 * 函数名：PcdReset
 * 描述  ：复位RC522
 * 输入  ：无
 * 返回  : 无
 * 调用  ：外部调用
 */
void PcdReset ( void )
{
	//hard reset
//	HAL_GPIO_WritePin(S52_NRSTPD_GPIO_Port,S52_NRSTPD_Pin,GPIO_PIN_RESET);
//	vTaskDelay(pdMS_TO_TICKS(1));
//	HAL_GPIO_WritePin(S52_NRSTPD_GPIO_Port,S52_NRSTPD_Pin,GPIO_PIN_SET);
//	vTaskDelay(pdMS_TO_TICKS(1));
	
	I_SI523_IO_Write(CommandReg, 0x0f);			//向CommandReg 写入 0x0f	作用是使RC522复位
	while(I_SI523_IO_Read(CommandReg) & 0x10 );	//Powerdown位为0时，表示RC522已准备好
	vTaskDelay(pdMS_TO_TICKS(1));
}

//void Pcd_Hard_Reset(void)
//{
//	GPIO_ResetBits( GPIOA , GPIO_Pin_2 );    // NPDREST 引脚，MCU需要设置上拉输出
//	vTaskDelay(pdMS_TO_TICKS(2));
//	GPIO_SetBits( GPIOA , GPIO_Pin_2 );
//	vTaskDelay(pdMS_TO_TICKS(2));
//}


//SI523_interfaces
/////////////////////////////////////////////////////////////////////
//功    能：清除寄存器的特定位
//参数说明: reg[IN]:寄存器地址
//          mask[IN]:要清除的位掩码
////////////////////////////////////////////////////////////////////
void I_SI523_ClearBitMask(unsigned char reg,unsigned char mask)
{
	char tmp = 0x00;
	tmp = I_SI523_IO_Read(reg);
	I_SI523_IO_Write(reg, tmp & ~mask);  // clear bit mask
} 

/////////////////////////////////////////////////////////////////////
//功    能：设置寄存器的特定位
//参数说明: reg[IN]:寄存器地址
//          mask[IN]:要设置的位掩码
////////////////////////////////////////////////////////////////////
void I_SI523_SetBitMask(unsigned char reg,unsigned char mask)
{
	char tmp = 0x00;
	tmp = I_SI523_IO_Read(reg);
	I_SI523_IO_Write(reg,tmp | mask);  // set bit mask
}

/////////////////////////////////////////////////////////////////////
//功    能：修改寄存器的特定位
//参数说明: RegAddr[IN]:寄存器地址
//          ModifyVal[IN]:1表示置位，0表示清零
//          MaskByte[IN]:要修改的位掩码
////////////////////////////////////////////////////////////////////
void I_SI523_SiModifyReg(unsigned char RegAddr, unsigned char ModifyVal, unsigned char MaskByte)
{
	unsigned char RegVal;
	RegVal = I_SI523_IO_Read(RegAddr);
	if(ModifyVal)
	{
			RegVal |= MaskByte;
	}
	else
	{
			RegVal &= (~MaskByte);
	}
	I_SI523_IO_Write(RegAddr, RegVal);
}


/*===============================
 函数功能：ACD模式初始化配置

 ================================*/
void ACD_init_Fun(void)
{
	PCD_SI523_TypeA_Init();	 //读A卡初始化配置
	
	PCD_ACD_AutoCalc();      //自动获取阈值
	
	PCD_ACD_Init();          //ACD初始化配置
}


/*===============================
 函数功能：ACD寻卡

 ================================*/
//void ACD_Fun(void)
//{
//	EXTI->IMR |= 0x00000008;	// Enable external interrupt
//	PCD_IRQ_flagA = 0;	//clear IRQ flag
//	ledGreenShining();	// LED indicator
//
//	while(1)
//	{
//		if(PCD_IRQ_flagA)
//		{
//			printf("\r\n\r\n\r\n PCD_IRQ_flagA");
//			EXTI->IMR &= 0xFFFFFFF7;		// Disable external interrupt
//
//			switch( PCD_IRQ() )
//			{
//				case 0:	//Other_IRQ
//					//printf("Other IRQ Occur\r\n");
//					PCD_SI523_TypeA_GetUID();
//					PcdReset();			//软复位
//					//PcdPowerdown();			//硬复位
//					PCD_SI523_TypeA_Init();
//					PCD_ACD_Init();
//					break;
//
//				case 1:	//ACD_IRQ
//					I_SI523_SiModifyReg(0x01, 0, 0x20);	// Turn on the analog part of receiver
//					PCD_SI523_TypeA_GetUID();
//
//					I_SI523_IO_Write(CommandReg, 0xb0);	 	//进入软掉电,重新进入ACD（ALPPL）
//				break;
//
//				case 2:	//ACDTIMER_IRQ
//					printf("ACDTIMER_IRQ:Reconfigure the register \r\n");
//					PcdReset();			//软复位
//					//PcdPowerdown();		//硬复位
//					PCD_SI523_TypeA_Init();
//					PCD_ACD_Init();
//					break;
//
//			}
//
//			EXTI->IMR |= 0x00000008;		// Enable external interrupt
//			PCD_IRQ_flagA = 0;
//		}
//		else
//		{
//			DelayMs(500);
//		}
//	}
//}

/*===============================
 函数功能：自动获取阈值

 ================================*/
void PCD_ACD_AutoCalc(void)
{
	unsigned char temp; 
	unsigned char temp_Compare=0; 
	unsigned char VCON_TR[8]={ 0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};//acd灵敏度调节
	unsigned char TR_Compare[4]={ 0x00, 0x00, 0x00, 0x00};
	ACDConfigRegC_Val = 0x7f;
	unsigned char	ACDConfigRegK_RealVal = 0;
	
	I_SI523_IO_Write(TxControlReg, 0x83);	//打开天线
	I_SI523_SetBitMask(CommandReg, 0x06);	//开启ADC_EXCUTE
	vTaskDelay(pdMS_TO_TICKS(1));
	
	for(int i=7; i>0; i--)
	{	
		I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigK << 2) | 0x40);		
		I_SI523_IO_Write(ACDConfigReg, VCON_TR[i]);
		
		I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigG << 2) | 0x40);
		temp_Compare = I_SI523_IO_Read(ACDConfigReg);
		for(int m=0;m<100;m++)
		{
			I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigG << 2) | 0x40);		
			temp = I_SI523_IO_Read(ACDConfigReg);
			
				if(	temp	==	0) 	break;          //处在接近的VCON值附近值，如果偶合出现0值，均有概率误触发，应舍弃该值。
			
			temp_Compare=(temp_Compare+temp)/2;		
			vTaskDelay(pdMS_TO_TICKS(1));//DelayUs(100);
		}		
		
		if(temp_Compare == 0 || temp_Compare == 0x7f) //比较当前值和所存值
		{

		}
		else
		{
			if(temp_Compare < ACDConfigRegC_Val)
			{
				ACDConfigRegC_Val = temp_Compare;
				ACDConfigRegK_Val = VCON_TR[i];
			}
		}
	}//取得最接近的参考电压VCON
	
	ACDConfigRegK_RealVal	=	ACDConfigRegK_Val;     //取得最接近的参考电压VCON
	
	for(int j=0; j<4; j++)
	{
		I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigK << 2) | 0x40);		
		I_SI523_IO_Write(ACDConfigReg, j*32+ACDConfigRegK_Val);
		
		I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigG << 2) | 0x40);
		temp_Compare = I_SI523_IO_Read(ACDConfigReg);
		for(int n=0;n<100;n++)
		{
			I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigG << 2) | 0x40);		
			temp = I_SI523_IO_Read(ACDConfigReg);
			temp_Compare=(temp_Compare+temp)/2;		
			vTaskDelay(pdMS_TO_TICKS(1));//DelayUs(100);
		}		
		TR_Compare[j] = temp_Compare;
	}//再调TR的档位，将采集值填入TR_Compare[]

	for(int z=0; z<3; z++)
	{
		if(TR_Compare[z] == 0x7f)
		{
			
		}
		else
		{
			ACDConfigRegC_Val = TR_Compare[z];//最终选择的配置
			ACDConfigRegK_Val = ACDConfigRegK_RealVal + z*32;
		}
	}//再选出一个非7f大值
	

	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigK << 2) | 0x40);
	// printf("\r\n ACDConfigRegK_Val:%02x ",ACDConfigRegK_Val);
	ESP_LOGI(TAG, "ACDConfigRegK_Val=%02X", ACDConfigRegK_Val);
	
	I_SI523_SetBitMask(CommandReg, 0x06);		//关闭ADC_EXCUTE
}



/*===============================
 函数功能：ACD初始化配置

 ================================*/
void PCD_ACD_Init(void)
{
	I_SI523_IO_Write(DivIrqReg, 0x60);	////清中断，该处不清中断，进入ACD模式后会异常产生有卡中断。
	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigJ << 2) | 0x40);		
	I_SI523_IO_Write(ACDConfigReg, 0x55);	//Clear ACC_IRQ
	
	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigA << 2) | 0x40);						//设置轮询时间
	I_SI523_IO_Write(ACDConfigReg, ACDConfigRegA_Val );
	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigB << 2) | 0x40);						//设置相对模式或者绝对模式
	I_SI523_IO_Write(ACDConfigReg, ACDConfigRegB_Val );
	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigC << 2) | 0x40);						//设置无卡场强值
	I_SI523_IO_Write(ACDConfigReg, ACDConfigRegC_Val );
	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigD << 2) | 0x40);						//设置灵敏度，一般建议为4，在调试时，可以适当降低验证该值，验证ACD功能
	I_SI523_IO_Write(ACDConfigReg, ACDConfigRegD_Val );
	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigH << 2) | 0x40);						//设置看门狗定时器时间
	I_SI523_IO_Write(ACDConfigReg, ACDConfigRegH_Val );
	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigI << 2) | 0x40);						 //设置ARI功能，在天线场强打开前1us产生ARI电平控制触摸芯片Si12T的硬件屏蔽引脚SCT
	I_SI523_IO_Write(ACDConfigReg, ACDConfigRegI_Val );	
	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigK << 2) | 0x40);					//设置ADC的基准电压和放大增益
	I_SI523_IO_Write(ACDConfigReg, ACDConfigRegK_Val );
	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigM << 2) | 0x40);					//设置监测ACD功能是否产生场强，意外产生可能导致读卡芯片复位或者寄存器丢失
	I_SI523_IO_Write(ACDConfigReg, ACDConfigRegM_Val );
	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigO << 2) | 0x40);					//设置ACD模式下相关功能的标志位传导到IRQ引脚
	I_SI523_IO_Write(ACDConfigReg, ACDConfigRegO_Val );
	
	I_SI523_IO_Write(ComIEnReg, ComIEnReg_Val);														//ComIEnReg，DivIEnReg   设置IRQ选择上升沿或者下降沿
	I_SI523_IO_Write(DivIEnReg, DivIEnReg_Val);
	
	I_SI523_IO_Write(ACDConfigSelReg, (ACDConfigJ << 2) | 0x40);				  //设置监测ACD功能下的重要寄存器的配置值，寄存器丢失后会立即产生中断
	I_SI523_IO_Write(ACDConfigReg, ACDConfigRegJ_Val );								// 写非0x55的值即开启功能，写0x55清除使能停止功能。
	
	I_SI523_IO_Write(CommandReg, 0xb0);	//进入ACD
}

/////////////////////////////////////////////////////////////////////
//功    能：读取并清除SI523中断状态
//返    回: 0=无中断, 1=ACD中断, 2=ACD看门狗中断
////////////////////////////////////////////////////////////////////
char PCD_IRQ(void)
{
	unsigned char status_SI523CD_IRQ;
	unsigned char temp_SI523CD_IRQ; 
	
	temp_SI523CD_IRQ = I_SI523_IO_Read(DivIrqReg);
	if	( temp_SI523CD_IRQ & 0x40)	//ACD中断
	{
		I_SI523_IO_Write(DivIrqReg, 0x40);		//Clear ACDIRq
		
		status_SI523CD_IRQ =1;
		return status_SI523CD_IRQ;
	}
	
	if ( temp_SI523CD_IRQ & 0x20)	//ACD看门狗中断
	{
		I_SI523_IO_Write(DivIrqReg, 0x20);		//Clear ACDTIMER_IRQ
		
		status_SI523CD_IRQ = 2;
		return status_SI523CD_IRQ;
	}
	
	I_SI523_IO_Write(DivIrqReg, 0x40);		//Clear ACDIRq
	I_SI523_IO_Write(DivIrqReg, 0x20);		//Clear ACDTIMER_IRQ
	I_SI523_IO_Write(0x20, (0x0f << 2) | 0x40);		
	I_SI523_IO_Write(0x0f, 0x0a);	//Clear OSCMon_IRQ,RFLowDetect_IRQ
	I_SI523_IO_Write(0x20, (0x09 << 2) | 0x40);		
	I_SI523_IO_Write(0x0f, 0x55);	//Clear ACC_IRQ

	return status_SI523CD_IRQ = 0;

}

/////////////////////////////////////////////////////////////////////
//功    能：读取并打印SI523芯片版本号
////////////////////////////////////////////////////////////////////
unsigned char SI523_CheckVer( void )
{
    unsigned char version = I_SI523_IO_Read(VersionReg);
    ESP_LOGD(TAG, "IC Version: 0x%02X", version);
	return version;
}

static unsigned char Flag_Detected_IC = 0;
static unsigned char SelectedSnr[4];



/////////////////////////////////////////////////////////////////////
//功    能：检查并检测卡片（带重试机制）
//返    回: 1=卡片状态改变, 0=无变化
////////////////////////////////////////////////////////////////////
unsigned char CR_CheckInt()
{
    unsigned char ATQA[2];
    unsigned char UID[12];
    unsigned char CardDetected = 0;
    unsigned char status = 0;
    unsigned char retry  = 5;
    unsigned char SAK    = 0;

	ESP_LOGI(TAG, "Card detection check started");
    while( retry-- )
    {
		ESP_LOGI(TAG, "Card detection retry: %d", retry);
        status = PcdRequest( PICC_REQIDL, ATQA );        // 寻卡
        if( !status )
        {
            status = PcdAnticoll( UID, PICC_ANTICOLL1 );      // 冲突检测
            if( !status )
            {
                if( PcdSelect1( UID, &SAK ) == MI_OK )
                {
                    if( ( SAK & 0x04 ) == 0 )
                    {
                        for( unsigned char count=0; count<4; count++ )
                        {
                            SelectedSnr[count] = UID[count];
                        }

                        CardDetected = 1;
                        ESP_LOGI(TAG, "Card detected: %02X%02X%02X%02X", UID[0], UID[1], UID[2], UID[3]);
                        break;
                    }
                }


                CardDetected = 1;
                break;
            }
        }
    }

    if( retry == 0 )
    {
        CardDetected = 0;
        ESP_LOGI(TAG, "No card detected after retry");
    }

    if( Flag_Detected_IC != CardDetected )      // 状态改变
    {
        status = 1;
        Flag_Detected_IC = CardDetected;        // 同步状态
        ESP_LOGI(TAG, "Card state changed: %d -> %d", Flag_Detected_IC, CardDetected);
    }

    return status;
}



/////////////////////////////////////////////////////////////////////
//功    能：获取最后检测到的卡片ID
//参数说明: ID[OUT]:卡片ID，4字节
//返    回: 1=获取成功, 0=无卡片
////////////////////////////////////////////////////////////////////
unsigned char CR_GetCardID( unsigned char *ID )
{
    if( Flag_Detected_IC == 1 )
    {
        for( unsigned char count=0; count<4; count++ )
        {
            ID[count] = SelectedSnr[count];
        }

        return 1;
    }

    return 0;
}














/////////////////////////////////////////////////////////////////////
//功    能：获取TypeA卡的UID（带重试机制）
//参数说明: id[OUT]:卡片UID，4字节
//返    回: 1=检测到卡片, 0=无卡片
////////////////////////////////////////////////////////////////////
unsigned char SI523_TypeA_GetUID( unsigned char *id )
{
    unsigned char ATQA[2];
    unsigned char UID[12];
    unsigned char SAK = 0;
    unsigned char CardDetected = 0;
    unsigned char retry  = 5;

    ESP_LOGI(TAG, "SI523_TypeA_GetUID: retry count=%d", retry);
    I_SI523_IO_Write( RFCfgReg, RFCfgReg_Val );                 // 复位接收增益

    while( retry-- )
    {
        if( PcdRequest( PICC_REQIDL, ATQA ) == MI_OK )              // 寻天线区内未进入休眠状态的卡，返回卡片类型 2字节
        {
            if( PcdAnticoll( UID, PICC_ANTICOLL1 ) == MI_OK )       // Anticoll 冲突检测 level1
            {
                if( PcdSelect1( UID, &SAK ) == MI_OK )
                {
                    if( ( SAK & 0x04 ) == 0 )
                    {
                        for( unsigned char count=0; count<4; count++ )
                        {
                            id[count] = SelectedSnr[count] = UID[count];
                        }

                        CardDetected = 1;
                        ESP_LOGI(TAG, "Card detected: %02X%02X%02X%02X", UID[0], UID[1], UID[2], UID[3]);
                        break;
                    }
                }
            }
        }
    }

    if( Flag_Detected_IC != CardDetected )
    {
        Flag_Detected_IC = CardDetected;
        ESP_LOGI(TAG, "Card state changed");
    }

    vTaskDelay(pdMS_TO_TICKS(1));//DelayUs( 100 );

    return CardDetected;
}

/////////////////////////////////////////////////////////////////////
//功    能：获取最后一张卡片的ID
//参数说明: id[OUT]:卡片UID，4字节
////////////////////////////////////////////////////////////////////
void GetLastCardID( unsigned char *id )
{
    ESP_LOGI(TAG, "LastCard: %02X%02X%02X%02X", SelectedSnr[0], SelectedSnr[1], SelectedSnr[2], SelectedSnr[3] );
    memcpy( id, SelectedSnr, 4 );
}

/////////////////////////////////////////////////////////////////////
//功    能：打印Mifare Ultralight卡片内容到日志
//参数说明: 无
//返    回: 无
////////////////////////////////////////////////////////////////////
void PICC_DumpMifareUltralightToLog(void)
{
    char status;
    //unsigned char byteCount;
    unsigned char buffer[18];
    char log_line[128];
    unsigned char i;
    
    ESP_LOGI(TAG, "Page  0  1  2  3");
    // Try the pages of the original Ultralight. Ultralight C has more pages.
    for(unsigned char page = 0; page < 16; page += 4) { // Read returns data for 4 pages at a time.
        // Read pages
        //byteCount = sizeof(buffer);
        status = PcdRead(page, buffer);
        if(status != MI_OK) {
            ESP_LOGE(TAG, "PcdRead() failed: status=%d", status);
            break;
        }
        // Dump data
        for(unsigned char offset = 0; offset < 4; offset++) {
            i = page+offset;
            int pos = 0;
            // Format page number
            if(i < 10) {
                pos += snprintf(log_line+pos, sizeof(log_line)-pos, "  %d  ", i);
            } else {
                pos += snprintf(log_line+pos, sizeof(log_line)-pos, " %d  ", i);
            }
            // Format hex data
            for(unsigned char index = 0; index < 4; index++) {
                i = 4*offset+index;
                if(buffer[i] < 0x10) {
                    pos += snprintf(log_line+pos, sizeof(log_line)-pos, " 0%02X", buffer[i]);
                } else {
                    pos += snprintf(log_line+pos, sizeof(log_line)-pos, " %02X", buffer[i]);
                }
            }
            ESP_LOGI(TAG, "%s", log_line);
        }
    }
}
