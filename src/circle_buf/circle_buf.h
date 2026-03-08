#ifndef __CIRCLE_BUF_H
#define __CIRCLE_BUF_H
#include "hal_data.h"
typedef struct circle_buf
{
    uint32_t r;                                          // 读位置
    uint32_t w;                                          // 写位置
    uint32_t max_len;                                    // buffer大小
    uint8_t *buffer;                                     // buffer指针
    int32_t (*put)(struct circle_buf *pcb, uint8_t v);   // 写buffer
    int32_t (*get)(struct circle_buf *pcb, uint8_t *pv); // 读buffer
} circle_buf_t;
extern circle_buf_t g_rx_buf;
extern circle_buf_t g_cmd_rx_buf;
extern circle_buf_t g_BL0942_rx_buf;
void BL0942_circlebuf_init(void);
void circlebuf_init(void);   //初始化环形缓冲区结构体变量
void BL0942_circlebuf_clear(void);
static int32_t circlebuf_put(struct circle_buf *pcb, uint8_t v);  //写缓冲区
static int32_t circlebuf_get(struct circle_buf *pcb, uint8_t *pv);//读缓冲区

#endif
