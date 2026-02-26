#ifndef __SDCARD_DATA_HANDLE_H
#define	__SDCARD_DATA_HANDLE_H

#include "hal_data.h"
#include "ff.h"


typedef struct {
    uint16_t id;            // 设备 ID
    char name[20];          // 设备名称
    float power;            // 典型功率 (W)
    float pf;               // 典型功率因数
    uint32_t startup_time;  // 启动时间参考 (ms)
    float weight;           // 特征权重
} Appliance_Data_t;

int Check_Device_Exist(const char *name);
FRESULT Save_Appliance_Data(Appliance_Data_t *device);
FRESULT Load_And_Print_All(void);

#endif
