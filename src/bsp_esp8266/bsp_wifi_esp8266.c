#include "bsp_wifi_esp8266.h"
#include "bsp_debug_uart.h"
#include "circle_buf.h"
#include "SoftAP_connect_wifi/SoftAP_connect_wifi.h"
#include "cJSON_handle.h"
#include "Systick.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern circle_buf_t g_rx_buf;
extern circle_buf_t g_cmd_rx_buf;
extern circle_buf_t g_http_rx_buf;

_Bool               Uart2_Send_Flag = false; // 用来判断 UART2 接收以及发送数据是否完成


/* 用来接收 UART2 数据的线性缓冲区 */
char                At_Rx_Buff[256];
uint8_t             Uart2_Num = 0;

/* UART2 命令环形缓冲溢出统计（中断写满时递增） */
static volatile uint32_t g_uart2_rb_overflow_cnt = 0;
/* 上次打印的溢出计数与时间戳（用于日志节流） */
static uint32_t g_uart2_rb_overflow_last_print = 0;
static uint32_t g_uart2_rb_overflow_last_tick = 0;

/*
 * UART2 行缓冲区。
 *
 * 作用：
 * - ESP8266 返回的数据是按字节进入中断回调的，而上层很多逻辑希望按“完整一行”处理
 * - 这里先把收到的字符暂存在 g_cmd_line_buf 中，直到遇到 '\n' 才认为一行结束
 * - 行结束后，只把包含 "+MQTTSUBRECV" 的 MQTT 下行消息转发到 g_cmd_rx_buf
 *
 * 这样做的目的：
 * - 避免把普通 AT 回显、OK/ERROR、杂项日志全部塞进命令缓冲区
 * - 让后续 MQTT 命令解析只面对“真正关心的订阅消息”
 *
 * g_cmd_line_len:
 * - 当前这一行已经累计了多少字节
 *
 * g_cmd_line_overflow:
 * - 标记当前这一行是否已经超长
 * - 一旦超长，本行剩余字符都会被丢弃，直到读到 '\n' 才恢复
 * - 这样可以避免单条异常长数据把缓存写坏
 */
#define CMD_LINE_BUF_SIZE 768U
static char g_cmd_line_buf[CMD_LINE_BUF_SIZE];
static uint16_t g_cmd_line_len = 0;
static bool g_cmd_line_overflow = false;

/*
 * 通用逐行组帧缓冲区。
 *
 * 这组变量用于把 UART 收到的零散字符临时拼成一行文本。
 * 它和上面的 g_cmd_line_buf 类似，都是“按字节接收、按行处理”的思路；
 * 区别在于这里是更轻量的通用缓冲，适合简单串口文本拼接场景。
 *
 * line_buffer:
 * - 保存当前正在拼接的一行内容
 *
 * line_idx:
 * - 指向当前可写位置，也表示这行已经写入了多少字符
 */
static char line_buffer[256]; 
static uint8_t line_idx = 0;

/*
 * 非阻塞 MQTT RAW 发送参数
 * - TX_TOPIC_MAX_LEN / TX_PAYLOAD_MAX_LEN: 单条任务的缓存上限
 * - TX_QUEUE_DEPTH: 最多缓存多少条待发任务
 * - TX_PROMPT_TIMEOUT_MS: 等待 '>' 超时
 * - TX_SEND_TIMEOUT_MS: 等待 UART 发送完成超时
 * - TX_OK_TIMEOUT_MS: 等待模块返回 OK 超时
 */
#define TX_TOPIC_MAX_LEN        80U
#define TX_PAYLOAD_MAX_LEN      640U
#define TX_QUEUE_DEPTH          4U
#define TX_PROMPT_TIMEOUT_MS    1000U
#define TX_SEND_TIMEOUT_MS      100U
#define TX_OK_TIMEOUT_MS        1000U

/* 单条待发送任务：topic + payload + payload长度 */
typedef struct
{
    uint16_t payload_len;
    char topic[TX_TOPIC_MAX_LEN];
    char payload[TX_PAYLOAD_MAX_LEN];
} mqtt_tx_item_t;

/* 发送状态机（一次 RAW 发布） */
typedef enum
{
    TX_STATE_IDLE = 0,
    TX_STATE_WAIT_PROMPT,
    TX_STATE_WAIT_PAYLOAD_SENT,
    TX_STATE_WAIT_OK,
} tx_state_t;

/* 环形队列：head 出队，tail 入队，count 表示当前队列长度 */
static mqtt_tx_item_t g_tx_queue[TX_QUEUE_DEPTH];
static uint8_t g_tx_q_head = 0;
static uint8_t g_tx_q_tail = 0;
static uint8_t g_tx_q_count = 0;

