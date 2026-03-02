#ifndef __APPLIANCE_IDENTIFICATION__H
#define	__APPLIANCE_IDENTIFICATION__H

#include "hal_data.h"

typedef enum {
    AI_IDLE = 0,       // 等触发
    AI_SAMPLING,       // 采样窗口中
    AI_READY,          // 采样结束待识别
    AI_LOCKED          // 已识别，结果锁住（直到 OFF 或低功耗）
} AI_State_t;


typedef struct {
    float i_max;            // 启动瞬间最大电流
    uint32_t start_tick;    // 触发时刻
    AI_State_t state;       // 当前插座的AI状态
} Socket_AI_Ctrl_t;


void Socket_Command_Handler(uint8_t index, bool target_on);
void AI_Recognition_Engine(uint8_t index, float p_now,float v_now ,float i_now, float pf_now);
void AI_Learning_Engine(uint8_t index, float p_now,float v_now ,float i_now, float pf_now);
char* Identify_Appliance_In_SD(float p_now, float pf_now, float i_max, float v_now, float i_steady);
void AI_Trigger_Sampling(uint8_t index);
void AI_Reset(uint8_t index);  // 可选：关断/离线时复位AI
#endif

