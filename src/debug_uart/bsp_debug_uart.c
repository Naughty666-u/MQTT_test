#include "bsp_debug_uart.h"
#include "bsp_wifi_esp8266.h"
#include "circle_buf.h"
extern circle_buf_t g_rx_buf;
/* 调试串口 UART0 初始化 */
void Debug_UART0_Init(void)
{
    fsp_err_t err = FSP_SUCCESS;
    
    err = R_SCI_UART_Open (&g_uart0_ctrl, &g_uart0_cfg);
    
}



/* 发送完成标志 */
volatile bool uart_send_complete_flag = false;


/* 串口中断回调 */
void debug_uart0_callback (uart_callback_args_t * p_args)
{
    switch (p_args->event)
    {
        case UART_EVENT_RX_CHAR:
        {
			R_SCI_UART_Write(&g_uart2_esp8266_ctrl, (uint8_t*)p_args->data,1);
           /* 核心修改：将接收到的数据压入环形缓冲区 */
            // p_args->data 包含了当前接收到的 1 字节数据
            if (g_rx_buf.put(&g_rx_buf, (uint8_t)p_args->data) != 0)
            {
                // 如果返回 -1，说明环形缓冲区满了（溢出）
                // 在物联网应用中，这里可以做一个简单的错误计数或调试打印
            }
            break;
        }
        case UART_EVENT_TX_COMPLETE:
        {
            uart_send_complete_flag = true;
            break;
        }
        default:
            break;
    }
}


/* 重定向 printf 输出 */
#if defined __GNUC__ && !defined __clang__
int _write(int fd, char *pBuffer, int size); //防止编译警告
int _write(int fd, char *pBuffer, int size)
{
    (void)fd;
    R_SCI_UART_Write(&g_uart0_ctrl, (uint8_t *)pBuffer, (uint32_t)size);
    while(uart_send_complete_flag == false);
    uart_send_complete_flag = false;

    return size;
}
#else
int fputc(int ch, FILE *f)
{
    (void)f;
    R_SCI_UART_Write(&g_uart0_ctrl, (uint8_t *)&ch, 1);
    while(uart_send_complete_flag == false);
    uart_send_complete_flag = false;

    return ch;
}
#endif





