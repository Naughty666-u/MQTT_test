#include "SoftAP_connect_wifi.h"
#include "bsp_esp8266/bsp_wifi_esp8266.h"
#include "circle_buf.h"
#include "log.h"
#include "Systick.h"
#include <stdio.h>
#include <string.h>

extern circle_buf_t g_http_rx_buf;
extern circle_buf_t g_cmd_rx_buf;
extern char At_Rx_Buff[256];
extern uint8_t Uart2_Num;
extern _Bool Uart2_Send_Flag;

#define WIFI_SSID_MAX_LEN  32U
#define WIFI_PASS_MAX_LEN  64U
#define SOFTAP_HTTP_BODY_MAX_LEN          1400U
#define SOFTAP_HTTP_CONTENT_TYPE_MAX_LEN    48U
#define SOFTAP_HTTP_HEADER_MAX_LEN        1600U
#define SOFTAP_HTTP_CMD_MAX_LEN             64U
#define SOFTAP_HTTP_TX_QUEUE_DEPTH           2U
#define SOFTAP_HTTP_PROMPT_TIMEOUT_MS     1000U
#define SOFTAP_HTTP_SEND_OK_TIMEOUT_MS    1000U
#define SOFTAP_HTTP_CLOSE_TIMEOUT_MS       500U
#define NET_CONN_CMD_MAX_LEN               256U
#define NET_CONN_RESET_LOW_MS              100U
#define NET_CONN_RESET_READY_TIMEOUT_MS   5000U
#define NET_CONN_READY_PROBE_TIMEOUT_MS   1000U
#define NET_CONN_STA_TIMEOUT_MS           1000U
#define NET_CONN_ATE0_TIMEOUT_MS           500U
#define NET_CONN_STAGE_GAP_MS              500U
#define NET_CONN_RFPOWER_SETTLE_MS         200U
#define NET_CONN_JOIN_AP_TIMEOUT_MS      20000U
#define NET_CONN_MQTT_USERCFG_TIMEOUT_MS  3000U
#define NET_CONN_MQTT_CONN_TIMEOUT_MS   100000U
#define NET_CONN_SUB_TIMEOUT_MS           3000U

/*
 * SoftAP 配网与 STA/MQTT 联网统一管理模块。
 *
 * 这个文件同时负责三件事：
 * 1. 常规上电后按当前凭据连接路由器和 MQTT Broker。
 * 2. 连续失败后切到 SoftAP，开放一个极简 HTTP 页面收集新凭据。
 * 3. 收到新凭据后退出 SoftAP，再次走 STA/MQTT 联网状态机。
 *
 * 之所以把流程集中在同一文件，是为了让串口 AT 收发、SoftAP 切换、
 * HTTP 应答和重连退避共享同一组状态，避免多个模块互相抢占 ESP8266。
 */
static char g_wifi_ssid[WIFI_SSID_MAX_LEN + 1U] = ID;
static char g_wifi_pass[WIFI_PASS_MAX_LEN + 1U] = PASSWORD;

/* WiFi 与 MQTT 都连通后置位，主循环据此跳过后续联网动作。 */
static bool g_net_ready = false;
/* 记录当前失败次数，用于决定何时从普通重连降级到 SoftAP 配网。 */
static uint8_t g_net_retry_idx = 0;
/* 下一次允许发起重连的时间戳，避免主循环高频重复打 AT。 */
static uint32_t g_net_next_retry_tick = 0;
/* SoftAP 配网页面已打开时为 true，UART 回调会把 HTTP 数据分流到专用缓冲区。 */
static bool g_softap_provision_running = false;
/* 配网页面成功写入新凭据后置位，由主循环取走并触发退出 SoftAP。 */
static volatile bool g_provision_ready = false;

/* SoftAP HTTP 发送队列的单个响应项。 */
typedef struct
{
    int link_id;
    int status_code;
    char content_type[SOFTAP_HTTP_CONTENT_TYPE_MAX_LEN];
    char body[SOFTAP_HTTP_BODY_MAX_LEN];
} softap_http_resp_t;

/* HTTP 响应发送状态机。ESP8266 需要先等 '>'，再发正文，再等 SEND OK，最后关闭连接。 */
typedef enum
{
    SOFTAP_HTTP_TX_IDLE = 0,
    SOFTAP_HTTP_TX_WAIT_PROMPT,
    SOFTAP_HTTP_TX_WAIT_BODY_SENT,
    SOFTAP_HTTP_TX_WAIT_SEND_OK,
    SOFTAP_HTTP_TX_WAIT_CLOSE_OK,
} softap_http_tx_state_t;

/* SoftAP 开关状态机。 */
typedef enum
{
    SOFTAP_CFG_IDLE = 0,
    SOFTAP_CFG_START_WAIT_MODE,
    SOFTAP_CFG_START_WAIT_AP,
    SOFTAP_CFG_START_WAIT_MUX,
    SOFTAP_CFG_START_WAIT_SERVER,
    SOFTAP_CFG_STOP_WAIT_SERVER_OFF,
    SOFTAP_CFG_STOP_WAIT_STA_MODE,
} softap_cfg_state_t;

/* SoftAP 状态机单步执行结果。 */
typedef enum
{
    SOFTAP_STEP_FAIL = -1,
    SOFTAP_STEP_PENDING = 0,
    SOFTAP_STEP_DONE = 1,
} softap_step_result_t;

/*
 * 普通联网状态机。
 *
 * 顺序基本等价于旧版阻塞式 `ESP8266_MQTT_Test()`：
 * 复位模组 -> 切 STA -> 关闭回显 -> 设置射频功率 ->
 * 连接路由器 -> 配置 MQTT 账户 -> 连接 Broker -> 订阅主题。
 */
typedef enum
{
    NET_CONN_IDLE = 0,
    NET_CONN_RESET_LOW_WAIT,
    NET_CONN_RESET_READY_WAIT,
    NET_CONN_READY_PROBE_WAIT_OK,
    NET_CONN_STA_WAIT_OK,
    NET_CONN_ATE0_GAP_WAIT,
    NET_CONN_ATE0_WAIT_OK,
    NET_CONN_RFPOWER_GAP_WAIT,
    NET_CONN_RFPOWER_SETTLE_WAIT,
    NET_CONN_JOIN_AP_WAIT_OK,
    NET_CONN_MQTT_USERCFG_WAIT_OK,
    NET_CONN_MQTT_CONN_WAIT_OK,
    NET_CONN_MQTT_SUB_WAIT_OK,
} net_conn_state_t;

