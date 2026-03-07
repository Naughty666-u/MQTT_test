#include "uart_hlw.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "bsp_debug_uart.h"
#include "circle_buf.h"
#include "cJSON_handle.h"
#include "appliance_identification.h"
#include "event_detector.h"
#include "Systick.h"

/* 每路插孔事件检测器 */
static EventDetector_t g_evt_det[4];
static bool g_evt_inited = false;

/* BL0942 电流量程配置（与模块型号一致：10A/20A） */
#define MAX_C 20

/* 功率过滤阈值 */
#define LOW_POWER_THRESHOLD_W 6.0f
#define MAX_VALID_POWER_W 5000.0f
#define POWER_UPLOAD_DELTA_W 10.0f

/* 低功耗日志节流时间戳，避免串口刷屏 */
static uint32_t g_lowpower_log_tick[4] = {0};

/* UART3 发送完成标志 */
static volatile int g_uart3_tx_complete = 0;

extern circle_buf_t g_BL0942_rx_buf;

/* BL0942 读取命令：0x58 0xAA */
uint8_t com_data[2] = {0x58, 0xAA};

/* CH444 选择脚：IN1=P603，IN0=P602 */
#define CH444_IN1_PIN BSP_IO_PORT_06_PIN_03
#define CH444_IN0_PIN BSP_IO_PORT_06_PIN_02

#define BL0942_FRAME_LEN         23U
#define BL0942_RX_TIMEOUT_MS     40U

typedef enum
{
    BL_STATE_IDLE = 0,
    BL_STATE_WAIT_FRAME,
} bl_poll_state_t;

static volatile uint8_t g_bl0942_rx_frame[BL0942_FRAME_LEN] = {0};
static volatile uint8_t g_bl0942_rx_count = 0;
static volatile bool g_bl0942_frame_ready = false;
static volatile bl_poll_state_t g_bl_poll_state = BL_STATE_IDLE;
static volatile uint8_t g_active_channel = 0;
static volatile uint32_t g_bl_deadline_tick = 0;

extern PowerStrip_t g_strip;

static void CH444_Select_Channel(uint8_t channel)
{
    /* channel: 0..3 -> IN1/IN0: 00/01/10/11 */
    bsp_io_level_t in0 = (channel & 0x01U) ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW;
    bsp_io_level_t in1 = (channel & 0x02U) ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW;

    g_ioport.p_api->pinWrite(g_ioport.p_ctrl, CH444_IN0_PIN, in0);
    g_ioport.p_api->pinWrite(g_ioport.p_ctrl, CH444_IN1_PIN, in1);
}

static void BL0942_Rx_Reset(void)
{
    g_bl0942_rx_count = 0;
    g_bl0942_frame_ready = false;
    memset((void *)g_bl0942_rx_frame, 0, sizeof(g_bl0942_rx_frame));
}

void GPTDrvInit_elc(void)
{
    fsp_err_t err = g_timer0.p_api->open(g_timer0.p_ctrl, g_timer0.p_cfg);
    if (FSP_SUCCESS != err)
    {
        printf("timer0 open failed\r\n");
    }

    err = g_timer0.p_api->enable(g_timer0.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        printf("timer0 enable failed\r\n");
    }
}

void ELCDrvInit(void)
{
    fsp_err_t err = g_elc.p_api->open(g_elc.p_ctrl, g_elc.p_cfg);
    if (FSP_SUCCESS != err)
    {
        printf("Elc open failed\r\n");
        return;
    }

    err = g_elc.p_api->enable(g_elc.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        printf("Elc enable failed\r\n");
        return;
    }
}

/* BL0942 对应 UART3 初始化 */
void BL0942_UART3_Init(void)
{
    fsp_err_t err = g_uart3.p_api->open(g_uart3.p_ctrl, g_uart3.p_cfg);
    if (FSP_SUCCESS != err)
    {
        printf("uart_hlw open failed\r\n");
    }
    else
    {
        printf("uart_hlw open success\r\n");
    }
}

void gpt_uart_callback(timer_callback_args_t *p_args)
{
    FSP_PARAMETER_NOT_USED(p_args);
}

void uart3_callback(uart_callback_args_t *p_args)
{
    switch (p_args->event)
    {
        case UART_EVENT_TX_COMPLETE:
        {
            g_uart3_tx_complete = 1;
            break;
        }

        case UART_EVENT_RX_CHAR:
        {
            if (g_bl_poll_state != BL_STATE_WAIT_FRAME)
            {
                return;
            }

            if (g_bl0942_rx_count < BL0942_FRAME_LEN)
            {
                g_bl0942_rx_frame[g_bl0942_rx_count++] = (uint8_t)p_args->data;
                if (g_bl0942_rx_count >= BL0942_FRAME_LEN)
                {
                    g_bl0942_frame_ready = true;
                }
            }
            break;
        }

        default:
            break;
    }
}

