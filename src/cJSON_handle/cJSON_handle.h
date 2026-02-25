#ifndef __CJSON_HANDLE_H
#define	__CJSON_HANDLE_H
#include "hal_data.h"
#include "stdio.h"

typedef struct {
    // 基础物理量
    float voltage;       // 全局电压
    float total_current; // 总电流 (4路之和)
    float total_power;   // 总功率 (4路之和)
    
    // 独立插座数据
    struct {
        bool  on;
        float power;
        char  device_name[16]; // 对应 JSON 中的 "device"
    } sockets[4];
} PowerStrip_t;


void upload_strip_status(void);
void process_cloud_data(const char *json_str);
void handle_uart_json_stream(void);

#endif
