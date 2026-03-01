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

    float p_filt;          // 功率滤波值
    float p_baseline;      // 基线（低功率/空载漂移）
    uint8_t on_cnt;        // ON 去抖计数
    uint8_t off_cnt;       // OFF 去抖计数

    uint32_t last_trigger_tick; // 上次触发识别的时刻（用于冷却）
} EventDetector_t;

void EventDetector_Init(EventDetector_t *d);
bool EventDetector_Update(EventDetector_t *d,
                          bool socket_on,
                          float p_now,
                          uint32_t now_tick);

#endif