/* 联网状态机单步执行结果。 */
typedef enum
{
    NET_CONN_STEP_FAIL = -1,
    NET_CONN_STEP_PENDING = 0,
    NET_CONN_STEP_DONE = 1,
} net_conn_step_result_t;

static softap_http_resp_t g_softap_http_tx_q[SOFTAP_HTTP_TX_QUEUE_DEPTH];
static uint8_t g_softap_http_tx_q_head = 0;
static uint8_t g_softap_http_tx_q_tail = 0;
static uint8_t g_softap_http_tx_q_count = 0;

static softap_http_resp_t g_softap_http_tx_active;
static bool g_softap_http_tx_has_active = false;
static softap_http_tx_state_t g_softap_http_tx_state = SOFTAP_HTTP_TX_IDLE;
static uint32_t g_softap_http_tx_deadline = 0;
static char g_softap_http_header_buf[SOFTAP_HTTP_HEADER_MAX_LEN];
static char g_softap_http_cmd_buf[SOFTAP_HTTP_CMD_MAX_LEN];
static softap_cfg_state_t g_softap_cfg_state = SOFTAP_CFG_IDLE;
static uint32_t g_softap_cfg_deadline = 0;

static net_conn_state_t g_net_conn_state = NET_CONN_IDLE;
static uint32_t g_net_conn_deadline = 0;
static char g_net_conn_cmd_buf[NET_CONN_CMD_MAX_LEN];

/* 内部 helper 保持 `static`，避免把状态机实现细节暴露到头文件。 */
static softap_step_result_t softap_start(void);
static softap_step_result_t softap_stop(void);
static void softap_provision_task(void);
static void softap_http_tx_task(void);
static void net_connect_start(void);
static bool net_connect_is_running(void);
static net_conn_step_result_t net_connect_task(void);
static void net_connect_begin_sta_mode(void);
static void softap_send_response_ex(int link_id,
                                    int status_code,
                                    const char *content_type,
                                    const char *body_text);

static bool softap_http_tx_queue_push(int link_id,
                                      int status_code,
                                      const char *content_type,
                                      const char *body_text)
{
    /* 队列满时直接丢弃，防止 SoftAP 侧响应挤爆固定长度缓冲区。 */
    if ((link_id < 0) || (g_softap_http_tx_q_count >= SOFTAP_HTTP_TX_QUEUE_DEPTH))
    {
        return false;
    }

    softap_http_resp_t *slot = &g_softap_http_tx_q[g_softap_http_tx_q_tail];
    memset(slot, 0, sizeof(*slot));
    slot->link_id = link_id;
    slot->status_code = status_code;
    snprintf(slot->content_type,
             sizeof(slot->content_type),
             "%s",
             (content_type != NULL) ? content_type : "text/plain");
    snprintf(slot->body,
             sizeof(slot->body),
             "%s",
             (body_text != NULL) ? body_text : "");

    g_softap_http_tx_q_tail =
        (uint8_t)((g_softap_http_tx_q_tail + 1U) % SOFTAP_HTTP_TX_QUEUE_DEPTH);
    g_softap_http_tx_q_count++;
    return true;
}

static bool softap_http_tx_queue_pop(softap_http_resp_t *out)
{
    if ((out == NULL) || (g_softap_http_tx_q_count == 0U))
    {
        return false;
    }

    *out = g_softap_http_tx_q[g_softap_http_tx_q_head];
    g_softap_http_tx_q_head =
        (uint8_t)((g_softap_http_tx_q_head + 1U) % SOFTAP_HTTP_TX_QUEUE_DEPTH);
    g_softap_http_tx_q_count--;
    return true;
}

static void softap_send_response_ex(int link_id,
                                    int status_code,
                                    const char *content_type,
                                    const char *body_text)
{
    if (!softap_http_tx_queue_push(link_id, status_code, content_type, body_text))
    {
        LOGW("[PROV] http queue full, drop resp link=%d status=%d\r\n",
             link_id,
             status_code);
    }
}

static bool softap_cfg_is_starting(void)
{
    return (g_softap_cfg_state == SOFTAP_CFG_START_WAIT_MODE) ||
           (g_softap_cfg_state == SOFTAP_CFG_START_WAIT_AP) ||
           (g_softap_cfg_state == SOFTAP_CFG_START_WAIT_MUX) ||
           (g_softap_cfg_state == SOFTAP_CFG_START_WAIT_SERVER);
}

static bool softap_cfg_is_stopping(void)
{
    return (g_softap_cfg_state == SOFTAP_CFG_STOP_WAIT_SERVER_OFF) ||
           (g_softap_cfg_state == SOFTAP_CFG_STOP_WAIT_STA_MODE);
}

static void softap_cfg_begin_command(const char *cmd,
                                     softap_cfg_state_t next_state,
                                     uint32_t timeout_ms)
{
    Clear_Buff();
    ESP8266_AT_Send((char *)cmd);
    g_softap_cfg_state = next_state;
    g_softap_cfg_deadline = HAL_GetTick() + timeout_ms;
}

static void net_connect_begin_command(const char *cmd,
                                      net_conn_state_t next_state,
                                      uint32_t timeout_ms)
{
    Clear_Buff();
    ESP8266_AT_Send((char *)cmd);
    g_net_conn_state = next_state;
    g_net_conn_deadline = HAL_GetTick() + timeout_ms;
}

static void net_connect_reset_state(void)
{
    g_net_conn_state = NET_CONN_IDLE;
    g_net_conn_deadline = 0;
    Clear_Buff();
}

/*
 * 进入 STA 配置阶段。
 *
 * 这里单独抽成一个 helper，是因为现在有两条路径都会进入切 STA：
 * 1. 正常收到了上电 `ready`
 * 2. `ready` 没收到，但补发 `AT` 后确认模块已经能响应
 *
 * 统一从这里发 `AT+CWMODE=1`，可以避免两条路径后续逻辑分叉。
 */
static void net_connect_begin_sta_mode(void)
{
    ESP8266_DEBUG_MSG("[NET] switch STA mode\r\n");
    net_connect_begin_command("AT+CWMODE=1\r\n",
                              NET_CONN_STA_WAIT_OK,
                              NET_CONN_STA_TIMEOUT_MS);
}

/*
 * 联网成功收尾。
 *
 * 这里会额外清空命令缓冲和 HTTP 缓冲，避免 SoftAP 残留数据污染后续 MQTT 收发。
 */
