#include "circle_buf.h"

/* UART2 命令接收环形缓冲区：从 1024 扩容到 4096，降低突发丢包概率 */
#define UART2_RX_BUF_SIZE   4096U
uint8_t rx_buf[UART2_RX_BUF_SIZE];
circle_buf_t g_rx_buf;

/* 云命令专用接收环形缓冲区（+MQTTSUBRECV 行） */
#define CMD_RX_BUF_SIZE     4096U
static uint8_t cmd_rx_buf[CMD_RX_BUF_SIZE];
circle_buf_t g_cmd_rx_buf;

/* SoftAP HTTP 配网专用接收缓冲区（+IPD 行） */
#define HTTP_RX_BUF_SIZE    4096U
static uint8_t http_rx_buf[HTTP_RX_BUF_SIZE];
circle_buf_t g_http_rx_buf;

uint8_t BL0942_rx_buf[48];
circle_buf_t g_BL0942_rx_buf;

static int32_t circlebuf_get(struct circle_buf *pcb, uint8_t *pv); // 读buffer
static int32_t circlebuf_put(struct circle_buf *pcb, uint8_t v);   // 写buffer
void circlebuf_init(void)
{
    g_rx_buf.r = g_rx_buf.w = 0;
    g_rx_buf.buffer = rx_buf;
    g_rx_buf.max_len = sizeof(rx_buf);
    g_rx_buf.get = circlebuf_get;
    g_rx_buf.put = circlebuf_put;

    g_cmd_rx_buf.r = g_cmd_rx_buf.w = 0;
    g_cmd_rx_buf.buffer = cmd_rx_buf;
    g_cmd_rx_buf.max_len = sizeof(cmd_rx_buf);
    g_cmd_rx_buf.get = circlebuf_get;
    g_cmd_rx_buf.put = circlebuf_put;

    g_http_rx_buf.r = g_http_rx_buf.w = 0;
    g_http_rx_buf.buffer = http_rx_buf;
    g_http_rx_buf.max_len = sizeof(http_rx_buf);
    g_http_rx_buf.get = circlebuf_get;
    g_http_rx_buf.put = circlebuf_put;
}
void BL0942_circlebuf_init(void)
{
    g_BL0942_rx_buf.r = g_BL0942_rx_buf.w = 0;
    g_BL0942_rx_buf.buffer = BL0942_rx_buf;
    g_BL0942_rx_buf.max_len = sizeof(BL0942_rx_buf);
    g_BL0942_rx_buf.get = circlebuf_get;
    g_BL0942_rx_buf.put = circlebuf_put;
}

void BL0942_circlebuf_clear(void)
{
    g_BL0942_rx_buf.r = g_BL0942_rx_buf.w = 0;
    memset(g_BL0942_rx_buf.buffer, 0, g_BL0942_rx_buf.max_len);
}
// 写环形缓冲区
static int32_t circlebuf_put(struct circle_buf *pcb, uint8_t v)
{
    uint32_t next_w;
    // 计算下一个写位置的下一个，如果越界则设置为0
    next_w = pcb->w + 1;
    if (next_w == pcb->max_len)
        next_w = 0;
    // 未满
    if (next_w != pcb->r)
    {
        // 写入数据
        pcb->buffer[pcb->w] = v;
        // 设置下一个写位置
        pcb->w = next_w;
        return 0;
    }
    return -1;
}
// 读环形缓冲区
static int32_t circlebuf_get(struct circle_buf *pcb, uint8_t *pv)
{
    // 缓冲区不为空
    if (pcb->w != pcb->r)
    {
        // 读出数据
        *pv = pcb->buffer[pcb->r];
        // 计算下一个位置，如果越界要设置为0
        pcb->r++;
        if (pcb->r == pcb->max_len)
            pcb->r = 0;
        return 0;
    }
    return -1;
}
