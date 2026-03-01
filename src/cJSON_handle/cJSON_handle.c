#include "cJSON.h"
#include <string.h>
#include "circle_buf.h"
#include  "cJSON_handle.h"
#include "stdlib.h"
#include "bsp_wifi_esp8266.h"
#include "bsp_led.h"
#include "Systick.h"
#include "appliance_identification.h"
extern circle_buf_t g_rx_buf;
#define MAX_JSON_SIZE 512
static char json_process_buf[MAX_JSON_SIZE]; // 线性缓冲区，用于解析
extern _Bool  Uart2_Send_Flag;
 extern uint8_t g_force_upload_flag;
 int brace_count = 0; // 新增：大括号计数器


// 假设全局变量定义
PowerStrip_t g_strip = {
    .voltage = 220.0f,
    .total_current = 0.0f,
    .total_power = 0.0f,
    .sockets = {
        // {on, power, device_name, prev_power}
        {false, 0.0f, "None"}, 
        {false, 0.0f, "None"},
        {false, 0.0f, "None"},
        {false, 0.0f, "None"}
    }
};


void upload_strip_status(void) 
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
	

    // 1. 填充基础电参 [cite: 63-67]
    cJSON_AddNumberToObject(root, "ts", 1772000100); // 实际应为动态时间戳
    cJSON_AddBoolToObject(root, "online", true);
    cJSON_AddNumberToObject(root, "total_power_w", g_strip.total_power);
    cJSON_AddNumberToObject(root, "voltage_v", g_strip.voltage);
    cJSON_AddNumberToObject(root, "current_a", g_strip.total_current);

    // 2. 创建插口数组 [cite: 68-70, 92]
    cJSON *sockets = cJSON_CreateArray();
    for(int i=0; i<4; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", i+1);
        cJSON_AddBoolToObject(item, "on", g_strip.sockets[i].on);
        
        // 如果开关关了，强行报 0.0，防止计量芯片温漂产生的小数值干扰后端
        cJSON_AddNumberToObject(item, "power_w", g_strip.sockets[i].on ? g_strip.sockets[i].power : 0.0);
        
        cJSON_AddStringToObject(item, "device", g_strip.sockets[i].device_name);
        
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
   // 1. 寻找 JSON 的起始大括号
    char *json_start = strchr(json_str, '{');
    if (!json_start) return;

    // 2. 寻找最后一个结束大括号
    char *json_end = strrchr(json_start, '}');
    if (!json_end) return;

    // 3. 临时截断字符串，确保 cJSON 只看到 {} 里的内容
    char backup = *(json_end + 1);
    *(json_end + 1) = '\0';

    cJSON *root = cJSON_Parse(json_start);
    if (!root) {
        // 如果解析失败，把那一串东西打印出来看看是不是被截断了
        printf("[JSON Error] 内容太杂导致解析失败!\n");
        *(json_end + 1) = backup; // 恢复原样
        return;
    }

    // 1. 获取 cmdId (用于 ACK 回执) [cite: 53, 121]
    cJSON *cmdId = cJSON_GetObjectItem(root, "cmdId");
    cJSON *type = cJSON_GetObjectItem(root, "type");       // ON/OFF [cite: 54]
    cJSON *socketId = cJSON_GetObjectItem(root, "socketId"); // 插孔号 [cite: 55]

    
	 if (cmdId && type && socketId) 
		{
        // 将 ID 转为 0-3 的数组索引
        int id_val = socketId->valueint;
        int index = id_val - 1; 

        // 3. 安全边界检查：防止下发 0 或 5 以上的 ID 导致数组越界
        if (index >= 0 && index < 4) 
		{
            bool is_on = (strcmp(type->valuestring, "ON") == 0);

            // 更新硬件状态结构体
            // 假设你使用的是 g_strip.socket_on[4] 或者 g_strip.sockets[index].on
            g_strip.sockets[index].on = is_on;
			if(g_strip.sockets[index].on ==true)
			{
				Socket_Command_Handler(index, true);
			}else
			{
				Socket_Command_Handler(index, false);
			}

            // 4. 打印对应的 LED 亮灭状态（满足你的需求）
            printf("\r\n[Hardware Control] 插座 %d 状态更新!\r\n", id_val);
            printf(">>> 物理插座 ID: %d, LED 状态: %s\r\n", id_val, is_on ? "【亮 (ON)】" : "【灭 (OFF)】");
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
        
        // 关键修改：不要在这里直接 upload_strip_status() !!
        // 设置一个标志位，让 main 循环去处理发送
        g_force_upload_flag = 1;
    }
    cJSON_Delete(root);
    *(json_end + 1) = backup; // 恢复原样
}



void handle_uart_json_stream(void)
{
    static uint16_t pos = 0;
    static int brace_count = 0; 
    static uint32_t last_byte_time = 0;
    uint8_t temp_byte;

    while (g_rx_buf.get(&g_rx_buf, &temp_byte) == 0)
    {
		
        last_byte_time = HAL_GetTick(); 

        if (pos < MAX_JSON_SIZE - 1)
		{
            json_process_buf[pos++] = (char)temp_byte;
            json_process_buf[pos] = '\0';
        }

        // --- 核心调试打印：看到底收到了什么字符 ---
        // 如果这里没打印字符，说明中断没把数据放进 ring buffer
        // printf("%c", temp_byte); 

        if (temp_byte == '{') 
		{
            brace_count++;
        }
        else if (temp_byte == '}') 
		{
            if (brace_count > 0) brace_count--;
            
            // 当括号闭合时，强制打印缓冲区内容进行检查
            if (brace_count == 0 && pos > 0) 
			{
                printf("\r\n[Parser] 捕获到完整括号对，当前缓冲区: %s\r\n", json_process_buf);
                
                if (strstr(json_process_buf, "+MQTTSUBRECV"))
				{
                    process_cloud_cmd(json_process_buf);
                } else 
				{
                    printf("[Parser] 未发现 +MQTTSUBRECV 标志，丢弃\r\n");
                }
                
                // 彻底重置
                pos = 0;
                memset(json_process_buf, 0, MAX_JSON_SIZE);
            }
        }
        
        // 超时重置逻辑（适当放宽到 1s）
        if (pos > 0 && (HAL_GetTick() - last_byte_time > 1000)) 
		{
            pos = 0;
            brace_count = 0;
            memset(json_process_buf, 0, MAX_JSON_SIZE);
        }
    }

}

