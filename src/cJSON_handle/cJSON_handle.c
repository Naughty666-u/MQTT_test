#include "cJSON.h"
#include <string.h>
#include "circle_buf.h"
#include  "cJSON_handle.h"
#include "stdlib.h"
#include "bsp_wifi_esp8266.h"
#include "bsp_led.h"
extern circle_buf_t g_rx_buf;
#define MAX_JSON_SIZE 512
static char json_process_buf[MAX_JSON_SIZE]; // 线性缓冲区，用于解析
extern _Bool  Uart2_Send_Flag;




PowerStrip_t g_strip = {220.0f, 0.0f, 0.0f, {false, false, false, false}};



void upload_strip_status(void) 
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    // 1. 填充基础电参 [cite: 63-67]
    cJSON_AddNumberToObject(root, "ts", 1772000100); // 实际应为动态时间戳
    cJSON_AddBoolToObject(root, "online", true);
    cJSON_AddNumberToObject(root, "total_power_w", g_strip.total_power);
    cJSON_AddNumberToObject(root, "voltage_v", g_strip.voltage);
    cJSON_AddNumberToObject(root, "current_a", g_strip.current);

    // 2. 创建插口数组 [cite: 68-70, 92]
    cJSON *sockets = cJSON_CreateArray();
    for(int i=0; i<2; i++) { // 以文档中2个插口为例
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", i+1);
        cJSON_AddBoolToObject(item, "on", g_strip.socket_on[i]);
        cJSON_AddNumberToObject(item, "power_w", g_strip.socket_on[i] ? (g_strip.total_power/2.0) : 0.0);
        cJSON_AddItemToArray(sockets, item);
    }
    cJSON_AddItemToObject(root, "sockets", sockets);

    // 3. 发送
    char *out = cJSON_PrintUnformatted(root);
    if (out) {
        // 使用你定义的宏 MQTT_PUB_STATUS [cite: 60, 163]
        Send_Data_Raw("dorm/A-303/strip01/status", out); 
        free(out);
    }
    cJSON_Delete(root);
}

void process_cloud_cmd(const char *json_str) 
{
    char *json_ptr = strchr(json_str, '{');
    if (!json_ptr) return;

    // 寻找最后一个 '}'，确保解析的是一段完整的对象
    char *json_end = strrchr(json_ptr, '}');
    if (json_end) {
        *(json_end + 1) = '\0'; 
    }

    printf("Fixed JSON: [%s]\n", json_ptr); // 再次确认

    cJSON *root = cJSON_Parse(json_ptr);
    if (!root) {
        printf("Parse Fail again! Error at: %s\n", cJSON_GetErrorPtr());
        return;
    }

    // 1. 获取 cmdId (用于 ACK 回执) [cite: 53, 121]
    cJSON *cmdId = cJSON_GetObjectItem(root, "cmdId");
    cJSON *type = cJSON_GetObjectItem(root, "type");       // ON/OFF [cite: 54]
    cJSON *socketId = cJSON_GetObjectItem(root, "socketId"); // 插孔号 [cite: 55]

    if (cmdId && type) {
        // 2. 执行硬件控制逻辑
        int id = socketId ? socketId->valueint : 1; 
        if (strcmp(type->valuestring, "ON") == 0) {
            g_strip.socket_on[id-1] = true;
			printf("\r\nLED_ON\r\n");
            // TODO: RA6M5 控制 GPIO 输出高电平
        } else {
            g_strip.socket_on[id-1] = false;
			printf("\r\nLED_OFF\r\n");
            // TODO: RA6M5 控制 GPIO 输出低电平
        }

        // 3. 立即发送命令回执 (ACK) [cite: 113-118]
        cJSON *ack = cJSON_CreateObject();
        cJSON_AddStringToObject(ack, "cmdId", cmdId->valuestring);
        cJSON_AddStringToObject(ack, "status", "success");
        cJSON_AddNumberToObject(ack, "costMs", 50);
        
        char *ack_out = cJSON_PrintUnformatted(ack);
        if(ack_out) {
            Send_Data_Raw("dorm/A-303/strip01/ack", ack_out);
            free(ack_out);
        }
        cJSON_Delete(ack);
        
        // 4. 控制完后，建议立即触发一次状态上报，让网页端秒刷新
        upload_strip_status();
    }
    cJSON_Delete(root);
}


void handle_uart_json_stream(void)
{
    static uint16_t pos = 0;
    static int brace_count = 0; // 新增：大括号计数器
    uint8_t temp_byte;

    while (g_rx_buf.get(&g_rx_buf, &temp_byte) == 0)
    {
        if (pos < MAX_JSON_SIZE - 1) {
            json_process_buf[pos++] = (char)temp_byte;
        }

        // 1. 监控大括号，判断 JSON 是否完整
        if (temp_byte == '{') brace_count++;
        if (temp_byte == '}') brace_count--;

        // 2. 只有当我们在缓冲区里发现了 +MQTTSUBRECV 
        //    并且大括号成对闭合（brace_count == 0）且 pos > 0 时，才处理
        if (brace_count == 0 && pos > 0 && strstr(json_process_buf, "+MQTTSUBRECV")) 
        {
            json_process_buf[pos] = '\0'; 
            
            // 调试打印：看看这次是不是完整的
            printf("Full Packet: %s\n", json_process_buf);
            
            process_cloud_cmd(json_process_buf);
            
            // 处理完清空
            pos = 0; 
            brace_count = 0;
            memset(json_process_buf, 0, MAX_JSON_SIZE); 
        }
        
        // 安全机制：如果 buffer 满了还没闭合，强制重置，防止卡死
        if (pos >= MAX_JSON_SIZE - 1) {
            pos = 0;
            brace_count = 0;
        }
    }
}