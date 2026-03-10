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

/* 运行时 WiFi 配置：默认用编译期宏，配网成功后被覆盖 */
static char g_wifi_ssid[WIFI_SSID_MAX_LEN + 1U] = ID;
static char g_wifi_pass[WIFI_PASS_MAX_LEN + 1U] = PASSWORD;

/* 网络连通状态 */
static bool g_net_ready = false;
/* 重连退避索引 */
static uint8_t g_net_retry_idx = 0;
/* 下次允许重连的时刻 */
static uint32_t g_net_next_retry_tick = 0;
/* 当前是否处于 SoftAP 配网模式 */
static bool g_softap_provision_running = false;
/* 是否收到新配网凭据 */
static volatile bool g_provision_ready = false;

/* ----------------------- 配网内部工具函数 ----------------------- */

static bool softap_wait_response(const char *ok, const char *err, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - start) < timeout_ms)
    {
        if ((ok != NULL) && (strstr(At_Rx_Buff, ok) != NULL))
        {
            return true;
        }
        if ((err != NULL) && (strstr(At_Rx_Buff, err) != NULL))
        {
            return false;
        }
        R_BSP_SoftwareDelay(10, BSP_DELAY_UNITS_MILLISECONDS);
    }
    return false;
}

/* ----------------------- 联网初始化函数（从 BSP 迁移） ----------------------- */

/* ESP8266 连接 WiFi（阻塞式初始化路径） */
bool ESP8266_STA_JoinAP(char *id, char *password, uint8_t timeout)
{
    char join_ap_at[256];

    sprintf(join_ap_at, "AT+CWJAP=\"%s\",\"%s\"\r\n", id, password);

    Clear_Buff();
    ESP8266_AT_Send(join_ap_at);

    /* timeout 参数沿用“秒”语义 */
    if (softap_wait_response("OK\r\n", "ERROR\r\n", ((uint32_t)timeout) * 1000U))
    {
        ESP8266_DEBUG_MSG("\r\nWifi连接成功\r\n");
        Clear_Buff();
        return true;
    }

    if (strstr(At_Rx_Buff, "+CWJAP:1\r\n"))
    {
        ESP8266_DEBUG_MSG("\r\nWifi连接超时，请检查各项配置是否正确\r\n");
    }
    else if (strstr(At_Rx_Buff, "+CWJAP:2\r\n"))
    {
        ESP8266_DEBUG_MSG("\r\nWifi密码错误，请检查Wifi密码是否正确\r\n");
    }
    else if (strstr(At_Rx_Buff, "+CWJAP:3\r\n"))
    {
        ESP8266_DEBUG_MSG("\r\n无法找到目标Wifi，请检查Wifi是否打开或Wifi名称是否正确\r\n");
    }
    else if (strstr(At_Rx_Buff, "+CWJAP:4\r\n"))
    {
        ESP8266_DEBUG_MSG("\r\nWifi连接失败，请检查各项配置是否正确\r\n");
    }
    else
    {
        ESP8266_DEBUG_MSG("\r\nWifi连接超出期望时间，请检查各项配置是否正确\r\n");
    }

    return false;
}

