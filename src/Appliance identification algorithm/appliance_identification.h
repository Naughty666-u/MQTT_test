#ifndef __APPLIANCE_IDENTIFICATION__H
#define	__APPLIANCE_IDENTIFICATION__H

#include "hal_data.h"


typedef enum {
    AI_IDLE,          // 待机：插座无电器或处于稳态
    AI_SAMPLING,      // 采样：检测到功率跳变，正在记录峰值
    AI_READY          // 完成：特征已提取，等待匹配数据库
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

