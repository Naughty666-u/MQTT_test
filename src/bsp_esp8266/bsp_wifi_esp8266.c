#include "bsp_wifi_esp8266.h"
#include "bsp_debug_uart.h"
#include "circle_buf.h"
#include "SoftAP_connect_wifi/SoftAP_connect_wifi.h"
#include "Systick.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern circle_buf_t g_rx_buf;
extern circle_buf_t g_cmd_rx_buf;
extern circle_buf_t g_http_rx_buf;

_Bool               Uart2_Send_Flag = false; // 用来判断 UART2 接收以及发送数据是否完成
_Bool               Uart2_Show_Flag = false; // 控制 UART2 收发数据显示标志

/* 用来接收 UART2 数据的线性缓冲区 */
char                At_Rx_Buff[256];
uint8_t             Uart2_Num = 0;

/* UART2 命令环形缓冲溢出统计（中断写满时递增） */
static volatile uint32_t g_uart2_rb_overflow_cnt = 0;
/* 上次打印的溢出计数与时间戳（用于日志节流） */
static uint32_t g_uart2_rb_overflow_last_print = 0;
static uint32_t g_uart2_rb_overflow_last_tick = 0;

/* UART2 行缓冲：仅提取 +MQTTSUBRECV 行写入 cmd_rx_buf */
#define CMD_LINE_BUF_SIZE 768U
static char g_cmd_line_buf[CMD_LINE_BUF_SIZE];
static uint16_t g_cmd_line_len = 0;
static bool g_cmd_line_overflow = false;

/* 临时缓冲区，用于从环形队列中组装一行数据 */
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


static bool tx_queue_push(const char *topic, const char *payload, uint16_t payload_len)
{
    if ((topic == NULL) || (payload == NULL) || (payload_len == 0U))
    {
        return false;
    }
    if ((strlen(topic) >= TX_TOPIC_MAX_LEN) || (payload_len >= TX_PAYLOAD_MAX_LEN))
    {
        return false;
    }
    if (g_tx_q_count >= TX_QUEUE_DEPTH)
    {
        return false;
    }

    mqtt_tx_item_t *slot = &g_tx_queue[g_tx_q_tail];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->topic, topic, TX_TOPIC_MAX_LEN - 1U);
    memcpy(slot->payload, payload, payload_len);
    slot->payload[payload_len] = '\0';
    slot->payload_len = payload_len;

    g_tx_q_tail = (uint8_t)((g_tx_q_tail + 1U) % TX_QUEUE_DEPTH);
    g_tx_q_count++;
    return true;
}

static bool tx_queue_pop(mqtt_tx_item_t *out)
{
    if ((out == NULL) || (g_tx_q_count == 0U))
    {
        return false;
    }

    *out = g_tx_queue[g_tx_q_head];
    g_tx_q_head = (uint8_t)((g_tx_q_head + 1U) % TX_QUEUE_DEPTH);
    g_tx_q_count--;
    return true;
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


/*设置ESP8266为 STA 模式*/
void ESP8266_STA ( void )
{
    ESP8266_AT_Send ( "AT+CWMODE=1\r\n" );

    /*等待设置完成*/
    while ( !Uart2_Send_Flag )
    {
         if (strstr( At_Rx_Buff , "OK\r\n" ))
         {
         ESP8266_DEBUG_MSG("\r\nESP8266已切换为STA模式\r\n");
         Clear_Buff();      //清除缓冲区数据
         }
    }
}
/**
 * @brief 强制恢复出厂设置并完全重置ESP8266
 * @note 该操作会清除所有保存的 WiFi 和 MQTT 信息，确保上电状态的一致性
 */
void ESP8266_Hard_Reset(void)
{
    ESP8266_DEBUG_MSG("\r\n[HARDWARE] 执行 P111 硬件强制复位...\r\n");

    // 1. 先确保引脚输出低电平 (复位生效)
    R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_14, BSP_IO_LEVEL_LOW);
    
    // 2. 持续拉低 100ms (ESP8266 要求复位信号至少持续几百微秒，100ms 绰绰有余)
    R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MILLISECONDS);
    
    // 3. 释放复位信号，拉高（模块开始启动）
    R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_14, BSP_IO_LEVEL_HIGH);

    // 4. 重要：等待模块内部初始化并输出 ready
    // 硬件复位后，模块至少需要 2-3 秒才能连上 WiFi 协议栈
    ESP8266_DEBUG_MSG("[HARDWARE] 复位信号已释放，等待 ready...\r\n");
    
    uint32_t timeout = 500; 
    while (timeout--)
    {
        // 这里的 At_Rx_Buff 会在中断中被填充
        if (strstr(At_Rx_Buff, "ready")) 
        {
            ESP8266_DEBUG_MSG("[HARDWARE] 模块已真正就绪！\r\n");
            break;
        }
        R_BSP_SoftwareDelay(10, BSP_DELAY_UNITS_MILLISECONDS);
    }
    
    // 净化缓冲区，准备发送 AT 指令
    Clear_Buff();
}

/*设置ESP8266为 AP 模式*/
void ESP8266_AP ( void )
{
      ESP8266_AT_Send ( "AT+CWMODE=2\r\n" );

      /*等待设置完成*/
      while ( !Uart2_Send_Flag )
      {
           if (strstr( At_Rx_Buff , "OK\r\n" ))
           {
           ESP8266_DEBUG_MSG("\r\nESP8266已切换为AP模式\r\n");
           Clear_Buff();      //清除缓冲区数据
           }
      }
}


