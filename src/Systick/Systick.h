#ifndef __SYSTICK_H
#define __SYSTICK_H

#include "hal_data.h"
fsp_err_t SystickInit(void);
void SysTick_Handler(void);
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t dwTime);

#endif