/* 主动向 BL0942 发起一次数据读取 */
void Send_com(void)
{
    g_uart3_tx_complete = 0;
    g_uart3.p_api->write(g_uart3.p_ctrl, com_data, 2);
}

void BL0942_Poll_Task(void)
{
    uint32_t now = HAL_GetTick();

    switch (g_bl_poll_state)
    {
        case BL_STATE_IDLE:
        {
            /* 切到当前通道并清理旧帧，防止串路污染 */
            CH444_Select_Channel(g_active_channel);
            BL0942_Rx_Reset();

            Send_com();
            g_bl_deadline_tick = now + BL0942_RX_TIMEOUT_MS;
            g_bl_poll_state = BL_STATE_WAIT_FRAME;
            break;
        }

        case BL_STATE_WAIT_FRAME:
        {
            if (g_bl0942_frame_ready)
            {
                uint8_t frame[BL0942_FRAME_LEN];
                memcpy(frame, (const void *)g_bl0942_rx_frame, BL0942_FRAME_LEN);

                Data_Processing(frame, g_active_channel);

                g_active_channel = (uint8_t)((g_active_channel + 1U) & 0x03U);
                g_bl_poll_state = BL_STATE_IDLE;
            }
            else if ((int32_t)(now - g_bl_deadline_tick) >= 0)
            {
                printf("[BL] timeout ch=%u rx=%u\r\n",
                       (unsigned)(g_active_channel + 1U),
                       (unsigned)g_bl0942_rx_count);
                g_active_channel = (uint8_t)((g_active_channel + 1U) & 0x03U);
                g_bl_poll_state = BL_STATE_IDLE;
            }
            break;
        }

        default:
            g_bl_poll_state = BL_STATE_IDLE;
            break;
    }
}

/*
 * BL0942 原始帧解析：
 * 1. 校验和校验
 * 2. 计算电流/电压/有功功率/功率因数/电量
 * 3. 更新全局功率状态
 * 4. 触发事件检测与识别引擎
 */
