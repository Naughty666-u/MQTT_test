#include "Relay.h"
#include "stdio.h"
#include  "cJSON_handle.h"

extern PowerStrip_t g_strip;

// 磁保持继电器引脚结构体
typedef struct {
    bsp_io_port_pin_t set_pin;   // 开启脉冲引脚 (SET)
    bsp_io_port_pin_t reset_pin; // 关闭脉冲引脚 (RESET)
} Relay_Config_t;

// 插座引脚映射表 
const Relay_Config_t g_relays[4] = {
    [0] = { .set_pin = BSP_IO_PORT_01_PIN_02, .reset_pin = BSP_IO_PORT_01_PIN_03 }, // 插座 1
    [1] = { .set_pin = BSP_IO_PORT_01_PIN_04, .reset_pin = BSP_IO_PORT_01_PIN_05 }, // 插座 2
    [2] = { .set_pin = BSP_IO_PORT_01_PIN_06, .reset_pin = BSP_IO_PORT_01_PIN_07 }, // 插座 3
    [3] = { .set_pin = BSP_IO_PORT_06_PIN_00, .reset_pin = BSP_IO_PORT_06_PIN_01 }  // 插座 4
};



/**
 * @brief  继电器开启 (发送 100ms 脉冲)
 */
void Relay_Set_ON(uint8_t index)
{
    if (index >= 4 || g_relays[index].set_pin == 0) return;

    // 发送 SET 脉冲
    g_ioport.p_api->pinWrite(g_ioport.p_ctrl, g_relays[index].set_pin, BSP_IO_LEVEL_HIGH);
    R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MILLISECONDS);
    g_ioport.p_api->pinWrite(g_ioport.p_ctrl, g_relays[index].set_pin, BSP_IO_LEVEL_LOW);
}

/**
 * @brief  继电器关闭 (发送 100ms 脉冲)
 */
void Relay_Set_OFF(uint8_t index)
{
    if (index >= 4 || g_relays[index].reset_pin == 0) return;

    // 发送 RESET 脉冲
    g_ioport.p_api->pinWrite(g_ioport.p_ctrl, g_relays[index].reset_pin, BSP_IO_LEVEL_HIGH);
    R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MILLISECONDS);
    g_ioport.p_api->pinWrite(g_ioport.p_ctrl, g_relays[index].reset_pin, BSP_IO_LEVEL_LOW);
}

/**
 * @brief  开机默认复位函数 (关闭所有插座)
 */
void Relay_Reset_All(void)
{
    printf("[System] 正在初始化继电器状态：全路切断...\r\n");
    for (uint8_t i = 0; i < 4; i++)
    {
        Relay_Set_OFF(i);
        // 同时同步全局状态结构体
        g_strip.sockets[i].on = false;
        strncpy(g_strip.sockets[i].device_name, "None", 15);
    }
}














//bsp_io_level_t led_level=BSP_IO_LEVEL_LOW;
//void sw_ctrl(bool sw)
//{
//	if(sw==true)
//	{
//		g_ioport.p_api->pinWrite(g_ioport.p_ctrl,BSP_IO_PORT_06_PIN_09,BSP_IO_LEVEL_HIGH);
//		R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MILLISECONDS);
//		g_ioport.p_api->pinWrite(g_ioport.p_ctrl,BSP_IO_PORT_06_PIN_09,BSP_IO_LEVEL_LOW);
//	}
//	else
//	{
//		g_ioport.p_api->pinWrite(g_ioport.p_ctrl,BSP_IO_PORT_06_PIN_08,BSP_IO_LEVEL_HIGH);
//		R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MILLISECONDS);
//		g_ioport.p_api->pinWrite(g_ioport.p_ctrl,BSP_IO_PORT_06_PIN_08,BSP_IO_LEVEL_LOW);
//	}
//}

