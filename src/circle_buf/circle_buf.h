#ifndef __CIRCLE_BUF_H
#define __CIRCLE_BUF_H

#include "hal_data.h"

typedef struct circle_buf
{
    uint32_t r;
    uint32_t w;
    uint32_t max_len;
    uint8_t *buffer;
    int32_t (*put)(struct circle_buf *pcb, uint8_t v);
    int32_t (*get)(struct circle_buf *pcb, uint8_t *pv);
} circle_buf_t;

extern circle_buf_t g_rx_buf;
extern circle_buf_t g_cmd_rx_buf;
extern circle_buf_t g_http_rx_buf;
extern circle_buf_t g_BL0942_rx_buf;

void BL0942_circlebuf_init(void);
void circlebuf_init(void);
void BL0942_circlebuf_clear(void);

/* 内部实现函数声明（保留原有头文件接口习惯） */
static int32_t circlebuf_put(struct circle_buf *pcb, uint8_t v);
static int32_t circlebuf_get(struct circle_buf *pcb, uint8_t *pv);

#endif
