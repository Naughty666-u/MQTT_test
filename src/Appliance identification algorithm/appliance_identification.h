#ifndef __APPLIANCE_IDENTIFICATION__H
#define __APPLIANCE_IDENTIFICATION__H

#include "hal_data.h"
#include "sdcard_data_handle.h"

typedef enum {
    /* 空闲态：未在采样，也未持有本轮识别结果。 */
    AI_IDLE = 0,
    /* 采样态：窗口内持续更新瞬态特征（如浪涌峰值电流）。 */
    AI_SAMPLING,
    /* 就绪态：采样窗口结束，等待执行一次匹配判定。 */
    AI_READY,
    /* 锁定态：本轮识别完成，等待外部事件触发下一轮。 */
    AI_LOCKED
} AI_State_t;

typedef struct {
    /* 采样窗口内观测到的最大电流（用于区分启动特征）。 */
    float i_max;
    /* 本轮采样开始时刻（毫秒 tick）。 */
    uint32_t start_tick;
    /* 当前插孔 AI 状态机状态。 */
    AI_State_t state;
} Socket_AI_Ctrl_t;

/* 控制单路插孔继电器开关，并同步清理/初始化该路 AI 运行态。 */
void Socket_Command_Handler(uint8_t index, bool target_on);
/* 主识别引擎：采样窗口 -> 特征冻结 -> SD 匹配 -> 命中或进入 pending。 */
void AI_Recognition_Engine(uint8_t index, float p_now, float v_now, float i_now, float pf_now);
/* 兼容旧接口：当前直接复用识别流程。 */
void AI_Learning_Engine(uint8_t index, float p_now, float v_now, float i_now, float pf_now);
/* 用当前特征与 SD 设备库做相似度匹配，返回设备名或 Unknown/SD Error。 */
char * Identify_Appliance_In_SD(float p_now, float pf_now, float i_max, float v_now, float i_steady);
/* 触发该插孔开始一次采样窗口（仅在 AI_IDLE 时生效）。 */
void AI_Trigger_Sampling(uint8_t index);
/* 复位该插孔 AI 状态机到空闲态。 */
void AI_Reset(uint8_t index);

/* 查询该插孔是否存在待命名的 Unknown 样本（pending）。 */
bool AI_Is_Pending(uint8_t index);
/* 获取该插孔当前 pendingId（用于 LEARN_COMMIT 对齐样本）。 */
uint32_t AI_Get_PendingId(uint8_t index);
/* 提交 Unknown 命名：校验 pendingId 后把样本写入 SD 设备库。 */
FRESULT AI_Commit_Pending(uint8_t index, uint32_t pending_id, const char *name);
/* 快速受理 LEARN_COMMIT：仅做轻量校验并排队，实际写 SD 在后台任务执行。 */
bool AI_Request_Commit_Pending(uint8_t index, uint32_t pending_id, const char *name);
/* 后台提交任务推进器：主循环高频调用。 */
void AI_Commit_Task(void);
/* 软件插拔重学习：OFF->ON 生成干净上电瞬态后再触发采样。 */
void AI_Request_Relearn_Replug(uint8_t index);
/* 非阻塞软件插拔任务推进器：主循环高频调用。 */
void AI_Replug_Task(void);

#endif