static net_conn_step_result_t net_connect_finish_success(void)
{
    uint8_t dummy;

    net_connect_reset_state();
    while (g_cmd_rx_buf.get(&g_cmd_rx_buf, &dummy) == 0) {}
    while (g_http_rx_buf.get(&g_http_rx_buf, &dummy) == 0) {}
    g_net_ready = true;
    return NET_CONN_STEP_DONE;
}

/* 联网失败收尾，只复位状态机，不修改重试计数，由上层统一决定退避策略。 */
static net_conn_step_result_t net_connect_finish_fail(void)
{
    net_connect_reset_state();
    return NET_CONN_STEP_FAIL;
}

/*
 * 启动联网状态机。
 *
 * 入口会先把 ESP8266 拉低复位脚，再等待模组上报 `ready`，
 * 后续所有 AT 步骤都由 `net_connect_task()` 非阻塞推进。
 */
static void net_connect_start(void)
{
    ESP8266_DEBUG_MSG("\r\n[NET] start connect flow...\r\n");
    ESP8266_UART2_Init();
    Clear_Buff();
    R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_14, BSP_IO_LEVEL_LOW);
    g_net_conn_state = NET_CONN_RESET_LOW_WAIT;
    g_net_conn_deadline = HAL_GetTick() + NET_CONN_RESET_LOW_MS;
}

static bool net_connect_is_running(void)
{
    return (g_net_conn_state != NET_CONN_IDLE);
}

/*
 * 非阻塞推进 WiFi + MQTT 联网状态机。
 *
 * 调用时机：主循环周期调用，或阻塞测试函数里轮询调用。
 * 返回值：
 * - `NET_CONN_STEP_PENDING` 表示当前步骤还在等待串口回复或等待超时点。
 * - `NET_CONN_STEP_DONE` 表示 WiFi 与 MQTT 都已就绪。
 * - `NET_CONN_STEP_FAIL` 表示本轮失败，由上层决定是否重试或切 SoftAP。
 */