/* 当前正在发送的任务上下文 */
static mqtt_tx_item_t g_tx_active;
static bool g_tx_has_active = false;
static tx_state_t g_tx_state = TX_STATE_IDLE;
static uint32_t g_tx_deadline = 0;
static char g_tx_cmd_buf[160];
static bool g_uart2_opened = false;

/*
 * WiFi 子系统初始化。
 *
 * 当前阶段先保留“主循环轮询”的裸机模式：
 * - 网络相关状态机仍然由后续 WiFi_ServiceTask() 推进
 * - 这里负责拉起首次联网流程
 */
void WiFi_Init(void)
{
    NET_Manager_Init();
}

/*
 * 裸机版 WiFi 总管函数。
 *
 * 这里统一收口所有 ESP8266 相关逻辑：
 * 1. 网络连接/重连/SoftAP 配网状态机
 * 2. MQTT 发布发送状态机
 * 3. MQTT 下行命令流式解析
 *
 * 后续迁移到 FreeRTOS 时，只需要把本函数放进独立 wifi_task 循环中即可。
 */
void WiFi_ServiceTask(void)
{
    NET_Manager_Task();
    ESP8266_TxTask();
    handle_uart_json_stream();
}

/*
 * 业务层发布请求入口。
 *
 * 当前实现先复用既有发送队列，保持行为不变；
 * 后续如果改成“业务请求队列 -> WiFi 请求分发”，外部调用点无需再改。
 */
bool WiFi_RequestPublish(char * topics , char * data )
{
    return Send_Data_Raw(topics, data);
}




/* ESP8266 UART2 初始化 */
void ESP8266_UART2_Init(void)
{
    if (g_uart2_opened)
    {
        return;
    }

    fsp_err_t err = R_SCI_UART_Open(g_uart2_esp8266.p_ctrl, g_uart2_esp8266.p_cfg);
    if (err == FSP_SUCCESS)
    {
        g_uart2_opened = true;
    }
}


void ESP8266_AT_Send(char * cmd )
{
    /* 必须先设为 false，再启动传输 */
    Uart2_Send_Flag = false;
    R_SCI_UART_Write(&g_uart2_esp8266_ctrl, (uint8_t *)cmd, strlen(cmd));
}


/*
 * 向 MQTT 发送队列尾部压入一条待发送消息。
 *
 * 这里实现的是一个固定长度的环形队列：
 * - g_tx_q_tail 指向“下一个可写入槽位”
 * - g_tx_q_count 表示当前队列中已有多少条待发送消息
 *
 * 该函数只负责把 topic/payload 拷贝到队列中，不直接和 ESP8266 交互。
 * 真正的串口发送动作由后续的 ESP8266_TxTask() 异步取出并推进。
 *
 * 返回值：
 * - true  : 入队成功，说明后续会有发送任务处理这条消息
 * - false : 入队失败，常见原因包括参数非法、长度超限、队列已满
 */
static bool tx_queue_push(const char *topic, const char *payload, uint16_t payload_len)
{
    /* 基本参数检查：topic/payload 不能为空，payload 长度也不能为 0。 */
    if ((topic == NULL) || (payload == NULL) || (payload_len == 0U))
    {
        return false;
    }

    /*
     * 长度保护：
     * - topic 需要为结尾的 '\0' 预留空间，所以必须严格小于缓冲区上限
     * - payload 同样需要为补 '\0' 预留 1 字节
     */
    if ((strlen(topic) >= TX_TOPIC_MAX_LEN) || (payload_len >= TX_PAYLOAD_MAX_LEN))
    {
        return false;
    }

    /* 队列已满时拒绝继续写入，避免覆盖尚未发送的数据。 */
    if (g_tx_q_count >= TX_QUEUE_DEPTH)
    {
        return false;
    }

    /* 取出当前尾指针对应槽位，准备写入新的发送任务。 */
    mqtt_tx_item_t *slot = &g_tx_queue[g_tx_q_tail];

    /* 先清空槽位，避免残留旧数据影响本次发送。 */
    memset(slot, 0, sizeof(*slot));

    /* 拷贝 topic 和 payload，并手动补齐字符串结束符。 */
    strncpy(slot->topic, topic, TX_TOPIC_MAX_LEN - 1U);
    memcpy(slot->payload, payload, payload_len);
    slot->payload[payload_len] = '\0';
    slot->payload_len = payload_len;

    /*
     * 尾指针向后移动一格；到数组末尾后通过取模回到 0，
     * 这就是环形队列的核心。
     */
    g_tx_q_tail = (uint8_t)((g_tx_q_tail + 1U) % TX_QUEUE_DEPTH);
    g_tx_q_count++;
    return true;
}

