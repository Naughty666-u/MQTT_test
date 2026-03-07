#include "bsp_wifi_esp8266.h"
#include "bsp_debug_uart.h"
#include "circle_buf.h"

extern circle_buf_t g_rx_buf;

_Bool               Uart2_Send_Flag = false; //用来判断UART2接收以及发送数据是否完成
_Bool               Uart2_Show_Flag = false;    //控制UART2收发数据显示标志

/*用来接收UART2数据的缓冲区*/
char                At_Rx_Buff[256];
uint8_t             Uart2_Num = 0;


/* 临时缓冲区，用于从环形队列中组装一行数据 */
static char line_buffer[256]; 
static uint8_t line_idx = 0;


void ESP8266_MQTT_Test(void)
{
    ESP8266_DEBUG_MSG("\r\n[SYSTEM] 开始初始化智能排插...\r\n");
    ESP8266_UART2_Init();
    ESP8266_Hard_Reset();
	R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_SECONDS); // 复位后多等 1s
    ESP8266_STA(); 
	R_BSP_SoftwareDelay(500, BSP_DELAY_UNITS_MILLISECONDS); // 给 ATE0 处理时间
    ESP8266_ATE0();
	R_BSP_SoftwareDelay(500, BSP_DELAY_UNITS_MILLISECONDS); // 给 ATE0 处理时间
	
	// 4. 【新增】降低发射功率 (针对供电不稳的 ESP-01S 非常有效)
    // 降低到 50 (最大 82)，减小瞬间电流峰值，防止电压跌落导致重启
    ESP8266_AT_Send("AT+RFPOWER=50\r\n");
    R_BSP_SoftwareDelay(200, BSP_DELAY_UNITS_MILLISECONDS);

	
    ESP8266_STA_JoinAP(ID, PASSWORD, 20);

    // 4. 配置 MQTT 属性 (注意：如果你的函数支持，应在这里传入遗嘱参数)
    // 根据文档 [cite: 147, 148]，遗嘱 Payload 应为 {"reason": "power_off"}
    ESP8266_DEBUG_MSG("配置 MQTT 用户属性\r\n");
    MQTT_SetUserProperty(CLIENT_ID, USER_NAME, USER_PASSWORD);
    

    // 5. 连接 MQTT 服务器
    ESP8266_DEBUG_MSG("连接 MQTT 服务器...\r\n");
    Connect_MQTT(MQTT_IP, MQTT_Port, 100);

    // 6. 订阅指令主题 [cite: 28, 29]
    ESP8266_DEBUG_MSG("订阅命令主题: %s\r\n", MQTT_SUB_TOPIC);
    Subscribes_Topics(MQTT_SUB_TOPIC); 

//    // 7. 第一次上线状态上报 (参照文档 7.2 节) [cite: 161]
//    ESP8266_DEBUG_MSG("上报初始在线状态...\r\n");
//    // 构造一个符合文档要求的简易 JSON [cite: 165-174]
//    char *init_payload = "{\"ts\":1772000100,\"online\":true,\"total_power_w\":0.0,\"voltage_v\":220.0,\"current_a\":0.0,\"sockets\":[]}";
//    Send_Data_Raw(MQTT_PUB_STATUS, init_payload);
		ESP8266_DEBUG_MSG("初始化完成，进入监听模式...\r\n");
		memset(At_Rx_Buff, 0, sizeof(At_Rx_Buff)); // 清除阻塞缓冲区
		// 重点：排空环形缓冲区，防止初始化期间的残留字符干扰 JSON 解析
		uint8_t dummy;
		while (g_rx_buf.get(&g_rx_buf, &dummy) == 0);
}


/*ESP8266 (SPI2 UART) 初始化函数*/
void ESP8266_UART2_Init(void)
{
    fsp_err_t err = FSP_SUCCESS;

    err = R_SCI_UART_Open(g_uart2_esp8266.p_ctrl, g_uart2_esp8266.p_cfg);
   
}


void ESP8266_AT_Send(char * cmd )
{
    /* 必须先设为 false，再启动传输！ */
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
 * @brief 强制恢复出厂设置并完全重置 ESP8266
 * @note 该操作会清除所有保存的 WiFi 和 MQTT 信息，确保上电状态的一致性
 */
void ESP8266_Hard_Reset(void)
{
    ESP8266_DEBUG_MSG("\r\n[HARDWARE] 执行 P111 硬件强制复位...\r\n");

    // 1. 先确保引脚输出低电平 (复位生效)
    R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_14, BSP_IO_LEVEL_LOW);
    
    // 2. 持续拉低 100ms (ESP8266 要求复位信号至少持续几百微秒，100ms 绰绰有余)
    R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MILLISECONDS);
    
    // 3. 释放复位信号，拉高 (模块开始启动)
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

