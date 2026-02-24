#include "circle_buf.h"

 uint8_t rx_buf[1024];
circle_buf_t g_rx_buf;
static int32_t circlebuf_get(struct circle_buf *pcb, uint8_t *pv); // 读buffer
static int32_t circlebuf_put(struct circle_buf *pcb, uint8_t v);   // 写buffer
void circlebuf_init(void)
{
    g_rx_buf.r = g_rx_buf.w = 0;
    g_rx_buf.buffer = rx_buf;
    g_rx_buf.max_len = sizeof(rx_buf);
    g_rx_buf.get = circlebuf_get;
    g_rx_buf.put = circlebuf_put;
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