static net_conn_step_result_t net_connect_task(void)
{
    const char *cfg_ssid = NULL;
    const char *cfg_pass = NULL;
    uint32_t now = HAL_GetTick();

    switch (g_net_conn_state)
    {
        case NET_CONN_IDLE:
            return NET_CONN_STEP_FAIL;

        case NET_CONN_RESET_LOW_WAIT:
            /* 先保持一段低电平，确保 ESP8266 真正完成硬复位。 */
            if ((int32_t)(now - g_net_conn_deadline) >= 0)
            {
                R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_14, BSP_IO_LEVEL_HIGH);
                Clear_Buff();
                g_net_conn_state = NET_CONN_RESET_READY_WAIT;
                g_net_conn_deadline = now + NET_CONN_RESET_READY_TIMEOUT_MS;
                ESP8266_DEBUG_MSG("[NET] wait module ready\r\n");
            }
            return NET_CONN_STEP_PENDING;

        case NET_CONN_RESET_READY_WAIT:
            /* 只有等到 `ready` 才开始发第一条 AT，避免复位后首条命令丢失。 */
            if (strstr(At_Rx_Buff, "ready") != NULL)
            {
                ESP8266_DEBUG_MSG("[NET] module ready\r\n");
                net_connect_begin_sta_mode();
                return NET_CONN_STEP_PENDING;
            }
            if ((int32_t)(now - g_net_conn_deadline) >= 0)
            {
                /*
                 * 某些模组在硬复位后不会稳定打印 `ready`，但 AT 口其实已经可用。
                 * 因此这里先补发一次 `AT` 探测，避免把“没看到启动日志”误判成“模组未就绪”。
                 */
                ESP8266_DEBUG_MSG("[NET] wait ready timeout, probe AT\r\n");
                net_connect_begin_command("AT\r\n",
                                          NET_CONN_READY_PROBE_WAIT_OK,
                                          NET_CONN_READY_PROBE_TIMEOUT_MS);
                return NET_CONN_STEP_PENDING;
            }
            return NET_CONN_STEP_PENDING;

        case NET_CONN_READY_PROBE_WAIT_OK:
            /*
             * 探测 AT 成功，说明模块已经能收发命令，只是启动阶段没有上报 `ready`。
             * 此时继续后续 STA/MQTT 流程，不再额外复位一次。
             */
            if (strstr(At_Rx_Buff, "OK") != NULL)
            {
                ESP8266_DEBUG_MSG("[NET] module responsive without ready\r\n");
                net_connect_begin_sta_mode();
                return NET_CONN_STEP_PENDING;
            }
            if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                ((int32_t)(now - g_net_conn_deadline) >= 0))
            {
                ESP8266_DEBUG_MSG("[NET] AT probe failed after ready timeout\r\n");
                return net_connect_finish_fail();
            }
            return NET_CONN_STEP_PENDING;

        case NET_CONN_STA_WAIT_OK:
            if (strstr(At_Rx_Buff, "OK") != NULL)
            {
                Clear_Buff();
                g_net_conn_state = NET_CONN_ATE0_GAP_WAIT;
                g_net_conn_deadline = now + NET_CONN_STAGE_GAP_MS;
                ESP8266_DEBUG_MSG("[NET] STA mode configured\r\n");
                return NET_CONN_STEP_PENDING;
            }
            if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                ((int32_t)(now - g_net_conn_deadline) >= 0))
            {
                ESP8266_DEBUG_MSG("[NET] STA mode configure failed\r\n");
                return net_connect_finish_fail();
            }
            return NET_CONN_STEP_PENDING;

        case NET_CONN_ATE0_GAP_WAIT:
            if ((int32_t)(now - g_net_conn_deadline) >= 0)
            {
                ESP8266_DEBUG_MSG("[NET] send ATE0\r\n");
                net_connect_begin_command("ATE0\r\n",
                                          NET_CONN_ATE0_WAIT_OK,
                                          NET_CONN_ATE0_TIMEOUT_MS);
            }
            return NET_CONN_STEP_PENDING;

        case NET_CONN_ATE0_WAIT_OK:
            if (strstr(At_Rx_Buff, "OK") != NULL)
            {
                Clear_Buff();
                g_net_conn_state = NET_CONN_RFPOWER_GAP_WAIT;
                g_net_conn_deadline = now + NET_CONN_STAGE_GAP_MS;
                ESP8266_DEBUG_MSG("[NET] echo disabled\r\n");
                return NET_CONN_STEP_PENDING;
            }
            if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                ((int32_t)(now - g_net_conn_deadline) >= 0))
            {
                ESP8266_DEBUG_MSG("[NET] disable echo failed\r\n");
                return net_connect_finish_fail();
            }
            return NET_CONN_STEP_PENDING;

        case NET_CONN_RFPOWER_GAP_WAIT:
            if ((int32_t)(now - g_net_conn_deadline) >= 0)
            {
                Clear_Buff();
                ESP8266_AT_Send("AT+RFPOWER=50\r\n");
                g_net_conn_state = NET_CONN_RFPOWER_SETTLE_WAIT;
                g_net_conn_deadline = now + NET_CONN_RFPOWER_SETTLE_MS;
                ESP8266_DEBUG_MSG("[NET] set RF power\r\n");
            }
            return NET_CONN_STEP_PENDING;

        case NET_CONN_RFPOWER_SETTLE_WAIT:
            if ((int32_t)(now - g_net_conn_deadline) >= 0)
            {
                NET_Manager_GetWiFiCredentials(&cfg_ssid, &cfg_pass);
                snprintf(g_net_conn_cmd_buf,
                         sizeof(g_net_conn_cmd_buf),
                         "AT+CWJAP=\"%s\",\"%s\"\r\n",
                         (cfg_ssid != NULL) ? cfg_ssid : "",
                         (cfg_pass != NULL) ? cfg_pass : "");
                ESP8266_DEBUG_MSG("[NET] join WiFi: %s\r\n",
                                  (cfg_ssid != NULL) ? cfg_ssid : "");
                net_connect_begin_command(g_net_conn_cmd_buf,
                                          NET_CONN_JOIN_AP_WAIT_OK,
                                          NET_CONN_JOIN_AP_TIMEOUT_MS);
            }
            return NET_CONN_STEP_PENDING;

        case NET_CONN_JOIN_AP_WAIT_OK:
            if (strstr(At_Rx_Buff, "OK\r\n") != NULL)
            {
                ESP8266_DEBUG_MSG("[NET] WiFi connected, config MQTT\r\n");
                snprintf(g_net_conn_cmd_buf,
                         sizeof(g_net_conn_cmd_buf),
                         "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"\r\n",
                         CLIENT_ID,
                         USER_NAME,
                         USER_PASSWORD);
                net_connect_begin_command(g_net_conn_cmd_buf,
                                          NET_CONN_MQTT_USERCFG_WAIT_OK,
                                          NET_CONN_MQTT_USERCFG_TIMEOUT_MS);
                return NET_CONN_STEP_PENDING;
            }
            if ((strstr(At_Rx_Buff, "ERROR\r\n") != NULL) ||
                ((int32_t)(now - g_net_conn_deadline) >= 0))
            {
                
                /* 按 ESP8266 的 +CWJAP 错误码输出更具体原因，便于现场排查。 */
                if (strstr(At_Rx_Buff, "+CWJAP:2\r\n") != NULL)
                {
                    ESP8266_DEBUG_MSG("[NET] WiFi error: AP not found\r\n");
                }
                else if (strstr(At_Rx_Buff, "+CWJAP:3\r\n") != NULL)
                {
                    ESP8266_DEBUG_MSG("[NET] WiFi error: auth failed\r\n");
                }
                else if (strstr(At_Rx_Buff, "+CWJAP:4\r\n") != NULL)
                {
                    ESP8266_DEBUG_MSG("[NET] WiFi error: connect timeout\r\n");
                }
                else
                {
                    ESP8266_DEBUG_MSG("[NET] WiFi connect failed\r\n");
                }
                return net_connect_finish_fail();
            }
            return NET_CONN_STEP_PENDING;

        case NET_CONN_MQTT_USERCFG_WAIT_OK:
            if (strstr(At_Rx_Buff, "OK") != NULL)
            {
                ESP8266_DEBUG_MSG("[NET] MQTT cfg done, connect broker\r\n");
                snprintf(g_net_conn_cmd_buf,
                         sizeof(g_net_conn_cmd_buf),
                         "AT+MQTTCONN=0,\"%s\",%s,1\r\n",
                         MQTT_IP,
                         MQTT_Port);
                net_connect_begin_command(g_net_conn_cmd_buf,
                                          NET_CONN_MQTT_CONN_WAIT_OK,
                                          NET_CONN_MQTT_CONN_TIMEOUT_MS);
                return NET_CONN_STEP_PENDING;
            }
            if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                ((int32_t)(now - g_net_conn_deadline) >= 0))
            {
                ESP8266_DEBUG_MSG("[NET] MQTT user cfg failed\r\n");
                return net_connect_finish_fail();
            }
            return NET_CONN_STEP_PENDING;

        case NET_CONN_MQTT_CONN_WAIT_OK:
            if (strstr(At_Rx_Buff, "OK\r\n") != NULL)
            {
                ESP8266_DEBUG_MSG("[NET] broker connected, subscribe topic\r\n");
                snprintf(g_net_conn_cmd_buf,
                         sizeof(g_net_conn_cmd_buf),
                         "AT+MQTTSUB=0,\"%s\",1\r\n",
                         MQTT_SUB_TOPIC);
                net_connect_begin_command(g_net_conn_cmd_buf,
                                          NET_CONN_MQTT_SUB_WAIT_OK,
                                          NET_CONN_SUB_TIMEOUT_MS);
                return NET_CONN_STEP_PENDING;
            }
            if ((strstr(At_Rx_Buff, "ERROR\r\n") != NULL) ||
                ((int32_t)(now - g_net_conn_deadline) >= 0))
            {
                ESP8266_DEBUG_MSG("[NET] broker connect failed\r\n");
                return net_connect_finish_fail();
            }
            return NET_CONN_STEP_PENDING;

        case NET_CONN_MQTT_SUB_WAIT_OK:
            if ((strstr(At_Rx_Buff, "OK") != NULL) ||
                (strstr(At_Rx_Buff, "ALREADY") != NULL))
            {
                ESP8266_DEBUG_MSG("[NET] MQTT subscribe done\r\n");
                return net_connect_finish_success();
            }
            if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                ((int32_t)(now - g_net_conn_deadline) >= 0))
            {
                ESP8266_DEBUG_MSG("[NET] MQTT subscribe failed\r\n");
                return net_connect_finish_fail();
            }
            return NET_CONN_STEP_PENDING;

        default:
            return net_connect_finish_fail();
    }
}

