#ifndef __APP_LOG_H__
#define __APP_LOG_H__

#include <stdint.h>
#include <stdio.h>
#include "Systick.h"

#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

#if (LOG_LEVEL <= LOG_LEVEL_DEBUG)
#define LOGD(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define LOGD(fmt, ...)
#endif

#if (LOG_LEVEL <= LOG_LEVEL_INFO)
#define LOGI(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define LOGI(fmt, ...)
#endif

#if (LOG_LEVEL <= LOG_LEVEL_WARN)
#define LOGW(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define LOGW(fmt, ...)
#endif

#if (LOG_LEVEL <= LOG_LEVEL_ERROR)
#define LOGE(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define LOGE(fmt, ...)
#endif

#define LOG_THROTTLE_MS(last_tick_var, interval_ms, fmt, ...)                  \
    do {                                                                        \
        uint32_t _log_now = HAL_GetTick();                                     \
        if ((uint32_t)(_log_now - (last_tick_var)) >= (uint32_t)(interval_ms)) \
        {                                                                       \
            (last_tick_var) = _log_now;                                        \
            printf(fmt, ##__VA_ARGS__);                                        \
        }                                                                       \
    } while (0)

#endif
