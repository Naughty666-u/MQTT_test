#include "Relay.h"


bsp_io_level_t led_level=BSP_IO_LEVEL_LOW;
void sw_ctrl(bool sw)
{
	if(sw==true)
	{
		g_ioport.p_api->pinWrite(g_ioport.p_ctrl,BSP_IO_PORT_06_PIN_09,BSP_IO_LEVEL_HIGH);
		R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MILLISECONDS);
		g_ioport.p_api->pinWrite(g_ioport.p_ctrl,BSP_IO_PORT_06_PIN_09,BSP_IO_LEVEL_LOW);
	}
	else
	{
		g_ioport.p_api->pinWrite(g_ioport.p_ctrl,BSP_IO_PORT_06_PIN_08,BSP_IO_LEVEL_HIGH);
		R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MILLISECONDS);
		g_ioport.p_api->pinWrite(g_ioport.p_ctrl,BSP_IO_PORT_06_PIN_08,BSP_IO_LEVEL_LOW);
	}
}