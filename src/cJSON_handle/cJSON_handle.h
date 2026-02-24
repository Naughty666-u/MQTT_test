#ifndef __CJSON_HANDLE_H
#define	__CJSON_HANDLE_H
#include "hal_data.h"
#include "stdio.h"
typedef struct {
    float voltage;       // 电压 [cite: 66, 96]
    float current;       // 电流 [cite: 67, 96]
    float total_power;   // 总功率 [cite: 65, 96]
    bool socket_on[4];   // 假设排插有4个插孔 [cite: 68-70]
} PowerStrip_t;

void upload_strip_status(void);
void process_cloud_data(const char *json_str);
void handle_uart_json_stream(void);

#endif