/*
 * 从 MQTT 发送队列头部取出一条待发送消息。
 *
 * 这是环形队列的“出队”操作：
 * - g_tx_q_head 指向当前最早入队、尚未发送的元素
 * - 成功取出后，head 前移，count 减 1
 *
 * 注意：
 * - 该函数会把队头元素完整复制到调用者提供的 out 中
 * - 队列为空或 out 为空时直接返回 false
 */
static bool tx_queue_pop(mqtt_tx_item_t *out)
{
    /* 输出指针非法，或者当前没有待发送消息。 */
    if ((out == NULL) || (g_tx_q_count == 0U))
    {
        return false;
    }

    /* 取出当前队头元素，供发送状态机后续使用。 */
    *out = g_tx_queue[g_tx_q_head];

    /* 队头前移一格，保持先进先出。 */
    g_tx_q_head = (uint8_t)((g_tx_q_head + 1U) % TX_QUEUE_DEPTH);
    g_tx_q_count--;
    return true;
}

/*
 * Send_Data_Raw 语义说明（与旧版不同）：
 * - 旧版：函数内部阻塞等待直到发送成功/失败
 * - 现在：只负责“入队”，立即返回
 * 返回 true 仅表示“入队成功”，实际发送由 ESP8266_TxTask 推进。
 */
bool Send_Data_Raw( char * topics , char * data )
{
    //订阅主题为空或者数据为空直接返回
    if ((topics == NULL) || (data == NULL))
    {
        return false;
    }
    /*发送数据字节个数为0直接返回*/
    uint16_t data_len = (uint16_t) strlen(data);
    if (data_len == 0U)
    {
        return false;
    }
     

    if (!tx_queue_push(topics, data, data_len))
    {
        LOGW("[TX] queue full/drop topic=%s len=%u\r\n", topics, (unsigned) data_len);
        return false;
    }

    return true;
}

/**
 * @brief 异步发送状态机：每 1ms ~ 10ms 调用一次即可
 * @note 这个函数绝不使用 while() 等待，只用 if()/switch() 检查条件并推进状态。
 */