/* 兼容旧调用方式的阻塞包装，内部仍复用新的非阻塞状态机。 */
bool ESP8266_MQTT_Test(void)
{
    net_connect_start();
    while (net_connect_is_running())
    {
        net_conn_step_result_t step = net_connect_task();
        if (step == NET_CONN_STEP_DONE)
        {
            return true;
        }
        if (step == NET_CONN_STEP_FAIL)
        {
            return false;
        }
        R_BSP_SoftwareDelay(10, BSP_DELAY_UNITS_MILLISECONDS);
    }
    return false;
}

/* 从简化 JSON 中提取字符串字段，只支持 `{\"key\":\"value\"}` 这类简单场景。 */
static bool extract_json_string_value(const char *json, const char *key, char *out, size_t out_sz)
{
    if ((json == NULL) || (key == NULL) || (out == NULL) || (out_sz == 0U))
    {
        return false;
    }

    char key_pat[40];
    snprintf(key_pat, sizeof(key_pat), "\"%s\"", key);
    char *k = strstr((char *)json, key_pat);
    if (k == NULL) return false;

    char *colon = strchr(k, ':');
    if (colon == NULL) return false;
    char *q1 = strchr(colon, '"');
    if (q1 == NULL) return false;
    q1++;
    char *q2 = strchr(q1, '"');
    if ((q2 == NULL) || (q2 <= q1)) return false;

    /* 始终保留结尾 `\\0`，避免后续拼 AT 命令时越界。 */
    size_t len = (size_t)(q2 - q1);
    if (len >= out_sz) len = out_sz - 1U;
    memcpy(out, q1, len);
    out[len] = '\0';
    return (len > 0U);
}

/* 从 `application/x-www-form-urlencoded` 里提取字段，支持 `ssid=xx&password=yy`。 */
static bool extract_form_value(const char *form, const char *key, char *out, size_t out_sz)
{
    
    if ((form == NULL) || (key == NULL) || (out == NULL) || (out_sz == 0U))
    {
        return false;
    }

    
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=", key);
    char *p = strstr((char *)form, pat);
    if (p == NULL) return false;
    p += strlen(pat);

    
    size_t i = 0;
    while ((p[i] != '\0') && (p[i] != '&') && (i < (out_sz - 1U)))
    {
        char c = p[i];
        
        out[i] = (c == '+') ? ' ' : c;
        i++;
    }
    out[i] = '\0';
    return (i > 0U);
}

static void softap_send_json(int link_id, int status_code, const char *json_body)
{
    softap_send_response_ex(link_id, status_code, "application/json", json_body);
}

/*
 * 处理 SoftAP 页面发来的 HTTP 请求。
 *
 * 数据来源：ESP8266 把 `+IPD` 负载转入 `g_http_rx_buf`，主循环解析后进入这里。
 * 成功路径：提取 SSID/密码 -> 更新全局凭据 -> 置位 `g_provision_ready`。
 * 失败路径：返回 4xx JSON，让网页端知道是路径错误、请求体不完整还是字段缺失。
 */
static void softap_process_http_payload(int link_id, const char *payload)
{
    static const char k_prov_html[] =
        "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Strip WiFi</title><style>body{font-family:sans-serif;padding:20px}input,button{width:100%;padding:10px;margin:8px 0}#msg{color:#333}</style>"
        "</head><body><h3>WiFi Config</h3><input id='ssid' placeholder='WiFi SSID'><input id='pwd' type='password' placeholder='WiFi password'>"
        "<button onclick='go()'>Start Config</button><p id='msg'></p><script>"
        "async function go(){const ssid=document.getElementById('ssid').value;const password=document.getElementById('pwd').value;"
        "const r=await fetch('/provision',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,password})});"
        "const t=await r.text();document.getElementById('msg').innerText=t;}</script></body></html>";

    if ((payload == NULL) || (link_id < 0))
    {
        return;
    }

    /* 首页只提供最小化配网页面，避免 MCU 端维护复杂静态资源。 */
    if ((strstr(payload, "GET / ") != NULL) || (strstr(payload, "GET /index.html") != NULL))
    {
        softap_send_response_ex(link_id, 200, "text/html; charset=utf-8", k_prov_html);
        return;
    }

    /* 当前仅开放两个提交入口，其他路径全部按未找到处理。 */
    if ((strstr(payload, "POST /provision") == NULL) && (strstr(payload, "POST /wifi") == NULL))
    {
        softap_send_json(link_id, 404, "{\"ok\":false,\"reason\":\"not_found\"}");
        return;
    }

    char *body = strstr((char *)payload, "\r\n\r\n");
    if (body == NULL)
    {
        softap_send_json(link_id, 400, "{\"ok\":false,\"reason\":\"bad_request\"}");
        return;
    }
    body += 4;

    char ssid[WIFI_SSID_MAX_LEN + 1U] = {0};
    char pass[WIFI_PASS_MAX_LEN + 1U] = {0};
    bool parsed = false;

    /* 先尝试 JSON，兼容 fetch；再尝试 form，兼容传统表单。 */
    if (extract_json_string_value(body, "ssid", ssid, sizeof(ssid)) &&
        extract_json_string_value(body, "password", pass, sizeof(pass)))
    {
        parsed = true;
    }
    
    else if (extract_form_value(body, "ssid", ssid, sizeof(ssid)) &&
             extract_form_value(body, "password", pass, sizeof(pass)))
    {
        parsed = true;
    }

    if (!parsed)
    {
        softap_send_json(link_id, 400, "{\"ok\":false,\"reason\":\"missing_field\"}");
        return;
    }

    strncpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid) - 1U);
    strncpy(g_wifi_pass, pass, sizeof(g_wifi_pass) - 1U);
    g_wifi_ssid[sizeof(g_wifi_ssid) - 1U] = '\0';
    g_wifi_pass[sizeof(g_wifi_pass) - 1U] = '\0';
    /* 这里只置位标志，不在 HTTP 处理函数里直接切网，避免和当前连接发送流程冲突。 */
    g_provision_ready = true;

    LOGI("[PROV] recv ssid=%s len(pass)=%u\r\n", g_wifi_ssid, (unsigned)strlen(g_wifi_pass));
    softap_send_json(link_id, 200, "{\"ok\":true,\"msg\":\"saved\"}");
}

/*
 * 启动 SoftAP 配网模式。
 *
 * 步骤：切 AP 模式 -> 配置 AP 参数 -> 开启多连接 -> 开启 80 端口 TCP Server。
 * 任何一步失败都回到 IDLE，由上层决定何时重试。
 */
