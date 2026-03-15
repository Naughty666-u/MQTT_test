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
        bool  on;              // 插孔通断状态
        float power;           // 插孔实时功率
        char  device_name[16]; // 对应 JSON 中的 "device"
        bool  pending_valid;   // 是否存在待命名（pending）样本
        uint32_t pending_id;   // 待命名样本ID，仅 Unknown 阶段上报
    } sockets[4];
} PowerStrip_t;


// 组装并上报 status 主题数据
void upload_strip_status(void);
// 上报任务：统一处理心跳上报、事件触发上报和离线清标志
void Upload_Status_Task(void);
// 请求尽快上报一次状态（由主循环统一节流执行）
void request_status_upload(void);
// 网页联调测试：填充4路模拟功率数据（遵循当前 on 状态）
void web_mqtt_test_fill_mock(void);
// 处理一条云端命令 JSON（ON/OFF/LEARN_COMMIT/RELEARN_REPLUG/CORRECT）
void process_cloud_cmd(const char *json_str);
// 历史接口名（保留声明，避免旧代码引用时报错）
void process_cloud_data(const char *json_str);
// 从 UART 环形缓冲区中流式提取并处理 JSON 命令
void handle_uart_json_stream(void);

#endif



