#include "Systick.h"

volatile uint32_t dwTick=0;
#define HAL_MAX_DELAY 0xFFFFFFU

//systick初始化
fsp_err_t SystickInit(void)
{
    //获取处理器时钟uwSysclk
    uint32_t uwSysclk=R_BSP_SourceClockHzGet(FSP_PRIV_CLOCK_PLL);
    //计数周期为uwSysclk/1000,即为1ms
    if(SysTick_Config(uwSysclk/1000)!=0)
    {
        return FSP_ERR_ASSERTION;
    }
    return FSP_SUCCESS;

}

void SysTick_Handler(void)
{
    dwTick+=1;

}

uint32_t HAL_GetTick(void)
{
    return dwTick;

}

//ms级延时
void HAL_Delay(uint32_t dwTime)
{
    uint32_t dWstart=dwTick;
    uint32_t dwWait=dwTime;
    if(dwWait<HAL_MAX_DELAY)
    {
        dwWait+=(uint32_t)(1);

    }

    while((dwTick-dWstart)<dwWait)
    {

    }


}