static softap_step_result_t softap_start(void)
{
    if (g_softap_provision_running)
    {
        return SOFTAP_STEP_DONE;
    }

    if (g_softap_cfg_state == SOFTAP_CFG_IDLE)
    {
        softap_cfg_begin_command("AT+CWMODE=2\r\n",
                                 SOFTAP_CFG_START_WAIT_MODE,
                                 1000U);
        return SOFTAP_STEP_PENDING;
    }

    switch (g_softap_cfg_state)
    {
        case SOFTAP_CFG_START_WAIT_MODE:
            if (strstr(At_Rx_Buff, "OK") != NULL)
            {
                softap_cfg_begin_command("AT+CWSAP=\"Strip-Config\",\"12345678\",6,3\r\n",
                                         SOFTAP_CFG_START_WAIT_AP,
                                         2000U);
            }
            else if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                     ((int32_t)(HAL_GetTick() - g_softap_cfg_deadline) >= 0))
            {
                g_softap_cfg_state = SOFTAP_CFG_IDLE;
                return SOFTAP_STEP_FAIL;
            }
            return SOFTAP_STEP_PENDING;

        case SOFTAP_CFG_START_WAIT_AP:
            if (strstr(At_Rx_Buff, "OK") != NULL)
            {
                softap_cfg_begin_command("AT+CIPMUX=1\r\n",
                                         SOFTAP_CFG_START_WAIT_MUX,
                                         1000U);
            }
            else if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                     ((int32_t)(HAL_GetTick() - g_softap_cfg_deadline) >= 0))
            {
                g_softap_cfg_state = SOFTAP_CFG_IDLE;
                return SOFTAP_STEP_FAIL;
            }
            return SOFTAP_STEP_PENDING;

        case SOFTAP_CFG_START_WAIT_MUX:
            if (strstr(At_Rx_Buff, "OK") != NULL)
            {
                softap_cfg_begin_command("AT+CIPSERVER=1,80\r\n",
                                         SOFTAP_CFG_START_WAIT_SERVER,
                                         1500U);
            }
            else if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                     ((int32_t)(HAL_GetTick() - g_softap_cfg_deadline) >= 0))
            {
                g_softap_cfg_state = SOFTAP_CFG_IDLE;
                return SOFTAP_STEP_FAIL;
            }
            return SOFTAP_STEP_PENDING;

        case SOFTAP_CFG_START_WAIT_SERVER:
            if (strstr(At_Rx_Buff, "OK") != NULL)
            {
                g_softap_cfg_state = SOFTAP_CFG_IDLE;
                g_softap_provision_running = true;
                LOGI("[PROV] SoftAP started ssid=Strip-Config ip=192.168.4.1\r\n");
                return SOFTAP_STEP_DONE;
            }
            if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                ((int32_t)(HAL_GetTick() - g_softap_cfg_deadline) >= 0))
            {
                g_softap_cfg_state = SOFTAP_CFG_IDLE;
                return SOFTAP_STEP_FAIL;
            }
            return SOFTAP_STEP_PENDING;

        default:
            return SOFTAP_STEP_FAIL;
    }
}

/*
 * 关闭 SoftAP 并恢复 STA 模式。
 *
 * 这里会顺带清空 HTTP 发送状态和队列，避免旧网页连接残留到下一轮配网。
 */
static softap_step_result_t softap_stop(void)
{
    if (!g_softap_provision_running && (g_softap_cfg_state == SOFTAP_CFG_IDLE))
    {
        return SOFTAP_STEP_DONE;
    }

    if (g_softap_cfg_state == SOFTAP_CFG_IDLE)
    {
        softap_cfg_begin_command("AT+CIPSERVER=0,1\r\n",
                                 SOFTAP_CFG_STOP_WAIT_SERVER_OFF,
                                 1000U);
        return SOFTAP_STEP_PENDING;
    }

    switch (g_softap_cfg_state)
    {
        case SOFTAP_CFG_STOP_WAIT_SERVER_OFF:
            /* 关闭 server 时把 ERROR 也视为可继续，避免模块状态和软件状态卡死。 */
            if ((strstr(At_Rx_Buff, "OK") != NULL) ||
                (strstr(At_Rx_Buff, "ERROR") != NULL) ||
                ((int32_t)(HAL_GetTick() - g_softap_cfg_deadline) >= 0))
            {
                softap_cfg_begin_command("AT+CWMODE=1\r\n",
                                         SOFTAP_CFG_STOP_WAIT_STA_MODE,
                                         1000U);
            }
            return SOFTAP_STEP_PENDING;

        case SOFTAP_CFG_STOP_WAIT_STA_MODE:
            if ((strstr(At_Rx_Buff, "OK") != NULL) ||
                (strstr(At_Rx_Buff, "ERROR") != NULL) ||
                ((int32_t)(HAL_GetTick() - g_softap_cfg_deadline) >= 0))
            {
                g_softap_cfg_state = SOFTAP_CFG_IDLE;
                g_softap_provision_running = false;
                g_softap_http_tx_state = SOFTAP_HTTP_TX_IDLE;
                g_softap_http_tx_has_active = false;
                g_softap_http_tx_q_count = 0;
                g_softap_http_tx_q_head = 0;
                g_softap_http_tx_q_tail = 0;
                LOGI("[PROV] SoftAP stopped\r\n");
                return SOFTAP_STEP_DONE;
            }
            return SOFTAP_STEP_PENDING;

        default:
            return SOFTAP_STEP_PENDING;
    }
}

/* 取走一次性“新凭据已写入”标志，防止主循环重复触发切网。 */
static bool softap_take_provisioned_flag(void)
{
    if (!g_provision_ready)
    {
        return false;
    }
    g_provision_ready = false;
    return true;
}

/*
 * SoftAP 运行阶段处理。
 *
 * 返回 true 表示当前仍由 SoftAP 接管主循环，外层不应再继续执行普通重连逻辑。
 */
static bool net_handle_softap_stage(uint32_t now)
{
    if (softap_cfg_is_stopping())
    {
        if (softap_stop() == SOFTAP_STEP_DONE)
        {
            g_provision_ready = false;
            g_net_retry_idx = 0;
            g_net_next_retry_tick = now;
            LOGI("[PROV] credentials received, retry connect now\r\n");
            return false;
        }
        return true;
    }

    if (softap_cfg_is_starting())
    {
        (void)softap_start();
        return true;
    }

    if (!g_softap_provision_running)
    {
        return false;
    }

    
    softap_provision_task();
    
    softap_http_tx_task();

    

    
    if (g_provision_ready &&
        (g_softap_http_tx_has_active ||
         (g_softap_http_tx_q_count > 0U) ||
         (g_softap_http_tx_state != SOFTAP_HTTP_TX_IDLE)))
    {
        return true;
    }

    
    if (softap_take_provisioned_flag())
    {
        if (softap_stop() == SOFTAP_STEP_DONE)
        {
            g_provision_ready = false;
            g_net_retry_idx = 0;
            g_net_next_retry_tick = now;
            LOGI("[PROV] credentials received, retry connect now\r\n");
            return false;
        }
        return true;
    }

    
    return true;
}