/*ESP8266连接WiFi函数，timeout：期望最大连接时间*/
void ESP8266_STA_JoinAP( char * id ,  char * password , uint8_t timeout )
{
    char  JoinAP_AT[256];

    uint8_t i;

    sprintf( JoinAP_AT , "AT+CWJAP=\"%s\",\"%s\"\r\n" , id , password);

    ESP8266_AT_Send( JoinAP_AT );

   /*判断连接是否设置成功，失败则打印错误信息*/
   while ( !Uart2_Send_Flag )
     {
    for(i = 0; i <= timeout; i++)
       {
          if ( strstr( At_Rx_Buff , "OK\r\n" ) )
           {
               ESP8266_DEBUG_MSG("\r\nWifi连接成功\r\n");
               Clear_Buff();      //清除缓冲区数据
               break;
           }
          if ( strstr( At_Rx_Buff , "ERROR\r\n" ) )
           {
               if( strstr( At_Rx_Buff , "+CWJAP:1\r\n" ))
               ESP8266_DEBUG_MSG("\r\nWifi连接超时，请检查各项配置是否正确\r\n");

               if( strstr( At_Rx_Buff , "+CWJAP:2\r\n" ))
               ESP8266_DEBUG_MSG("\r\nWifi密码错误，请检查Wifi密码是否正确\r\n");

               if( strstr( At_Rx_Buff , "+CWJAP:3\r\n" ))
                   ESP8266_DEBUG_MSG("\r\n无法找到目标Wifi，请检查Wifi是否打开或Wifi名称是否正确\r\n");

               if( strstr( At_Rx_Buff , "+CWJAP:4\r\n" ))
               ESP8266_DEBUG_MSG("\r\nWifi连接失败，请检查各项配置是否正确\r\n");

               while(1)
               {
                ESP8266_ERROR_Alarm();
               }      //LED灯警告错误，红灯闪烁
           }
           if ( i == timeout )
           {
               ESP8266_DEBUG_MSG("\r\nWifi连接超出期望时间，请检查各项配置是否正确\r\n");
               while(1)
               {
                ESP8266_ERROR_Alarm();
               }      //LED灯警告错误，红灯闪烁
           }
           R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_SECONDS);
        }
      }
}

/*设置 MQTT 用户属性*/
void MQTT_SetUserProperty( char * client_id , char * user_name, char * user_password )
{
    char  SetUserProperty_AT[256];

    sprintf( SetUserProperty_AT , "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"\r\n" , client_id , user_name , user_password);

    ESP8266_AT_Send ( SetUserProperty_AT );

    /*等待设置完成*/
    while ( !Uart2_Send_Flag )
    {
         if (strstr( At_Rx_Buff , "OK\r\n" ))
         {
         ESP8266_DEBUG_MSG("\r\nMQTT用户属性已设置完成\r\n");
         Clear_Buff();      //清除缓冲区数据
         }
    }

}

/*连接MQTT服务器函数*/
void Connect_MQTT( char * mqtt_ip , char * mqtt_port , uint8_t timeout )
{
	
    char  Connect_MQTT_AT[256];

    uint8_t i;

    sprintf( Connect_MQTT_AT , "AT+MQTTCONN=0,\"%s\",%s,1\r\n" , mqtt_ip , mqtt_port);

    ESP8266_AT_Send( Connect_MQTT_AT );

       /*判断连接是否设置成功，失败则打印错误信息*/
    while ( !Uart2_Send_Flag )
     {
    for(i = 0; i <= timeout; i++)
       {
          if ( strstr( At_Rx_Buff , "OK\r\n" ) )
           {
               ESP8266_DEBUG_MSG("\r\nMQTT服务器连接成功\r\n");
               Clear_Buff();      //清除缓冲区数据
               break;
           }
          if ( strstr( At_Rx_Buff , "ERROR\r\n" ) )
           {
               ESP8266_DEBUG_MSG("\r\nMQTT服务器连接失败，请检查各项配置是否正确\r\n");
               while(1)
               {
                ESP8266_ERROR_Alarm();
               }      //LED灯警告错误，红灯闪烁
           }
           if ( i == timeout )
           {
               ESP8266_DEBUG_MSG("\r\nMQTT服务器连接超出期望时间，请检查各项配置是否正确\r\n");
               while(1)
               {
                ESP8266_ERROR_Alarm();
               }      //LED灯警告错误，红灯闪烁
           }
           R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_SECONDS);
        }
      }


}