void ESP8266_TxTask(void)
{
    uint32_t now = HAL_GetTick(); // 获取当前系统时间（毫秒）
    /* 每 1s 最多打印一次溢出统计，避免日志反向拖慢主循环 */
    if ((uint32_t)(now - g_uart2_rb_overflow_last_tick) >= 1000U)
    {
        if (g_uart2_rb_overflow_cnt != g_uart2_rb_overflow_last_print)
        {
            LOGW("[RB] overflow cmd_rx count=%lu (+%lu)\r\n",
                 (unsigned long)g_uart2_rb_overflow_cnt,
                 (unsigned long)(g_uart2_rb_overflow_cnt - g_uart2_rb_overflow_last_print));
            g_uart2_rb_overflow_last_print = g_uart2_rb_overflow_cnt;
        }
        g_uart2_rb_overflow_last_tick = now;
    }

    /* 阶段 0：寻找新任务 */
    /*当发送状态满足空闲且现在没有处于发送状态时调用*/
    if ((g_tx_state == TX_STATE_IDLE) && !g_tx_has_active)
    {
        // 队列为空则直接返回，不占用主循环时间
        if (!tx_queue_pop(&g_tx_active))
        {
            return;
        }
        g_tx_has_active = true;

        // 组装 RAW 发布指令，告诉模块：topic 是谁、payload 长度是多少
        snprintf(g_tx_cmd_buf,
                 sizeof(g_tx_cmd_buf),
                 "AT+MQTTPUBRAW=0,\"%s\",%u,1,0\r\n",
                 g_tx_active.topic,
                 (unsigned) g_tx_active.payload_len);

        // 清空回显缓冲，避免旧数据影响状态判断
        Clear_Buff();
        // 发出 AT 命令，下一阶段等待模块返回 '>'
        ESP8266_AT_Send(g_tx_cmd_buf);
        g_tx_state = TX_STATE_WAIT_PROMPT;
        // 设置超时“死线”，防止一直卡在等待状态
        g_tx_deadline = now + TX_PROMPT_TIMEOUT_MS;
        return;
    }

    switch (g_tx_state)
    {
        /* 阶段 1：等待提示符 '>' */
        case TX_STATE_WAIT_PROMPT:
            // 收到 '>' 说明模块已准备好接收 payload
            if (strchr(At_Rx_Buff, '>') != NULL)
            {
                /*清楚发送标志位*/
                Uart2_Send_Flag = false;
                // 正式发送 payload（JSON）
                fsp_err_t werr = R_SCI_UART_Write(&g_uart2_esp8266_ctrl,
                                                  (uint8_t *) g_tx_active.payload,
                                                  g_tx_active.payload_len);
                if (werr != FSP_SUCCESS)
                {
                    // UART 写失败：当前任务失败，状态机复位
                    LOGE("[TX] RAW write fail err=%d topic=%s\r\n",
                         (int) werr,
                         g_tx_active.topic);
                    g_tx_state = TX_STATE_IDLE;
                    g_tx_has_active = false;
                    Clear_Buff();
                    return;
                }

                // payload 已交给 UART，下一阶段等待 TX_COMPLETE
                g_tx_state = TX_STATE_WAIT_PAYLOAD_SENT;
                g_tx_deadline = now + TX_SEND_TIMEOUT_MS;
            }
            else if ((int32_t) (now - g_tx_deadline) >= 0)
            {
                // 超时还没收到 '>'，判定失败并复位
                LOGW("[TX] RAW prompt timeout topic=%s\r\n", g_tx_active.topic);
                g_tx_state = TX_STATE_IDLE;
                g_tx_has_active = false;
                Clear_Buff();
            }
            break;

        /* 阶段 2：等待 UART 硬件把 payload 发完 */
        case TX_STATE_WAIT_PAYLOAD_SENT:
            // 串口发送完成回调会把 Uart2_Send_Flag 置位
            if (Uart2_Send_Flag)
            {
                // 已发完，进入等待模块最终 OK
                g_tx_state = TX_STATE_WAIT_OK;
                g_tx_deadline = now + TX_OK_TIMEOUT_MS;
            }
            else if ((int32_t) (now - g_tx_deadline) >= 0)
            {
                // TX_COMPLETE 超时，判定失败并复位
                LOGW("[TX] RAW tx complete timeout topic=%s\r\n", g_tx_active.topic);
                g_tx_state = TX_STATE_IDLE;
                g_tx_has_active = false;
                Clear_Buff();
            }
            break;

        /* 阶段 3：等待最终确认 OK */
        case TX_STATE_WAIT_OK:
            // 模块回 OK：一次发布流程完整成功
            if (strstr(At_Rx_Buff, "OK") != NULL)
            {
                ESP8266_DEBUG_MSG("RAW 发布成功\\r\\n");
                g_tx_state = TX_STATE_IDLE;
                g_tx_has_active = false;
                Clear_Buff();
            }
            else if ((strstr(At_Rx_Buff, "ERROR") != NULL) || ((int32_t) (now - g_tx_deadline) >= 0))
            {
                // 模块回 ERROR 或等待超时：按失败处理并复位
                LOGW("[TX] RAW publish timeout topic=%s\r\n", g_tx_active.topic);
                g_tx_state = TX_STATE_IDLE;
                g_tx_has_active = false;
                Clear_Buff();
            }
            break;

        case TX_STATE_IDLE:
        default:
            break;
    }
}
/*
 * ESP8266 所在 UART2 的中断回调函数。
 *
 * 该回调主要处理两类事件：
 * 1. UART_EVENT_RX_CHAR
 *    每收到 1 个字节就进入一次，用于把 ESP8266 回包分发到不同缓冲区
 * 2. UART_EVENT_TX_COMPLETE
 *    串口硬件发送完成时置位标志，通知上层“这一帧已经发完”
 *
 * RX 路径里同时维护了三条数据通路：
 * - At_Rx_Buff   : 线性文本缓冲，给阻塞式 AT 命令/状态机用 strstr 检查返回值
 * - g_http_rx_buf: SoftAP 配网模式下保留原始字节流，供 HTTP/+IPD 解析使用
 * - g_cmd_rx_buf : 只接收完整的 +MQTTSUBRECV 行，供 MQTT 下行命令处理
 *
 * 这种“同一串口、按用途分流”的方式可以避免不同业务相互干扰。
 */