/* 达到重试阈值后进入 SoftAP 配网模式。 */
static bool net_maybe_enter_softap_stage(void)
{
    
    if (net_connect_is_running())
    {
        return false;
    }

    if (softap_cfg_is_starting())
    {
        return true;
    }

    if ((g_net_retry_idx < 3U) || g_softap_provision_running || softap_cfg_is_stopping())
    {
        return false;
    }

    softap_step_result_t step = softap_start();
    if (step == SOFTAP_STEP_FAIL)
    {
        return false;
    }
    if (step == SOFTAP_STEP_DONE)
    {
        LOGW("[NET] enter SoftAP provisioning mode\r\n");
    }
    return true;
}

/*
 * 普通重连阶段。
 *
 * 只有未进入 SoftAP、且还没到下一次退避时间时，才会重新启动联网状态机。
 */
static void net_try_reconnect_stage(uint32_t now)
{
    static const uint32_t k_retry_backoff_ms[] = {1000U, 2000U, 5000U, 10000U, 30000U};

    
    if (net_connect_is_running())
    {
        net_conn_step_result_t step = net_connect_task();
        if (step == NET_CONN_STEP_DONE)
        {
            g_net_ready = true;
            g_net_retry_idx = 0;
            g_net_next_retry_tick = now;
            LOGI("[NET] reconnect success\r\n");
        }
        else if (step == NET_CONN_STEP_FAIL)
        {
            uint8_t max_idx = (uint8_t)(sizeof(k_retry_backoff_ms) / sizeof(k_retry_backoff_ms[0]) - 1U);
            uint8_t use_idx = (g_net_retry_idx <= max_idx) ? g_net_retry_idx : max_idx;
            uint32_t delay_ms = k_retry_backoff_ms[use_idx];
            if (g_net_retry_idx < max_idx)
            {
                g_net_retry_idx++;
            }
            g_net_next_retry_tick = now + delay_ms;
            LOGW("[NET] reconnect failed, next in %lu ms\r\n", (unsigned long)delay_ms);
        }
        return;
    }

    
    if ((int32_t)(now - g_net_next_retry_tick) < 0)
    {
        return;
    }

    
    LOGI("[NET] reconnect attempt idx=%u\r\n", (unsigned)g_net_retry_idx);
    net_connect_start();

    
    return;
}

/*
 * 解析 SoftAP 收到的 HTTP 数据。
 *
 * 这里按 `+IPD,<link>,<len>:` 头解析链路号和正文，再把正文交给 HTTP 业务层。
 */
static void softap_provision_task(void)
{
    static char ipd_hdr[64];
    static uint16_t ipd_hdr_len = 0;
    static char ipd_payload[768];
    static uint16_t ipd_payload_len = 0;
    static uint32_t ipd_payload_need = 0;
    static int ipd_link_id = -1;
    static bool ipd_payload_mode = false;

    
    if (!g_softap_provision_running)
    {
        return;
    }

    uint8_t ch;
    while (g_http_rx_buf.get(&g_http_rx_buf, &ch) == 0)
    {
        if (!ipd_payload_mode)
        {
            
            if (ipd_hdr_len < (sizeof(ipd_hdr) - 1U))
            {
                ipd_hdr[ipd_hdr_len++] = (char)ch;
                ipd_hdr[ipd_hdr_len] = '\0';
            }
            else
            {
                
                memmove(ipd_hdr, ipd_hdr + 1, sizeof(ipd_hdr) - 2U);
                ipd_hdr[sizeof(ipd_hdr) - 2U] = (char)ch;
                ipd_hdr[sizeof(ipd_hdr) - 1U] = '\0';
                ipd_hdr_len = (uint16_t)(sizeof(ipd_hdr) - 1U);
            }

            if (ch == ':')
            {
                char *p = strstr(ipd_hdr, "+IPD,");
                unsigned int len = 0;
                int link = -1;
                if ((p != NULL) && (sscanf(p, "+IPD,%d,%u:", &link, &len) == 2) && (len > 0U))
                {
                    
                    ipd_payload_mode = true;
                    ipd_payload_need = len;
                    ipd_payload_len = 0;
                    ipd_link_id = link;
                    ipd_hdr_len = 0;
                    ipd_hdr[0] = '\0';
                }
            }
            continue;
        }

        
        if (ipd_payload_len < (sizeof(ipd_payload) - 1U))
        {
            ipd_payload[ipd_payload_len++] = (char)ch;
        }

        if (ipd_payload_need > 0U)
        {
            ipd_payload_need--;
        }

        if (ipd_payload_need == 0U)
        {
            
            ipd_payload[ipd_payload_len] = '\0';
            softap_process_http_payload(ipd_link_id, ipd_payload);

            
            ipd_payload_mode = false;
            ipd_payload_len = 0;
            ipd_link_id = -1;
        }
    }
}

/*
 * SoftAP HTTP 发送任务。
 *
 * ESP8266 的发送链路是串行的：
 * 1. 发 `AT+CIPSEND`
 * 2. 等待 `>`
 * 3. 发送 HTTP 报文
 * 4. 等待 `SEND OK`
 * 5. 发 `AT+CIPCLOSE`
 *
 * 该状态机让主循环可以持续跑其他任务，而不是卡在一次网页响应上。
 */