/* 设置 MQTT 用户属性 */
void MQTT_SetUserProperty(char *client_id, char *user_name, char *user_password)
{
    char set_user_property_at[256];

    sprintf(set_user_property_at, "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"\r\n", client_id, user_name, user_password);
    ESP8266_AT_Send(set_user_property_at);

    while (!Uart2_Send_Flag)
    {
        if (strstr(At_Rx_Buff, "OK\r\n"))
        {
            ESP8266_DEBUG_MSG("\r\nMQTT用户属性已设置完成\r\n");
            Clear_Buff();
        }
    }
}

/* 连接 MQTT Broker（阻塞式初始化路径） */
bool Connect_MQTT(char *mqtt_ip, char *mqtt_port, uint8_t timeout)
{
    char connect_mqtt_at[256];

    sprintf(connect_mqtt_at, "AT+MQTTCONN=0,\"%s\",%s,1\r\n", mqtt_ip, mqtt_port);

    Clear_Buff();
    ESP8266_AT_Send(connect_mqtt_at);

    if (softap_wait_response("OK\r\n", "ERROR\r\n", ((uint32_t)timeout) * 1000U))
    {
        ESP8266_DEBUG_MSG("\r\nMQTT服务器连接成功\r\n");
        Clear_Buff();
        return true;
    }

    if (strstr(At_Rx_Buff, "ERROR\r\n") != NULL)
    {
        ESP8266_DEBUG_MSG("\r\nMQTT服务器连接失败，请检查各项配置是否正确\r\n");
    }
    else
    {
        ESP8266_DEBUG_MSG("\r\nMQTT服务器连接超出期望时间，请检查各项配置是否正确\r\n");
    }

    return false;
}

/* 订阅主题函数 */
bool Subscribes_Topics(char *topics)
{
    char sub_topics_at[256];

    sprintf(sub_topics_at, "AT+MQTTSUB=0,\"%s\",1\r\n", topics);

    Clear_Buff();
    ESP8266_AT_Send(sub_topics_at);

    if (softap_wait_response("OK", "ERROR\r\n", 3000U))
    {
        ESP8266_DEBUG_MSG("\r\n主题订阅成功\r\n");
        Clear_Buff();
        return true;
    }

    if (strstr(At_Rx_Buff, "ALREADY\r\n") != NULL)
    {
        ESP8266_DEBUG_MSG("\r\n已经订阅过该主题\r\n");
        Clear_Buff();
        return true;
    }

    ESP8266_DEBUG_MSG("\r\n主题订阅失败或超时\r\n");
    return false;
}

/**
 * @brief 关闭 ESP8266 命令回显 (ATE0)
 * @return 0: 成功, -1: 超时
 */
int ESP8266_ATE0(void)
{
    ESP8266_DEBUG_MSG("[SYSTEM] 正在关闭 ATE 回显...\r\n");
    Clear_Buff();
    ESP8266_AT_Send("ATE0\r\n");

    uint32_t timeout = 100; /* 500ms */
    while (timeout--)
    {
        if (strstr(At_Rx_Buff, "OK"))
        {
            ESP8266_DEBUG_MSG("[SYSTEM] 回显已成功关闭！\r\n");
            Clear_Buff();
            return 0;
        }
        R_BSP_SoftwareDelay(5, BSP_DELAY_UNITS_MILLISECONDS);
    }
    ESP8266_DEBUG_MSG("[ERROR] ATE0 无响应\r\n");
    return -1;
}

bool ESP8266_MQTT_Test(void)
{
    ESP8266_DEBUG_MSG("\r\n[SYSTEM] 开始初始化智能排插...\r\n");
    ESP8266_UART2_Init();
    ESP8266_Hard_Reset();
    R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_SECONDS); /* 复位后多等 1s */
    ESP8266_STA();
    R_BSP_SoftwareDelay(500, BSP_DELAY_UNITS_MILLISECONDS); /* 给 ATE0 处理时间 */
    if (ESP8266_ATE0() != 0)
    {
        ESP8266_DEBUG_MSG("[NET] 初始化失败：ATE0 无响应\r\n");
        return false;
    }
    R_BSP_SoftwareDelay(500, BSP_DELAY_UNITS_MILLISECONDS);

    /* 降低发射功率，减少瞬时电流峰值 */
    ESP8266_AT_Send("AT+RFPOWER=50\r\n");
    R_BSP_SoftwareDelay(200, BSP_DELAY_UNITS_MILLISECONDS);

    const char *cfg_ssid = NULL;
    const char *cfg_pass = NULL;
    NET_Manager_GetWiFiCredentials(&cfg_ssid, &cfg_pass);
    if (!ESP8266_STA_JoinAP((char *)cfg_ssid, (char *)cfg_pass, 20))
    {
        ESP8266_DEBUG_MSG("[NET] 初始化失败：WiFi 连接失败\r\n");
        return false;
    }

    ESP8266_DEBUG_MSG("配置 MQTT 用户属性\r\n");
    MQTT_SetUserProperty(CLIENT_ID, USER_NAME, USER_PASSWORD);

    ESP8266_DEBUG_MSG("连接 MQTT 服务器..\r\n");
    if (!Connect_MQTT(MQTT_IP, MQTT_Port, 100))
    {
        ESP8266_DEBUG_MSG("[NET] 初始化失败：MQTT 连接失败\r\n");
        return false;
    }

    ESP8266_DEBUG_MSG("订阅命令主题: %s\r\n", MQTT_SUB_TOPIC);
    if (!Subscribes_Topics(MQTT_SUB_TOPIC))
    {
        ESP8266_DEBUG_MSG("[NET] 初始化失败：订阅主题失败\r\n");
        return false;
    }

    ESP8266_DEBUG_MSG("初始化完成，进入监听模式...\r\n");
    memset(At_Rx_Buff, 0, sizeof(At_Rx_Buff));
    uint8_t dummy;
    while (g_cmd_rx_buf.get(&g_cmd_rx_buf, &dummy) == 0) {}
    while (g_http_rx_buf.get(&g_http_rx_buf, &dummy) == 0) {}
    return true;
}

