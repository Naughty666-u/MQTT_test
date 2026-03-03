#include "hal_data.h"

FSP_CPP_HEADER
void R_BSP_WarmStart(bsp_warm_start_event_t event);
FSP_CPP_FOOTER

#include "debug_uart/bsp_debug_uart.h"
#include "bsp_esp8266/bsp_wifi_esp8266.h"
#include "circle_buf.h"
#include "cJSON_handle.h"
#include "stdlib.h"
#include "Systick.h"
#include "Key.h"
#include "Relay.h"
#include "uart_hlw.h"
#include <time.h>
#include "sdcard_data_handle.h"
#include "appliance_identification.h"
#include "ai_validate.h"

/* 1=开启网页联调模拟数据；0=关闭（使用真实采样数据） */
#define WEB_MQTT_TEST_MODE 0

MKFS_PARM f_opt = {
    .fmt = FM_FAT32,
    .n_fat = 0,
    .align = 0,
    .n_root = 0,
    .au_size = 0,
};

FATFS fs;
FIL fnew;
UINT fnum;
FRESULT res_sd;

static uint32_t last_poll = 0;
uint32_t last_report = 0;
extern PowerStrip_t g_strip;
uint8_t g_force_upload_flag = 0;

/* 按键触发标志 */
extern volatile bool key1_sw2_press;
extern volatile bool key2_sw3_press;
extern circle_buf_t g_BL0942_rx_buf;
extern volatile bool g_uart3_rx_end;

void hal_entry(void)
{
    /* 调试串口初始化 */
    Debug_UART0_Init();

    SystickInit();

    /* 上电先断开所有继电器，保证安全初始态 */
    Relay_Reset_All();
    Key_IRQ_Init();
    uart_hlw_init();

    circlebuf_init();
    BL0942_circlebuf_init();

    /* 挂载 SD 卡文件系统 */
    res_sd = f_mount(&fs, "1:", 1);

    /* 初始化 ESP8266 + MQTT */
    ESP8266_MQTT_Test();

    while (1)
    {
        /* 解析来自云端的 JSON 指令流 */
		
        handle_uart_json_stream();

        /* 定时上报或被命令触发的立即上报 */
        if ((HAL_GetTick() - last_report >= 3000U) || g_force_upload_flag)
        {
#if WEB_MQTT_TEST_MODE
            /* 上报前填充模拟数据，便于网页联调验证显示与控制链路。 */
            web_mqtt_test_fill_mock();
#endif
            g_force_upload_flag = 0;
            upload_strip_status();
            last_report = HAL_GetTick();
        }

        if (key1_sw2_press)
        {
            key1_sw2_press = false;
            Socket_Command_Handler(0, true);
            printf("key0 is pressed\r\n");
            g_ioport.p_api->pinWrite(g_ioport.p_ctrl, BSP_IO_PORT_01_PIN_02, BSP_IO_LEVEL_LOW);
        }

        if (key2_sw3_press)
        {
            key2_sw3_press = false;
            Socket_Command_Handler(0, false);
            printf("key1 is pressed\r\n");
            g_ioport.p_api->pinWrite(g_ioport.p_ctrl, BSP_IO_PORT_01_PIN_02, BSP_IO_LEVEL_HIGH);
        }

        if (g_uart3_rx_end == 1)
        {
            g_uart3_rx_end = 0;

            Data_Processing(g_BL0942_rx_buf.buffer, 0);
            BL0942_circlebuf_clear();
        }

        /* 周期轮询 BL0942 电参 */
        if (HAL_GetTick() - last_poll >= 100U)
        {
            last_poll = HAL_GetTick();
            Send_com();
        }
    }
}

#if BSP_TZ_SECURE_BUILD
/* Enter non-secure code */
R_BSP_NonSecureEnter();
#endif

void R_BSP_WarmStart(bsp_warm_start_event_t event)
{
    if (BSP_WARM_START_RESET == event)
    {
#if BSP_FEATURE_FLASH_LP_VERSION != 0
        /* Enable reading from data flash. */
        R_FACI_LP->DFLCTL = 1U;
#endif
    }

    if (BSP_WARM_START_POST_C == event)
    {
        /* Configure pins. */
        R_IOPORT_Open(&g_ioport_ctrl, g_ioport.p_cfg);
    }
}

#if BSP_TZ_SECURE_BUILD

BSP_CMSE_NONSECURE_ENTRY void template_nonsecure_callable();

/* Trustzone Secure Projects require at least one nonsecure callable function in order to build. */
BSP_CMSE_NONSECURE_ENTRY void template_nonsecure_callable()
{
}

#endif
