#ifndef EVENT_DETECTOR_H
#define EVENT_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    EVT_STATE_OFF = 0,
    EVT_STATE_ON  = 1,
} evt_state_t;

typedef struct {
    evt_state_t state;

    /* 低通滤波后的功率值（W）。 */
    float p_filt;

    /* 空载基线功率估计值（W），主要在 OFF 状态更新。 */
    float p_baseline;

    /* OFF->ON 与 ON->OFF 的去抖计数器。 */
    uint8_t on_cnt;
    uint8_t off_cnt;

    /* 最近一次上报 OFF->ON 触发的时间戳（ms tick）。 */
    uint32_t last_trigger_tick;
} EventDetector_t;

void EventDetector_Init(EventDetector_t *d);

/* 仅在确认发生 OFF->ON 且通过冷却时间检查时返回 true。 */
bool EventDetector_Update(EventDetector_t *d,
                          bool socket_on,
                          float p_now,
                          uint32_t now_tick);

#endif
