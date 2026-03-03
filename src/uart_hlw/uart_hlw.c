#include "uart_hlw.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* 低功耗日志节流时间戳，避免串口刷屏 */
static uint32_t g_lowpower_log_tick[4] = {0};

/* UART3 发送完成标志 */
static volatile int g_uart3_tx_complete = 0;

extern circle_buf_t g_BL0942_rx_buf;

/* BL0942 读取命令：0x58 0xAA */
uint8_t com_data[2] = {0x58, 0xAA};

/* UART3 一帧接收结束标志 */
volatile bool g_uart3_rx_end = 0;

extern PowerStrip_t g_strip;

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
    if (p_args->event == TIMER_EVENT_CYCLE_END)
    {
        g_uart3_rx_end = 1;
    }
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
            if (g_uart3_rx_end)
            {
                return;
            }

            /* 接收字节写入 BL0942 专用环形缓冲区 */
            g_BL0942_rx_buf.put(&g_BL0942_rx_buf, (uint8_t)p_args->data);
            break;
        }

        default:
            break;
    }
}

/* 主动向 BL0942 发起一次数据读取 */
void Send_com(void)
{
    g_uart3.p_api->write(g_uart3.p_ctrl, com_data, 2);
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

        /* 2) 更新全局电压 */
        g_strip.voltage = (float)V1;

        /* 3) 重新计算总功率/总电流 */
        float temp_p_sum = 0.0f;
        float temp_c_sum = 0.0f;
        for (int j = 0; j < 4; j++)
        {
            temp_p_sum += g_strip.sockets[j].power;
            temp_c_sum += (g_strip.sockets[j].power / g_strip.voltage);
        }
        g_strip.total_power = temp_p_sum;
        g_strip.total_current = temp_c_sum;

        /* 低功耗归类：不进入识别 */
        if (g_strip.sockets[index].on && g_strip.sockets[index].power < LOW_POWER_THRESHOLD_W)
        {
            uint32_t now_tick = HAL_GetTick();
            if (now_tick - g_lowpower_log_tick[index] >= 1000U)
            {
                g_lowpower_log_tick[index] = now_tick;
                printf("[LP] socket=%d power=%.2fW (skip AI)\r\n", index, g_strip.sockets[index].power);
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
            }
        }
    }
}

void uart_hlw_init(void)
{
    ELCDrvInit();
    GPTDrvInit_elc();
    BL0942_UART3_Init();

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
