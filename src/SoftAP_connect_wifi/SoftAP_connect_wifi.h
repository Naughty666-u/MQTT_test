#ifndef __SOFTAP_CONNECT_WIFI_H__
#define __SOFTAP_CONNECT_WIFI_H__

#include <stdbool.h>

/*
 * 这里的头文件只放模块对外接口。
 *
 * 像 SoftAP 发送队列、联网状态机、HTTP 解析这类 `static` 内部函数，
 * 仍然保留在 `SoftAP_connect_wifi.c` 内部声明和定义。
 * 这样可以避免把模块内部实现细节暴露给其他文件。
 */

/* 网络管理初始化：
 * 1) 先尝试按当前凭据连接 WiFi/MQTT
 * 2) 失败则进入降级模式，并由 Task 周期重试
 */
void NET_Manager_Init(void);

/* 网络管理任务（主循环高频调用）：
 * - 处理 SoftAP 配网收包
 * - 处理重连退避
 * - 多次失败后进入 SoftAP 配网模式
 */
void NET_Manager_Task(void);

/* 当前网络是否已就绪（WiFi+MQTT） */
bool NET_Manager_IsReady(void);

/* 读取当前用于 STA 连接的 WiFi 凭据 */
void NET_Manager_GetWiFiCredentials(const char **ssid, const char **password);

/* 当前是否处于 SoftAP 配网模式（供 UART 回调做数据分流） */
bool NET_Manager_IsProvisioningRunning(void);

#endif
