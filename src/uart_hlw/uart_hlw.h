#ifndef __UART_HLW_H
#define __UART_HLW_H
#include "hal_data.h"
void uart_hlw_init(void);
void Data_Processing(unsigned char *data,uint8_t index);
void BL0942_UART3_Init(void);
void Send_com(void);
void Simulation_Random_Load_Test(void);
#endif