/*订阅主题函数*/
void Subscribes_Topics( char * topics )
{
    char  Sub_Topics_AT[256];

    sprintf( Sub_Topics_AT , "AT+MQTTSUB=0,\"%s\",1\r\n" , topics);

    ESP8266_AT_Send( Sub_Topics_AT );

    while ( !Uart2_Send_Flag )
      {
          R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_SECONDS);  //等待订阅时间

          if ( strstr( At_Rx_Buff , "OK" ) )
           {
               ESP8266_DEBUG_MSG("\r\n主题订阅成功\r\n");
               Clear_Buff();      //清除缓冲区数据
               break;
           }
          if ( strstr( At_Rx_Buff , "ALREADY\r\n" ) )
           {
               ESP8266_DEBUG_MSG("\r\n已经订阅过该主题\r\n");
               Clear_Buff();      //清除缓冲区数据
               break;
           }
       }
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

    uint32_t timeout = 100; // 500ms
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

bool Send_Data_Raw( char * topics , char * data )
{
    char cmd_buf[256];

    if ((topics == NULL) || (data == NULL))
    {
        return false;
    }

    uint16_t data_len = (uint16_t)strlen(data);
    if (data_len == 0U)
    {
        return false;
    }

    // 1) 发送发布命令并准备等待提示符
    sprintf(cmd_buf, "AT+MQTTPUBRAW=0,\"%s\",%d,1,0\r\n", topics, data_len);
    Clear_Buff();
    ESP8266_AT_Send(cmd_buf);

    // 2) 等待 ESP8266 返回 '>' 提示符
    uint32_t timeout = 200;
    while (timeout--)
    {
        if (strchr(At_Rx_Buff, '>')) break;
        R_BSP_SoftwareDelay(5, BSP_DELAY_UNITS_MILLISECONDS);
    }
    if (timeout == 0)
    {
        printf("[TX] RAW prompt timeout topic=%s\r\n", topics);
        Clear_Buff();
        return false;
    }

    // 3) 发送消息载荷
    Uart2_Send_Flag = false;
    fsp_err_t werr = R_SCI_UART_Write(&g_uart2_esp8266_ctrl, (uint8_t *)data, data_len);
    if (werr != FSP_SUCCESS)
    {
        printf("[TX] RAW write fail err=%d topic=%s\r\n", (int)werr, topics);
        Clear_Buff();
        return false;
    }

    uint32_t safety = 0xFFFF;
    while(!Uart2_Send_Flag && safety--);
    if (safety == 0)
    {
        printf("[TX] RAW tx complete timeout topic=%s\r\n", topics);
        Clear_Buff();
        return false;
    }

    // 4) 等待最终 "OK" 回执
    timeout = 200;
    bool ok = false;
    while (timeout--)
    {
        if (strstr(At_Rx_Buff, "OK"))
        {
            ESP8266_DEBUG_MSG("RAW发送成功\r\n");
            ok = true;
            break;
        }
        R_BSP_SoftwareDelay(5, BSP_DELAY_UNITS_MILLISECONDS);
    }

    if (!ok)
    {
        printf("[TX] RAW publish timeout topic=%s\r\n", topics);
    }

    Clear_Buff();
    return ok;
}

/*Wifi串口回调函数*/
void esp8266_uart2_callback(uart_callback_args_t * p_args)
{
       switch(p_args->event)
       {
           case UART_EVENT_RX_CHAR:

              uint8_t data = (uint8_t)p_args->data;

           // 1. 安全存入线性缓冲区 (供阻塞指令使用)
            if (Uart2_Num < 255) {
                At_Rx_Buff[Uart2_Num++] = (char)data;
                At_Rx_Buff[Uart2_Num] = '\0'; // 随时封端，防止 strstr 跑飞
            }
            

            /* 2. 【新逻辑】存入环形缓冲区，供主循环异步解析 */
            /* 只要初始化了环形缓冲区，这里就会自动存入，非常快，不堵塞 */
            if (g_rx_buf.put != NULL)
            {
                g_rx_buf.put(&g_rx_buf, data);
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

        // 2. 判断是否是一行结束 (遇到换行符 \n)
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
                    // 查找数据部分 (查找第二个逗号后面的内容)
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
