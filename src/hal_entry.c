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
#include "SoftAP_connect_wifi/SoftAP_connect_wifi.h"
#include "log.h"
#include <stdio.h>

/* 1=开启网页联调模拟数据；0=关闭（使用真实采样数据） */
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

void hal_entry(void)
{
    Debug_UART0_Init();
    SystickInit();

    Relay_Reset_All();
    Key_GPT_Init();
    uart_hlw_init();

    circlebuf_init();
    BL0942_circlebuf_init();

    res_sd = f_mount(&fs, "1:", 1);
    /*
     * 第一阶段先保持裸机轮询模式，但把 ESP8266 相关逻辑全部收口到 WiFi 总管入口。
     * 这样后续迁移到 FreeRTOS 时，只需要把 WiFi_ServiceTask() 挪进独立任务即可。
     */
    WiFi_Init();

    while (1)
    {
        /* WiFi 总管：统一推进联网、配网、MQTT 发送和 MQTT 下行解析。 */
        WiFi_ServiceTask();
        Relay_Task();
        AI_Replug_Task();
        AI_Commit_Task();
        Key_Task();
        Upload_Status_Task();

        /* CH444 + UART3 分时轮询四路 BL0942 */
        BL0942_Poll_Task();
    }
}

#if BSP_TZ_SECURE_BUILD
R_BSP_NonSecureEnter();
#endif

void R_BSP_WarmStart(bsp_warm_start_event_t event)
{
    if (BSP_WARM_START_RESET == event)
    {
#if BSP_FEATURE_FLASH_LP_VERSION != 0
        R_FACI_LP->DFLCTL = 1U;
#endif
    }

    if (BSP_WARM_START_POST_C == event)
    {
        R_IOPORT_Open(&g_ioport_ctrl, g_ioport.p_cfg);
    }
}

#if BSP_TZ_SECURE_BUILD
BSP_CMSE_NONSECURE_ENTRY void template_nonsecure_callable();

BSP_CMSE_NONSECURE_ENTRY void template_nonsecure_callable()
{
}
#endif