/*设置ESP8266为 STA + AP 模式*/
void ESP8266_STA_AP ( void )
{
      ESP8266_AT_Send ( "AT+CWMODE=3\r\n" );

      /*等待设置完成*/
      while ( !Uart2_Send_Flag )
      {
           if (strstr( At_Rx_Buff , "OK\r\n" ))
           {
           ESP8266_DEBUG_MSG("\r\nESP8266已切换为STA+AP模式\r\n");
           Clear_Buff();      //清除缓冲区数据
           }
      }
}

/*重启ESP8266函数*/
void ESP8266_Rst(void)
{
    ESP8266_AT_Send ( "AT+RST\r\n" );

    /*判断是否设置成功*/
    while ( !Uart2_Send_Flag )
    {
         if (strstr( At_Rx_Buff , "ready\r\n" ))
         {
         R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_SECONDS);  //等待重启完成
         ESP8266_DEBUG_MSG("\r\nESP8266已重启\r\n");
         Clear_Buff();      //清除缓冲区数据
         }
    }
}

/*发布MQTT消息函数*/
void Send_Data( char * topics , char * data )
{
    char  Send_Data[256];

    sprintf( Send_Data , "AT+MQTTPUB=0,\"%s\",\"%s\",1,0\r\n" , topics , data );

    ESP8266_AT_Send( Send_Data );

    while ( !Uart2_Send_Flag )
      {
          R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_SECONDS);  //等待发布时间

          if ( strstr( At_Rx_Buff , "OK\r\n" ) )
           {
               ESP8266_DEBUG_MSG("\r\n消息发布成功\r\n");
               Uart2_Show_Flag = true;  //打开串口回显
               Clear_Buff();      //清除缓冲区数据
               break;
           }
          if ( strstr( At_Rx_Buff , "ALREADY" ) )
           {
               ESP8266_DEBUG_MSG("\r\n消息发布失败，请检查消息格式等信息是否正确\r\n");
               Clear_Buff();      //清除缓冲区数据
               break;
           }
       }
}

/*
 * Send_Data_Raw 语义说明（与旧版不同）：
 * - 旧版：函数内部阻塞等待直到发送成功/失败
 * - 现在：只负责“入队”，立即返回
 * 返回 true 仅表示“入队成功”，实际发送由 ESP8266_TxTask 推进。
 */
bool Send_Data_Raw( char * topics , char * data )
{
    if ((topics == NULL) || (data == NULL))
    {
        return false;
    }

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
/*Wifi串口回调函数*/
void esp8266_uart2_callback(uart_callback_args_t * p_args)
{
       switch(p_args->event)
       {
           case UART_EVENT_RX_CHAR:

              uint8_t data = (uint8_t)p_args->data;

            // 1. 安全存入线性缓冲区（供阻塞指令使用）
            if (Uart2_Num < 255) {
                At_Rx_Buff[Uart2_Num++] = (char)data;
                At_Rx_Buff[Uart2_Num] = '\0'; // 随时封端，防止 strstr 跑飞
            }

            /* SoftAP 配网模式下，保留原始字节流给 +IPD 组帧器 */
            if (NET_Manager_IsProvisioningRunning() && (g_http_rx_buf.put != NULL))
            {
                if (g_http_rx_buf.put(&g_http_rx_buf, data) != 0)
                {
                    g_uart2_rb_overflow_cnt++;
                }
            }
            

            /* 2) 将 UART2 文本按行组装，只转发 +MQTTSUBRECV 到命令环形缓冲区 */
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

            if (data == '\n')
            {
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
                g_cmd_line_len = 0;
                g_cmd_line_overflow = false;
                g_cmd_line_buf[0] = '\0';
            }
//			 /*进入透传模式后打开串口调试助手收发数据显示*/
                if( Uart2_Show_Flag )
                R_SCI_UART_Write(&g_uart0_ctrl, (uint8_t *)&(p_args->data), 1);

                break;

           case UART_EVENT_TX_COMPLETE:
           {
                Uart2_Send_Flag = true;      //指令发送完成标志

                break;
           }
           default:
               break;
       }
}




void Process_RingBuffer_Data(void)
{
    uint8_t ch;
    
    // 循环读取环形缓冲区，直到读空为止
    while (g_rx_buf.get(&g_rx_buf, &ch) == 0)
    {
        // 1. 把读到的字符存入临时行缓冲区
        if (line_idx < 255)
        {
            line_buffer[line_idx++] = ch;
            line_buffer[line_idx] = '\0'; // 动态补结束符
        }
        else
        {
            // 如果一行太长还没收到换行符，强制清空，防止溢出
            line_idx = 0;
        }

        // 2. 判断是否是一行结束(遇到换行符 \n)
        // ESP8266 的回复通常以 \r\n 结尾
        if (ch == '\n')
        {
            // --- 这里就是处理完整一行数据的地方 ---
            
            // 检查是不是 MQTT 接收消息
            // 格式: +MQTTSUBRECV:0,"BBB/1",5,hello
            char *msg_ptr = strstr(line_buffer, "+MQTTSUBRECV");
            
            if (msg_ptr != NULL)
            {
                // 进一步解析
                if (strstr(line_buffer, "\"BBB/1\""))
                {
                    // 查找数据部分（查找第二个逗号后面的内容）
                    // 简单粗暴法：查找数据关键词
                    if (strstr(line_buffer, "hello"))
                    {
                        printf("\r\n[RingBuf] 收到问候: hello\r\n");
                        // R_IOPORT_PinWrite(...) // 动作
                    }
                    else if (strstr(line_buffer, "open_led"))
                    {
                         printf("\r\n[RingBuf] 动作: 开灯\r\n");
                    }
                }
            }

            // 处理完这一行后，清空临时缓冲区，准备接收下一行
            line_idx = 0;
            memset(line_buffer, 0, sizeof(line_buffer));
        }
    }
}
