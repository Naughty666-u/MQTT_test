#ifndef __BSP_WIFI_ESP8266_H
#define __BSP_WIFI_ESP8266_H
#include "hal_data.h"



/*宏定义调试信息*/
#define ESP8266_DEBUG   1

#if     (ESP8266_DEBUG == 1)
#define     ESP8266_DEBUG_MSG(fmt, ... )        printf ( fmt, ##__VA_ARGS__ )
#else
#define     ESP8266_DEBUG_MSG(fmt, ... )
#endif

// WiFi 配置
#define ID              "大宝贝儿"
#define PASSWORD        "b37z198v4479"

// MQTT 服务器 (参照文档第5行)
#define MQTT_IP         "175.27.162.174"  // [cite: 5]
#define MQTT_Port       "1883"            // [cite: 5]

// 设备身份定义 (参照文档第24-26行)
#define ROOM_ID         "A-303"
#define DEVICE_NAME     "strip01"

// MQTT 用户属性 (根据实际后端要求填写，如果文档没写就先用原有的)
#define CLIENT_ID       "QiMing"
#define USER_NAME       "ESP8266"
#define USER_PASSWORD   "1234567"

// --- 核心 Topic 修改 ---
// 订阅：后端下发命令的主题 [cite: 29]
#define MQTT_SUB_TOPIC  "dorm/" ROOM_ID "/" DEVICE_NAME "/cmd" 

// 发布：状态上报主题 [cite: 60]
#define MQTT_PUB_STATUS "dorm/" ROOM_ID "/" DEVICE_NAME "/status"

// 发布：命令回执主题 [cite: 112]
#define MQTT_PUB_ACK    "dorm/" ROOM_ID "/" DEVICE_NAME "/ack"

// 遗嘱：离线告警主题 [cite: 146]
#define MQTT_WILL_TOPIC "dorm/" ROOM_ID "/" DEVICE_NAME "/lwt"

#define ESP8266_MODULE_ENABLE     R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_14, BSP_IO_LEVEL_HIGH);  //使能ESP8266模块
#define ESP8266_MODULE_DISABLE    R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_14, BSP_IO_LEVEL_LOW);   //关闭ESP8266模块

/*红灯闪烁*/
#define ESP8266_ERROR_Alarm()     R_PORT6->PODR ^= 1<<(BSP_IO_PORT_06_PIN_08 & 0xFF); \
                                  R_BSP_SoftwareDelay(500, BSP_DELAY_UNITS_MILLISECONDS);


/*清除UART2数据缓冲区函数*/
#define   Clear_Buff()   memset( At_Rx_Buff , 0 , sizeof(At_Rx_Buff) ); \
                         Uart2_Num = 0;
void Send_Data_Raw( char * topics , char * data );
void  ESP8266_Hard_Reset(void);
void ESP8266_MQTT_Test(void);
void ESP8266_UART2_Init(void);
void ESP8266_AT_Send(char * cmd );
void ESP8266_Rst(void);
void ESP8266_STA ( void );
void ESP8266_AP ( void );
void ESP8266_STA_AP ( void );
void ESP8266_STA_JoinAP( char * id ,  char * password , uint8_t timeout );
void MQTT_SetUserProperty( char * client_id , char * user_name, char * user_password );
void Connect_MQTT( char * mqtt_ip , char * mqtt_port , uint8_t timeout );
void Subscribes_Topics( char * topics );
void Send_Data( char * topics , char * data );
void Process_RingBuffer_Data(void);
int ESP8266_ATE0(void);
#endif
