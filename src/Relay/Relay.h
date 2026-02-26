#ifndef __RELAY_H
#define __RELAY_H
#include "hal_data.h"
void Relay_Set_ON(uint8_t index);
void Relay_Set_OFF(uint8_t index);
void Relay_Reset_All(void);

#endif