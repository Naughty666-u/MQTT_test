#ifndef __BSP_WIFI_ESP8266_H
#define __BSP_WIFI_ESP8266_H
#include "hal_data.h"



/* 调试开关 */
#define ESP8266_DEBUG   1

#if     (ESP8266_DEBUG == 1)
#define     ESP8266_DEBUG_MSG(fmt, ... )        printf ( fmt, ##__VA_ARGS__ )
#else
#define     ESP8266_DEBUG_MSG(fmt, ... )
#endif

// WiFi 配置
#define ID              "OPPO Reno12"
#define PASSWORD        "88888888"

// MQTT 服务器
#define MQTT_IP         "175.27.162.174"  // [cite: 5]
#define MQTT_Port       "1883"            // [cite: 5]

// 设备身份
#define ROOM_ID         "A-303"
#define DEVICE_NAME     "strip01"

// MQTT 鉴权
#define CLIENT_ID       "QiMing"
#define USER_NAME       "ESP8266"
#define USER_PASSWORD   "1234567"

// --- 核心 Topic 定义 ---
// 订阅：后端下发命令主题
#define MQTT_SUB_TOPIC  "dorm/" ROOM_ID "/" DEVICE_NAME "/cmd" 

// 发布：状态上报主题
#define MQTT_PUB_STATUS "dorm/" ROOM_ID "/" DEVICE_NAME "/status"

// 发布：命令 ACK 主题
#define MQTT_PUB_ACK    "dorm/" ROOM_ID "/" DEVICE_NAME "/ack"

// 遗嘱：离线告警主题
#define MQTT_WILL_TOPIC "dorm/" ROOM_ID "/" DEVICE_NAME "/lwt"

#define ESP8266_MODULE_ENABLE     R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_14, BSP_IO_LEVEL_HIGH);  // 使能 ESP8266 模块
#define ESP8266_MODULE_DISABLE    R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_14, BSP_IO_LEVEL_LOW);   // 关闭 ESP8266 模块

/* 错误告警（红灯闪烁） */
#define ESP8266_ERROR_Alarm()     R_PORT6->PODR ^= 1<<(BSP_IO_PORT_06_PIN_08 & 0xFF); \
                                  R_BSP_SoftwareDelay(500, BSP_DELAY_UNITS_MILLISECONDS);


/* 清空 UART2 接收缓冲区 */
#define   Clear_Buff()   memset( At_Rx_Buff , 0 , sizeof(At_Rx_Buff) ); \
                         Uart2_Num = 0;

/*
 * WiFi 子系统初始化入口。
 *
 * 这里不是简单初始化 UART，而是启动整个 ESP8266 网络管理链路：
 * - 初始化联网状态机
 * - 准备后续在主循环中由 WiFi_ServiceTask() 持续推进
 */
void WiFi_Init(void);

/*
 * 裸机版 WiFi 总管函数。
 *
 * 调用方式：
 * - 在主循环中高频调用
 * - 统一推进 ESP8266 相关的收发、联网、SoftAP 配网和 MQTT 下行处理
 *
 * 这样做的目的，是先在裸机阶段建立“只有一个入口能碰 ESP8266”的边界，
 * 后续迁移到 FreeRTOS 时，可直接把本函数放进独立 wifi_task 中运行。
 */
void WiFi_ServiceTask(void);

/*
 * 业务层发布请求入口。
 *
 * 业务模块只表达“我要发布一条 MQTT 消息”，
 * 实际入队和发送仍由 WiFi 子系统内部统一处理。
 */
bool WiFi_RequestPublish(char * topics , char * data );

bool Send_Data_Raw( char * topics , char * data );
void ESP8266_TxTask(void);
bool ESP8266_MQTT_Test(void);
void ESP8266_UART2_Init(void);
void ESP8266_AT_Send(char * cmd );
#endif