void esp8266_uart2_callback(uart_callback_args_t * p_args)
{
       switch(p_args->event)
       {
           case UART_EVENT_RX_CHAR:
 
              uint8_t data = (uint8_t)p_args->data;
 
            /*
             * 1. 先存入线性 AT 接收缓冲区。
             *
             * 这里的 At_Rx_Buff 主要给：
             * - 阻塞式 AT 命令函数
             * - 非阻塞发送状态机
             * 用来通过 strstr() 查找 "OK"、"ERROR"、">"、"ready" 等关键回复。
             *
             * 每写入一个字符后都立即补 '\0'，这样外部即使在“半行状态”下调用
             * strstr() 也不会因为字符串未封尾而越界。
             */
             if (Uart2_Num < 255) {
                 At_Rx_Buff[Uart2_Num++] = (char)data;
                 At_Rx_Buff[Uart2_Num] = '\0'; // 随时封端，防止 strstr 跑飞
             }
 
            /*
             * 2. SoftAP 配网模式下，额外保留原始字节流。
             *
             * 配网页面/HTTP 请求处理依赖 ESP8266 的原始输入字节，因此这里不能只保留
             * 文本化后的行数据，还要把字节原样塞进 g_http_rx_buf。
             *
             * 如果环形缓冲区已满，put() 会失败，此时只累计溢出计数，不在中断里打印日志，
             * 避免中断处理过重。
             */
             if (NET_Manager_IsProvisioningRunning() && (g_http_rx_buf.put != NULL))
             {
                 if (g_http_rx_buf.put(&g_http_rx_buf, data) != 0)
                 {
                     g_uart2_rb_overflow_cnt++;
                }
            }
            

            /*
             * 3. 把当前字节继续拼进“命令行缓冲区”。
             *
             * 这里的目标不是保存所有串口文本，而是以“行”为单位筛选 MQTT 订阅下行：
             * 只有完整一行里包含 "+MQTTSUBRECV" 时，后面才会转发到 g_cmd_rx_buf。
             *
             * 若单行长度超出 CMD_LINE_BUF_SIZE：
             * - 置位 g_cmd_line_overflow
             * - 本行后续字符不再写入
             * - 一直等到遇到 '\n' 才整体丢弃并恢复
             *
             * 这样可避免异常长回包破坏缓冲区内容。
             */
             if (!g_cmd_line_overflow)
             {
                 if (g_cmd_line_len < (CMD_LINE_BUF_SIZE - 1U))
                 {
                     g_cmd_line_buf[g_cmd_line_len++] = (char)data;
                    g_cmd_line_buf[g_cmd_line_len] = '\0';
                }
                else
                {
                    /* 单行过长，丢弃到行尾后再恢复 */
                    g_cmd_line_overflow = true;
                }
             }
 
             //当前接收到的数据到了行尾开始判断本行有没有发生 overflow以及是否包含 "+MQTTSUBRECV"
             if (data == '\n')
             {
                 /*
                  * 4. 一行接收完成后进行筛选和转发。
                  *
                  * 条件：
                  * - 本行没有发生 overflow
                  * - 本行中包含 "+MQTTSUBRECV"
                  *
                  * 满足条件时，将整行逐字节写入 g_cmd_rx_buf，交给后续 MQTT 订阅消息解析器。
                  * 这里仍然使用环形缓冲区，是为了把“中断收包”和“主循环解析”解耦。
                  */
                 if ((!g_cmd_line_overflow) && (strstr(g_cmd_line_buf, "+MQTTSUBRECV") != NULL))
                 {
                     for (uint16_t i = 0; i < g_cmd_line_len; i++)
                     {
                         if ((g_cmd_rx_buf.put != NULL) &&
                            (g_cmd_rx_buf.put(&g_cmd_rx_buf, (uint8_t)g_cmd_line_buf[i]) != 0))
                        {
                            /* 命令缓冲区已满：记录溢出 */
                            g_uart2_rb_overflow_cnt++;
                             break;
                         }
                     }
                 }

                 /* 无论这一行是否有用，行处理结束后都要复位行缓冲状态。 */
                 g_cmd_line_len = 0;
                 g_cmd_line_overflow = false;
                 g_cmd_line_buf[0] = '\0';
             }
 
 
                 break;
 
           case UART_EVENT_TX_COMPLETE:
           {
                /*
                 * UART2 一帧发送完成。
                 *
                 * 这里只表示“MCU 已经把数据通过 UART 发出去了”，
                 * 不代表 ESP8266 已经处理完成、更不代表 MQTT 发布成功。
                 *
                 * 上层通常会把它作为：
                 * - 阻塞式发送函数的退出条件之一
                 * - 非阻塞发送状态机从“等待串口发完”切到“等待模块返回 OK”的依据
                 */
                 Uart2_Send_Flag = true;      //指令发送完成标志
 
                 break;
            }
           default:
               break;
       }
}



