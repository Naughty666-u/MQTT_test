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
#define WEB_MQTT_TEST_MODE 0
#define HEARTBEAT_MS 1500U
#define UPLOAD_MIN_GAP_MS 200U

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

uint32_t last_report = 0;
static uint32_t last_upload_tick = 0;
extern PowerStrip_t g_strip;
uint8_t g_force_upload_flag = 0;

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

    NET_Manager_Init();

    while (1)
    {
        NET_Manager_Task();
        Relay_Task();
        ESP8266_TxTask();
        handle_uart_json_stream();
        AI_Replug_Task();
        AI_Commit_Task();

        int key_evt = key_control();
        for (uint8_t i = 0; i < 4; i++)
        {
            if (key_evt & (1 << i))
            {
                bool target_on = !g_strip.sockets[i].on;
                Socket_Command_Handler(i, target_on);
                LOGD("[KEY] key%u pressed, relay%u -> %s\r\n",
                     (unsigned)(i + 1),
                     (unsigned)(i + 1),
                     target_on ? "ON" : "OFF");
            }
        }

        uint32_t now_tick = HAL_GetTick();
        bool heartbeat_due = (now_tick - last_report >= HEARTBEAT_MS);
        bool event_due = g_force_upload_flag && (now_tick - last_upload_tick >= UPLOAD_MIN_GAP_MS);
        if (NET_Manager_IsReady() && (heartbeat_due || event_due))
        {
#if WEB_MQTT_TEST_MODE
            web_mqtt_test_fill_mock();
#endif
            g_force_upload_flag = 0;
            upload_strip_status();
            last_report = now_tick;
            last_upload_tick = now_tick;
        }
        else if (!NET_Manager_IsReady())
        {
            /* 离线模式下不发送网络上报，避免发送队列堆积 */
            g_force_upload_flag = 0;
        }

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
