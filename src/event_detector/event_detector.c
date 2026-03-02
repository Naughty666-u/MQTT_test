#include "event_detector.h"
#include "stdio.h"
#define P_ON_ABS_W          (5.0f)   // 绝对开机阈值（先用 5W，后续按噪声调）
#define P_OFF_ABS_W         (2.0f)   // 绝对关机阈值（迟滞）
#define P_ON_REL_W          (6.0f)   // 相对基线增量阈值（防漂移误触发）
#define P_OFF_REL_W         (3.0f)

#define ON_DEBOUNCE_CNT     (5)      // 连续2个点满足才算ON（按你100ms约0.5s）
#define OFF_DEBOUNCE_CNT    (15)      // 连续4个点满足才算OFF（约1.5s）

#define TRIGGER_COOLDOWN_MS (8000)   // 触发冷却时间：8秒内不重复触发

// 一阶低通滤波系数：越大越平滑（0.7~0.9都可以）
#define ALPHA_P             (0.80f)
// 基线更新系数：越大越慢（空载漂移慢更新）
#define ALPHA_BASE          (0.95f)
static uint32_t last_dbg = 0;
static float fmaxf_local(float a, float b) { return a > b ? a : b; }

void EventDetector_Init(EventDetector_t *d)
{
    d->state = EVT_STATE_OFF;
    d->p_filt = 0.0f;
    d->p_baseline = 0.0f;
    d->on_cnt = 0;
    d->off_cnt = 0;

    // ✅ 关键：保证上电后第一次一定允许触发
    d->last_trigger_tick = (uint32_t)(0 - TRIGGER_COOLDOWN_MS);  
}

/**
 * @brief  事件检测核心函数：实现带动态基线、消抖及冷却保护的状态机
 * @param  socket_on: 继电器物理状态 (硬件层面的开关状态)
 * @param  p_now: 传感器当前测得的瞬时功率
 * @param  now_tick: 系统当前毫秒计数 (HAL_GetTick)
 * @return true: 触发了一次有效的开启事件 (OFF -> ON)
 */
bool EventDetector_Update(EventDetector_t *d, bool socket_on, float p_now, uint32_t now_tick)
{
    /* --- 1. 物理安全锁：如果插座已关闭 --- */
    if (!socket_on)
    {
        d->state = EVT_STATE_OFF; // 强制状态为关闭
        d->on_cnt = 0;
        d->off_cnt = 0;

        // 当插座断开时，快速将滤波值和基线向 0 拉回，防止下次上电时残留旧数据
        d->p_filt = ALPHA_P * d->p_filt;
        d->p_baseline = ALPHA_BASE * d->p_baseline;

        return false;
    }

    /* --- 2. 信号预处理 --- */
    // 低通滤波：平滑功率波动，防止由于电流采样瞬时毛刺导致的误触发
    d->p_filt = ALPHA_P * d->p_filt + (1.0f - ALPHA_P) * p_now;

    // 动态基线更新：仅在 OFF 状态下学习。
    // 这能有效滤除传感器随温度产生的零点漂移 (例如：没插设备时显示 0.5W)
    if (d->state == EVT_STATE_OFF)
    {
        d->p_baseline = ALPHA_BASE * d->p_baseline 
                      + (1.0f - ALPHA_BASE) * d->p_filt;
    }

    /* --- 3. 计算自适应阈值 --- */
    // p_on_th = 取 (绝对阈值) 与 (基线 + 相对增量) 之间的最大值，保证检测的灵敏度与稳定性
    float p_on_th  = fmaxf_local(P_ON_ABS_W,  d->p_baseline + P_ON_REL_W);
    float p_off_th = fmaxf_local(P_OFF_ABS_W, d->p_baseline + P_OFF_REL_W);

    /* --- 4. 调试日志打印 (建议将 last_dbg 移入结构体以支持多路) --- */
    if (now_tick - last_dbg >= 500)
    {
        last_dbg = now_tick;
        printf("[EVT] P=%.2f P_filt=%.2f base=%.2f on_th=%.2f off_th=%.2f state=%s\r\n",
               p_now, d->p_filt, d->p_baseline, p_on_th, p_off_th,
               (d->state == EVT_STATE_ON) ? "ON" : "OFF");
    }

    /* --- 5. 核心状态机逻辑 --- */

    // A. 当前状态：OFF -> 寻找开启时机
    if (d->state == EVT_STATE_OFF)
    {
        if (d->p_filt > p_on_th) // 功率超过开启阈值
        {
            if (++d->on_cnt >= ON_DEBOUNCE_CNT) // 连续 N 次确认，消除合闸瞬间的电弧噪声
            {
                d->on_cnt = 0;
                d->off_cnt = 0;

                /* ⭐ 冷却防火墙：防止用户频繁插拔导致的识别任务堆积 ⭐ */
                if ((now_tick - d->last_trigger_tick) >= TRIGGER_COOLDOWN_MS)
                {
                    d->last_trigger_tick = now_tick; // 更新最近触发时间
                    d->state = EVT_STATE_ON;         // 正式切入 ON 状态

                    printf("[EVT] OFF->ON TRIGGER at P=%.2f (th=%.2f)\r\n",
                           d->p_filt, p_on_th);

                    return true; // 唯一返回 true 的出口，通知主程序：去采样识别吧！
                }
                else
                {
                    // 冷却时间未到：即使功率够大，也保持 OFF 状态，直到冷却结束
                    d->state = EVT_STATE_OFF;
                }
            }
        }
        else
        {
            d->on_cnt = 0; // 功率不足，重置计数器
        }
    }
    // B. 当前状态：ON -> 寻找关闭时机
    else 
    {
        if (d->p_filt < p_off_th) // 功率低于关闭阈值
        {
            if (++d->off_cnt >= OFF_DEBOUNCE_CNT) // 长时间低功率确认设备已关机
            {
                d->off_cnt = 0;
                d->on_cnt = 0;
                d->state = EVT_STATE_OFF; // 回到 OFF 状态，重新开始基线学习
                printf("[EVT] ON->OFF at P=%.2f\r\n", d->p_filt);
            }
        }
        else
        {
            d->off_cnt = 0; // 功率回升，说明设备仍在运行
        }
    }

    return false; // 大部分时间返回 false，不触发识别
}