static void softap_http_tx_task(void)
{
    uint32_t now = HAL_GetTick();

    /* 空闲时从队列取一条待发响应，开始新一轮发送。 */
    if ((g_softap_http_tx_state == SOFTAP_HTTP_TX_IDLE) && !g_softap_http_tx_has_active)
    {
        if (!softap_http_tx_queue_pop(&g_softap_http_tx_active))
        {
            return;
        }

        g_softap_http_tx_has_active = true;
        snprintf(g_softap_http_header_buf,
                 sizeof(g_softap_http_header_buf),
                 "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nConnection: close\r\nContent-Length: %u\r\n\r\n%s",
                 g_softap_http_tx_active.status_code,
                 g_softap_http_tx_active.content_type,
                 (unsigned)strlen(g_softap_http_tx_active.body),
                 g_softap_http_tx_active.body);
        snprintf(g_softap_http_cmd_buf,
                 sizeof(g_softap_http_cmd_buf),
                 "AT+CIPSEND=%d,%u\r\n",
                 g_softap_http_tx_active.link_id,
                 (unsigned)strlen(g_softap_http_header_buf));

        
        g_softap_http_tx_state = SOFTAP_HTTP_TX_WAIT_PROMPT;
        g_softap_http_tx_deadline = now + SOFTAP_HTTP_PROMPT_TIMEOUT_MS;
        return;
    }

    switch (g_softap_http_tx_state)
    {
        case SOFTAP_HTTP_TX_WAIT_PROMPT:
            /* 只有收到 `>` 才能发送正文，否则直接发会被模组丢弃。 */
            if (strchr(At_Rx_Buff, '>') != NULL)
            {
                Uart2_Send_Flag = false;
                if (R_SCI_UART_Write(&g_uart2_esp8266_ctrl,
                                     (uint8_t *)g_softap_http_header_buf,
                                     strlen(g_softap_http_header_buf)) != FSP_SUCCESS)
                {
                    LOGW("[PROV] http body send fail link=%d\r\n",
                         g_softap_http_tx_active.link_id);
                    g_softap_http_tx_state = SOFTAP_HTTP_TX_IDLE;
                    g_softap_http_tx_has_active = false;
                    Clear_Buff();
                    return;
                }

                g_softap_http_tx_state = SOFTAP_HTTP_TX_WAIT_BODY_SENT;
                g_softap_http_tx_deadline = now + SOFTAP_HTTP_PROMPT_TIMEOUT_MS;
            }
            else if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                     ((int32_t)(now - g_softap_http_tx_deadline) >= 0))
            {
                
                LOGW("[PROV] http prompt timeout link=%d\r\n",
                     g_softap_http_tx_active.link_id);
                g_softap_http_tx_state = SOFTAP_HTTP_TX_IDLE;
                g_softap_http_tx_has_active = false;
                Clear_Buff();
            }
            break;

        case SOFTAP_HTTP_TX_WAIT_BODY_SENT:
            
            if (Uart2_Send_Flag)
            {
                g_softap_http_tx_state = SOFTAP_HTTP_TX_WAIT_SEND_OK;
                g_softap_http_tx_deadline = now + SOFTAP_HTTP_SEND_OK_TIMEOUT_MS;
            }
            else if ((int32_t)(now - g_softap_http_tx_deadline) >= 0)
            {
                
                LOGW("[PROV] http tx complete timeout link=%d\r\n",
                     g_softap_http_tx_active.link_id);
                g_softap_http_tx_state = SOFTAP_HTTP_TX_IDLE;
                g_softap_http_tx_has_active = false;
                Clear_Buff();
            }
            break;

        case SOFTAP_HTTP_TX_WAIT_SEND_OK:
            
            if (strstr(At_Rx_Buff, "SEND OK") != NULL)
            {
                Clear_Buff();
                snprintf(g_softap_http_cmd_buf,
                         sizeof(g_softap_http_cmd_buf),
                         "AT+CIPCLOSE=%d\r\n",
                         g_softap_http_tx_active.link_id);
                ESP8266_AT_Send(g_softap_http_cmd_buf);
                g_softap_http_tx_state = SOFTAP_HTTP_TX_WAIT_CLOSE_OK;
                g_softap_http_tx_deadline = now + SOFTAP_HTTP_CLOSE_TIMEOUT_MS;
            }
            else if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                     ((int32_t)(now - g_softap_http_tx_deadline) >= 0))
            {
                
                LOGW("[PROV] http send ok timeout link=%d\r\n",
                     g_softap_http_tx_active.link_id);
                g_softap_http_tx_state = SOFTAP_HTTP_TX_IDLE;
                g_softap_http_tx_has_active = false;
                Clear_Buff();
            }
            break;

        case SOFTAP_HTTP_TX_WAIT_CLOSE_OK:
            
            if (strstr(At_Rx_Buff, "OK") != NULL)
            {
                g_softap_http_tx_state = SOFTAP_HTTP_TX_IDLE;
                g_softap_http_tx_has_active = false;
                Clear_Buff();
            }
            else if ((strstr(At_Rx_Buff, "ERROR") != NULL) ||
                     ((int32_t)(now - g_softap_http_tx_deadline) >= 0))
            {
                
                LOGW("[PROV] http close timeout link=%d\r\n",
                     g_softap_http_tx_active.link_id);
                g_softap_http_tx_state = SOFTAP_HTTP_TX_IDLE;
                g_softap_http_tx_has_active = false;
                Clear_Buff();
            }
            break;

        case SOFTAP_HTTP_TX_IDLE:
        default:
            break;
    }
}

/*
 * 模块初始化。
 *
 * 上电默认直接尝试当前凭据联网，不主动进入 SoftAP，只有失败累计到阈值才降级。
 */
void NET_Manager_Init(void)
{
    
    g_net_ready = false;
    g_net_retry_idx = 0;
    g_net_next_retry_tick = HAL_GetTick() + 1000U;
    g_softap_provision_running = false;
    net_connect_start();
}

/*
 * 网络管理主任务。
 *
 * 调用频率：主循环高频调用。
 * 执行阶段：
 * 1. 已联网则直接返回。
 * 2. 如果当前处于 SoftAP 阶段，则优先跑配网收发与收尾。
 * 3. 达到失败阈值后尝试切入 SoftAP。
 * 4. 否则按退避时间执行普通 WiFi/MQTT 重连。
 */
void NET_Manager_Task(void)
{
    uint32_t now = HAL_GetTick();

    if (g_net_ready)
    {
        return;
    }

    if (net_handle_softap_stage(now))
    {
        return;
    }

    if (net_maybe_enter_softap_stage())
    {
        return;
    }

    
    net_try_reconnect_stage(now);
}

bool NET_Manager_IsReady(void)
{
    return g_net_ready;
}

void NET_Manager_GetWiFiCredentials(const char **ssid, const char **password)
{
    if (ssid != NULL)
    {
        *ssid = g_wifi_ssid;
    }
    if (password != NULL)
    {
        *password = g_wifi_pass;
    }
}

bool NET_Manager_IsProvisioningRunning(void)
{
    return g_softap_provision_running;
}
