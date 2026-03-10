#include "Relay.h"
#include "stdio.h"
#include <string.h>
#include "cJSON_handle.h"
#include "Systick.h"
extern PowerStrip_t g_strip;

/* 继电器数量与脉冲宽度（毫秒） */
#define RELAY_COUNT      4U
#define RELAY_PULSE_MS   100U

/* 继电器引脚定义 */
typedef struct {
    bsp_io_port_pin_t set_pin;   /* 开启脉冲引脚（SET） */
    bsp_io_port_pin_t reset_pin; /* 关闭脉冲引脚（RESET） */
} Relay_Config_t;

/* 插座引脚映射表 */
static const Relay_Config_t g_relays[RELAY_COUNT] = {
    [0] = { .set_pin = BSP_IO_PORT_01_PIN_03, .reset_pin = BSP_IO_PORT_01_PIN_02 }, /* 插座 1 */
    [1] = { .set_pin = BSP_IO_PORT_01_PIN_05, .reset_pin = BSP_IO_PORT_01_PIN_04 }, /* 插座 2 */
    [2] = { .set_pin = BSP_IO_PORT_01_PIN_07, .reset_pin = BSP_IO_PORT_01_PIN_06 }, /* 插座 3 */
    [3] = { .set_pin = BSP_IO_PORT_06_PIN_01, .reset_pin = BSP_IO_PORT_06_PIN_00 }  /* 插座 4 */
};

/* 每路继电器的非阻塞脉冲上下文 */
typedef struct {
    bool active;
    bsp_io_port_pin_t active_pin;
    uint32_t deadline;
} Relay_PulseCtx_t;

static Relay_PulseCtx_t g_relay_pulse[RELAY_COUNT];

/*
 * 启动一条非阻塞脉冲：置高后登记截止时间，到时由 Relay_Task 拉低。
 * 若该路存在未结束脉冲，先收尾旧脉冲，避免引脚重叠拉高。
 */
static void relay_start_pulse(uint8_t index, bsp_io_port_pin_t pin)
{
    if ((index >= RELAY_COUNT) || (pin == 0))
    {
        return;
    }

    if (g_relay_pulse[index].active)
    {
        g_ioport.p_api->pinWrite(g_ioport.p_ctrl,
                                 g_relay_pulse[index].active_pin,
                                 BSP_IO_LEVEL_LOW);
    }

    g_ioport.p_api->pinWrite(g_ioport.p_ctrl, pin, BSP_IO_LEVEL_HIGH);
    g_relay_pulse[index].active = true;
    g_relay_pulse[index].active_pin = pin;
    g_relay_pulse[index].deadline = HAL_GetTick() + RELAY_PULSE_MS;
}

/**
 * @brief  继电器开启（发送 100ms 脉冲，非阻塞）
 */
void Relay_Set_ON(uint8_t index)
{
    if (index >= RELAY_COUNT)
    {
        return;
    }

    relay_start_pulse(index, g_relays[index].set_pin);
}

/**
 * @brief  继电器关闭（发送 100ms 脉冲，非阻塞）
 */
void Relay_Set_OFF(uint8_t index)
{
    if (index >= RELAY_COUNT)
    {
        return;
    }

    relay_start_pulse(index, g_relays[index].reset_pin);
}

/**
 * @brief  继电器非阻塞任务：到时结束脉冲
 * @note   建议主循环每 1ms~10ms 调用一次
 */
void Relay_Task(void)
{
    uint32_t now = HAL_GetTick();
    for (uint8_t i = 0; i < RELAY_COUNT; i++)
    {
        if (!g_relay_pulse[i].active)
        {
            continue;
        }

        if ((int32_t)(now - g_relay_pulse[i].deadline) >= 0)
        {
            g_ioport.p_api->pinWrite(g_ioport.p_ctrl,
                                     g_relay_pulse[i].active_pin,
                                     BSP_IO_LEVEL_LOW);
            g_relay_pulse[i].active = false;
            g_relay_pulse[i].active_pin = (bsp_io_port_pin_t)0;
        }
    }
}

/**
 * @brief  开机默认复位函数（关闭所有插座）
 */
void Relay_Reset_All(void)
{
    printf("[System] 正在初始化继电器状态：全路切断...\r\n");
    for (uint8_t i = 0; i < RELAY_COUNT; i++)
    {
        Relay_Set_OFF(i);

        /* 同步全局状态结构体 */
        g_strip.sockets[i].on = false;
        strncpy(g_strip.sockets[i].device_name, "None", 15);
        g_strip.sockets[i].device_name[15] = '\0';
    }
}