static bool extract_json_string_value(const char *json, const char *key, char *out, size_t out_sz)
{
    if ((json == NULL) || (key == NULL) || (out == NULL) || (out_sz == 0U))
    {
        return false;
    }

    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    char *k = strstr((char *)json, pat);
    if (k == NULL) return false;
    char *colon = strchr(k, ':');
    if (colon == NULL) return false;
    char *q1 = strchr(colon, '"');
    if (q1 == NULL) return false;
    q1++;
    char *q2 = strchr(q1, '"');
    if ((q2 == NULL) || (q2 <= q1)) return false;

    size_t len = (size_t)(q2 - q1);
    if (len >= out_sz) len = out_sz - 1U;
    memcpy(out, q1, len);
    out[len] = '\0';
    return (len > 0U);
}

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

static void softap_send_response_ex(int link_id,
                                    int status_code,
                                    const char *content_type,
                                    const char *body_text)
{
    char body[1400];
    char header[1600];
    char cmd[64];

    snprintf(body, sizeof(body), "%s", (body_text != NULL) ? body_text : "");
    snprintf(header,
             sizeof(header),
             "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nConnection: close\r\nContent-Length: %u\r\n\r\n%s",
             status_code,
             (content_type != NULL) ? content_type : "text/plain",
             (unsigned)strlen(body),
             body);

    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%u\r\n", link_id, (unsigned)strlen(header));
    Clear_Buff();
    ESP8266_AT_Send(cmd);
    if (!softap_wait_response(">", "ERROR", 1000U))
    {
        return;
    }

    (void)R_SCI_UART_Write(&g_uart2_esp8266_ctrl, (uint8_t *)header, strlen(header));
    (void)softap_wait_response("SEND OK", "ERROR", 1000U);

    snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%d\r\n", link_id);
    Clear_Buff();
    ESP8266_AT_Send(cmd);
    (void)softap_wait_response("OK", "ERROR", 500U);
}

static void softap_send_json(int link_id, int status_code, const char *json_body)
{
    softap_send_response_ex(link_id, status_code, "application/json", json_body);
}

static void softap_process_http_payload(int link_id, const char *payload)
{
    static const char k_prov_html[] =
        "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Strip WiFi</title><style>body{font-family:sans-serif;padding:20px}input,button{width:100%;padding:10px;margin:8px 0}#msg{color:#333}</style>"
        "</head><body><h3>配网设置</h3><input id='ssid' placeholder='WiFi名称'><input id='pwd' type='password' placeholder='WiFi密码'>"
        "<button onclick='go()'>保存并连接</button><p id='msg'></p><script>"
        "async function go(){const ssid=document.getElementById('ssid').value;const password=document.getElementById('pwd').value;"
        "const r=await fetch('/provision',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,password})});"
        "const t=await r.text();document.getElementById('msg').innerText=t;}</script></body></html>";

    if ((payload == NULL) || (link_id < 0))
    {
        return;
    }

    if ((strstr(payload, "GET / ") != NULL) || (strstr(payload, "GET /index.html") != NULL))
    {
        softap_send_response_ex(link_id, 200, "text/html; charset=utf-8", k_prov_html);
        return;
    }

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
    g_provision_ready = true;

    LOGI("[PROV] recv ssid=%s len(pass)=%u\r\n", g_wifi_ssid, (unsigned)strlen(g_wifi_pass));
    softap_send_json(link_id, 200, "{\"ok\":true,\"msg\":\"saved\"}");
}

