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
    d->last_trigger_tick = 0;
}

bool EventDetector_Update(EventDetector_t *d,
                          bool socket_on,
                          float p_now,
                          uint32_t now_tick)
{
    // 插座继电器断开时：强制当作 OFF，并慢慢把基线拉回 0
    if (!socket_on)
    {
        d->state = EVT_STATE_OFF;
        d->on_cnt = 0;
        d->off_cnt = 0;
        d->p_filt = ALPHA_P * d->p_filt + (1.0f - ALPHA_P) * 0.0f;
        d->p_baseline = ALPHA_BASE * d->p_baseline + (1.0f - ALPHA_BASE) * 0.0f;
        return false;
    }

    // 1) 低通滤波功率，压掉 BL0942 抖动
    d->p_filt = ALPHA_P * d->p_filt + (1.0f - ALPHA_P) * p_now;

    // 2) 基线：在“认为OFF/低功率”的时候慢更新（避免稳态功率把基线拉高）
    if (d->state == EVT_STATE_OFF)
    {
        d->p_baseline = ALPHA_BASE * d->p_baseline + (1.0f - ALPHA_BASE) * d->p_filt;
    }

    // 3) 阈值（取绝对阈值和相对基线阈值中较大者）
    float p_on_th  = fmaxf_local(P_ON_ABS_W,  d->p_baseline + P_ON_REL_W);
    float p_off_th = fmaxf_local(P_OFF_ABS_W, d->p_baseline + P_OFF_REL_W);
    
	// ===================== 调试打印 =====================


	if (now_tick - last_dbg >= 500)   // 每500ms打印一次
	{
		last_dbg = now_tick;

		printf("[EVT] P=%.2f Pf=%.2f base=%.2f on_th=%.2f off_th=%.2f state=%s\r\n",
			   p_now,
			   d->p_filt,
			   d->p_baseline,
			   p_on_th,
			   p_off_th,
			   (d->state == EVT_STATE_ON) ? "ON" : "OFF");
	}
    // 4) 状态机 + 去抖 + 冷却
    if (d->state == EVT_STATE_OFF)
    {
        if (d->p_filt > p_on_th)
        {
            if (++d->on_cnt >= ON_DEBOUNCE_CNT)
            {
                d->on_cnt = 0;
                d->off_cnt = 0;
                d->state = EVT_STATE_ON;

				if ((now_tick - d->last_trigger_tick) > TRIGGER_COOLDOWN_MS)
				{
					d->last_trigger_tick = now_tick;
					printf("[EVT] OFF->ON TRIGGER at P=%.2f (th=%.2f)\r\n",
						   d->p_filt, p_on_th);

					return true;
				}
            }
        }
        else
        {
            d->on_cnt = 0;
        }
    }
    else // EVT_STATE_ON
    {
        if (d->p_filt < p_off_th)
        {
            if (++d->off_cnt >= OFF_DEBOUNCE_CNT)
            {
                d->off_cnt = 0;
                d->on_cnt = 0;
                d->state = EVT_STATE_OFF;
            }
        }
        else
        {
            d->off_cnt = 0;
        }
    }

    return false;
}