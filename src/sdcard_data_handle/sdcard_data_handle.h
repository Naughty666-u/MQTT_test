#ifndef __SDCARD_DATA_HANDLE_H
#define	__SDCARD_DATA_HANDLE_H

#include "hal_data.h"
#include "ff.h"


typedef struct {
    uint16_t id;            // 设备 ID
    char name[20];          // 设备名称
    float power;            // 稳态有功功率 (W)
    float pf;               // 典型功率因数
    float i_surge_ratio;    // 启动激增比 (I_max / I_steady)
    float q_reactive;       // 无功功率 (Var)
} Appliance_Data_t;

#define DEVICE_DB_PATH "1:Device.csv" // 定义数据库文件路径

int Check_Device_Exist(const char *name);
FRESULT Save_Appliance_Data(Appliance_Data_t *device);
FRESULT Load_And_Print_All(void);

#endif