static bool softap_start(void)
{
    if (g_softap_provision_running)
    {
        return true;
    }

    Clear_Buff();
    ESP8266_AT_Send("AT+CWMODE=2\r\n");
    if (!softap_wait_response("OK", "ERROR", 1000U))
    {
        return false;
    }

    Clear_Buff();
    ESP8266_AT_Send("AT+CWSAP=\"Strip-Config\",\"12345678\",6,3\r\n");
    if (!softap_wait_response("OK", "ERROR", 2000U))
    {
        return false;
    }

    Clear_Buff();
    ESP8266_AT_Send("AT+CIPMUX=1\r\n");
    if (!softap_wait_response("OK", "ERROR", 1000U))
    {
        return false;
    }

    Clear_Buff();
    ESP8266_AT_Send("AT+CIPSERVER=1,80\r\n");
    if (!softap_wait_response("OK", "ERROR", 1500U))
    {
        return false;
    }

    g_softap_provision_running = true;
    LOGI("[PROV] SoftAP started ssid=Strip-Config ip=192.168.4.1\r\n");
    return true;
}

static bool softap_stop(void)
{
    if (!g_softap_provision_running)
    {
        return true;
    }

    Clear_Buff();
    ESP8266_AT_Send("AT+CIPSERVER=0,1\r\n");
    (void)softap_wait_response("OK", "ERROR", 1000U);

    Clear_Buff();
    ESP8266_AT_Send("AT+CWMODE=1\r\n");
    (void)softap_wait_response("OK", "ERROR", 1000U);

    g_softap_provision_running = false;
    LOGI("[PROV] SoftAP stopped\r\n");
    return true;
}

static bool softap_take_provisioned_flag(void)
{
    if (!g_provision_ready)
    {
        return false;
    }
    g_provision_ready = false;
    return true;
}

static void softap_provision_task(void)
{
    static char ipd_hdr[96];
    static uint16_t ipd_hdr_len = 0;
    static char ipd_payload[1024];
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

/* ----------------------- 对外接口 ----------------------- */

void NET_Manager_Init(void)
{
    g_net_ready = ESP8266_MQTT_Test();
    if (!g_net_ready)
    {
        g_net_retry_idx = 0;
        g_net_next_retry_tick = HAL_GetTick() + 1000U;
        g_softap_provision_running = false;
        LOGW("[NET] init failed, enter degraded mode\r\n");
    }
}

void NET_Manager_Task(void)
{
    static const uint32_t k_retry_backoff_ms[] = {1000U, 2000U, 5000U, 10000U, 30000U};
    uint32_t now = HAL_GetTick();

    softap_provision_task();

    if (g_net_ready)
    {
        return;
    }

    if (softap_take_provisioned_flag())
    {
        (void)softap_stop();
        g_softap_provision_running = false;
        g_net_retry_idx = 0;
        g_net_next_retry_tick = now;
        LOGI("[PROV] credentials received, retry connect now\r\n");
    }

    if ((g_net_retry_idx >= 3U) && !g_softap_provision_running)
    {
        if (softap_start())
        {
            g_softap_provision_running = true;
            LOGW("[NET] enter SoftAP provisioning mode\r\n");
            return;
        }
    }

    if (g_softap_provision_running)
    {
        return;
    }

    if ((int32_t)(now - g_net_next_retry_tick) < 0)
    {
        return;
    }

    LOGI("[NET] reconnect attempt idx=%u\r\n", (unsigned)g_net_retry_idx);
    g_net_ready = ESP8266_MQTT_Test();
    if (g_net_ready)
    {
        if (g_softap_provision_running)
        {
            (void)softap_stop();
            g_softap_provision_running = false;
        }
        LOGI("[NET] reconnect success\r\n");
        g_net_retry_idx = 0;
        g_net_next_retry_tick = now;
        return;
    }

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