void Data_Processing(unsigned char *data, uint8_t index)
{
    if (index >= 4) return;

    uint8_t i = 0, check_num = 0;
    uint32_t count = 88;
    uint32_t V_REG = 0, PF_COUNT = 0;
    int32_t C_REG = 0, P_REG = 0;
    double V1 = 0, C1 = 0, P1 = 0, P2 = 0, P3 = 0, E_con = 0;

    /* 累加前 22 字节用于校验码计算 */
    for (i = 0; i < 22; i++)
    {
        count = count + data[i];
    }

    /* 取反得到校验码 */
    check_num = ~(count & 0xFF);

    if (check_num == data[22])
    {
        /* 电流寄存器（有符号） */
        C_REG = data[3] * 65536 + data[2] * 256 + data[1];
        if (data[3] & 0x80)
        {
            C_REG = -(16777216 - C_REG);
        }

        if (MAX_C == 10)
        {
            C1 = C_REG * 1.218 / (305978 * 3);
        }
        else
        {
            C1 = C_REG * 1.218 / (305978);
        }

        /* 电压寄存器 */
        V_REG = data[6] * 65536 + data[5] * 256 + data[4];
        V1 = V_REG * 1.218 * 1950.51 / 37734390;

        /* 视在功率 */
        P2 = V1 * C1;

        /* 有功功率寄存器（有符号） */
        P_REG = data[12] * 65536 + data[11] * 256 + data[10];
        if (data[12] & 0x80)
        {
            P_REG = -(16777216 - P_REG);
        }

        if (MAX_C == 10)
        {
            P1 = P_REG * 1.218 * 1.218 * 1950.51 / 5411610;
        }
        else
        {
            P1 = P_REG * 1.218 * 1.218 * 1950.51 / 1803870;
        }

        /* 功率因数 = 有功 / 视在 */
        if (P2 != 0)
        {
            P3 = P1 / P2;
        }

        /* 电量脉冲计数 */
        PF_COUNT = data[15] * 65536 + data[14] * 256 + data[13];
        if (MAX_C == 10)
        {
            E_con = PF_COUNT / 16051.896;
        }
        else
        {
            E_con = PF_COUNT / 5350.632;
        }

        (void)E_con; /* 当前流程未使用，保留计算结果便于后续扩展 */
    }
    else
    {
        printf("Check Error\r\n");
    }

    if (check_num == data[22])
    {
        float p_used = (P1 >= 0.0) ? (float)P1 : (float)(-P1);
        float p_prev = g_strip.sockets[index].power;

        /* 异常功率过滤，避免污染识别状态 */
        if (p_used > MAX_VALID_POWER_W)
        {
            printf("[PWR] drop abnormal power: %.2fW\r\n", p_used);
            AI_Reset(index);
            EventDetector_Init(&g_evt_det[index]);
            return;
        }

        /* 1) 更新该路实时功率 */
        g_strip.sockets[index].power = p_used;
        if (fabsf(p_used - p_prev) >= POWER_UPLOAD_DELTA_W)
        {
            request_status_upload();
        }

        /* 2) 更新全局电压 */
        g_strip.voltage = (float)V1;

        /* 3) 重新计算总功率/总电流（仅统计 on=true 的插座，避免网页口径冲突） */
        float temp_p_sum = 0.0f;
        float temp_c_sum = 0.0f;
        for (int j = 0; j < 4; j++)
        {
            if (!g_strip.sockets[j].on)
            {
                continue;
            }

            temp_p_sum += g_strip.sockets[j].power;
            if (g_strip.voltage > 1.0f)
            {
                temp_c_sum += (g_strip.sockets[j].power / g_strip.voltage);
            }
        }
        g_strip.total_power = temp_p_sum;
        g_strip.total_current = temp_c_sum;

        /* 低功耗归类：不进入识别 */
        if (g_strip.sockets[index].on && g_strip.sockets[index].power < LOW_POWER_THRESHOLD_W)
        {
            uint32_t now_tick = HAL_GetTick();
            if (now_tick - g_lowpower_log_tick[index] >= 3000U)
            {
                g_lowpower_log_tick[index] = now_tick;
                printf("[LP] socket=%d power=%.2fW (skip AI)\r\n", index, g_strip.sockets[index].power);
            }

            if (strcmp(g_strip.sockets[index].device_name, "LowPower") != 0)
            {
                request_status_upload();
            }
            strncpy(g_strip.sockets[index].device_name, "LowPower", 15);
            AI_Reset(index);
            EventDetector_Init(&g_evt_det[index]);
            return;
        }

        /* OFF->ON 事件检测：决定是否触发采样窗口 */
        bool need_trigger = EventDetector_Update(&g_evt_det[index],
                                                 g_strip.sockets[index].on,
                                                 p_used,
                                                 HAL_GetTick());
        if (need_trigger)
        {
            AI_Trigger_Sampling(index);
        }

        /* 推进识别状态机 */
        AI_Recognition_Engine(index, p_used, (float)V1, (float)C1, (float)P3);

        /* 长时间低功率 OFF 收敛后，显示回到 None */
        if (g_strip.sockets[index].on && g_evt_det[index].state == EVT_STATE_OFF)
        {
            if (g_strip.sockets[index].power < 1.0f && g_evt_det[index].off_cnt >= 3)
            {
                strncpy(g_strip.sockets[index].device_name, "None", 15);
                AI_Reset(index);
                request_status_upload();
            }
        }
    }
}

void uart_hlw_init(void)
{
    

    ELCDrvInit();
    GPTDrvInit_elc();
    BL0942_UART3_Init();
    CH444_Select_Channel(0);
    BL0942_Rx_Reset();
    g_bl_poll_state = BL_STATE_IDLE;
    g_active_channel = 0;

    if (!g_evt_inited)
    {
        for (int i = 0; i < 4; i++)
        {
            EventDetector_Init(&g_evt_det[i]);
        }
        g_evt_inited = true;
    }
}

/* 随机负载模拟（调试用途） */
void Simulation_Random_Load_Test(void)
{
    /* 模拟电压波动 218.0V ~ 222.0V */
    g_strip.voltage = 218.0f + (float)(rand() % 41) / 10.0f;

    float base_p[4]  = {150.0f, 15.0f, 2.0f, 40.0f};
    float range_p[4] = {20.0f, 2.0f, 0.5f, 5.0f};

    float power_sum = 0.0f;

    for (int i = 0; i < 4; i++)
    {
        if (g_strip.sockets[i].on)
        {
            float noise = (float)(rand() % (int)(range_p[i] * 10)) / 10.0f;
            g_strip.sockets[i].power = base_p[i] + (noise - (range_p[i] / 2.0f));
            power_sum += g_strip.sockets[i].power;
        }
        else
        {
            g_strip.sockets[i].power = 0.0f;
        }
    }

    g_strip.total_power = power_sum;
    g_strip.total_current = g_strip.total_power / g_strip.voltage;
}
