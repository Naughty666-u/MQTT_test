#include "appliance_identification.h"
#include "Systick.h"
#include "stdio.h"
#include "cJSON_handle.h"
#include <math.h>
#include "ff.h"
#include "sdcard_data_handle.h"
#include "Relay.h"
// 为 4 个插座开辟 4 个独立的 AI 控制块
static Socket_AI_Ctrl_t g_ai_ctrl[4] = {0};
extern PowerStrip_t g_strip;



/**
 * @brief  人工/指令控制插座开关 (含 AI 同步触发)
 */
void Socket_Command_Handler(uint8_t index, bool target_on)
{
    if (index >= 4) return;

    if (target_on) 
    {
        // 1. 硬件：拉合继电器
        Relay_Set_ON(index);
        
        // 2. AI：强制进入采样状态，准备捕捉这 2 秒的电流
        Socket_AI_Ctrl_t *p_ai = &g_ai_ctrl[index];
        p_ai->state = AI_SAMPLING;
        p_ai->start_tick = HAL_GetTick();
        p_ai->i_max = 0;
        
        // 3. 状态更新
        g_strip.sockets[index].on = true;
        strncpy(g_strip.sockets[index].device_name, "Detecting...", 15);
    }
    else 
    {
        // 硬件：断开继电器
        Relay_Set_OFF(index);
        
        // 状态更新
        g_strip.sockets[index].on = false;
        strncpy(g_strip.sockets[index].device_name, "None", 15);
    }
}
/**
 * @brief  AI 学习引擎 - 捕捉特征并保存至 SD 卡
 * @param  index: 插座索引 (0-3)
 * @param  p_now: 实时有功功率 (P1)
 * @param  i_now: 实时有效电流 (C1)
 * @param  pf_now: 实时功率因数 (P3)
 */
void AI_Learning_Engine(uint8_t index, float p_now,float v_now ,float i_now, float pf_now)
{
    if (index >= 4) return;
    Socket_AI_Ctrl_t *p_ai = &g_ai_ctrl[index];

    switch (p_ai->state)
    {
        case AI_IDLE:
           
            break;

        case AI_SAMPLING:
            // 捕捉采样窗口内的电流极值 (Inrush Current)
            if (i_now > p_ai->i_max) p_ai->i_max = i_now;

            // 2秒采样窗口，捕捉电器从“暂态”进入“稳态”的过程
            if (HAL_GetTick() - p_ai->start_tick > 2000)
            {
                p_ai->state = AI_READY;
            }
            break;

        case AI_READY:
            printf("[AI] 插座[%d] 特征提取完成！\r\n", index);
            printf(" >> 稳态功率: %.2fW, 功率因数: %.2f, 峰值电流: %.2fA\r\n", 
                    p_now, pf_now, p_ai->i_max);
		
		    // 构造一个特征结构体
			Appliance_Data_t new_feat;
		
            char base_name[12];
			char final_name[20];
			int retry_count = 0;

			// 1. 定义基础名字 (根据插座索引)
			sprintf(base_name, "Socket%d", index);
			strcpy(final_name, base_name);

			// 2. 【核心逻辑】自动序列化检测
			// 只要名字存在，就在后面加序号重试，直到找到唯一名
			while (Check_Device_Exist(final_name) == 1) 
			{
				retry_count++;
				sprintf(final_name, "%s_%d", base_name, retry_count);
				if(retry_count > 99) break; // 安全保护，防止死循环
			}
			
			new_feat.id = (uint16_t)(HAL_GetTick() & 0xFFFF);
			strncpy(new_feat.name, final_name, 19);
			new_feat.power = p_now;
			new_feat.pf = pf_now;
			new_feat.i_surge_ratio = p_ai->i_max / (i_now + 0.001f); // 实时计算激增比
			new_feat.q_reactive = sqrtf(fabsf((v_now*i_now)*(v_now*i_now) - p_now*p_now)); // 计算无功
            
		
		    printf("[Learning] 提取完成，正在入库: %s\r\n", new_feat.name);
			// 4. 保存到 SD 卡
			if (Save_Appliance_Data(&new_feat) == FR_OK) {
				printf("[AI-Learn] 录入成功! 设备名: %s, 稳态功率: %.1fW\r\n", final_name, p_now);
			}

            p_ai->state = AI_IDLE; 
            break;
    }

}

/**
 * @brief  AI 识别引擎 - 捕捉特征并在 SD 卡中比对识别
 * @param  index: 插座索引 (0-3)
 * @param  p_now: 实时有功功率 (P1)
 * @param  i_now: 实时有效电流 (C1)
 * @param  pf_now: 实时功率因数 (P3)
 */
void AI_Recognition_Engine(uint8_t index, float p_now,float v_now ,float i_now, float pf_now)
{
    if (index >= 4) return;
    Socket_AI_Ctrl_t *p_ai = &g_ai_ctrl[index];

    switch (p_ai->state)
    {
        case AI_IDLE:
           
            break;

        case AI_SAMPLING:
            // 捕捉采样窗口内的电流极值 (Inrush Current)
            if (i_now > p_ai->i_max) p_ai->i_max = i_now;

            // 2秒采样窗口，捕捉电器从“暂态”进入“稳态”的过程
            if (HAL_GetTick() - p_ai->start_tick > 2000)
            {
                p_ai->state = AI_READY;
            }
            break;

        case AI_READY:
            // 调用之前写的五维度匹配算法
			char* match_name = Identify_Appliance_In_SD(p_now, pf_now, p_ai->i_max, v_now, i_now);
			
			// 将识别出的名称写入全局结构体，供 MQTT 上报使用
			strncpy(g_strip.sockets[index].device_name, match_name, 15);
			
			printf("[Recognition] 插座[%d] 识别成功: %s\r\n", index, match_name);

            p_ai->state = AI_IDLE; 
            break;
    }

}


/**
 * @brief  AI 负载识别算法（基于更新后的五维度结构体）
 * @param  p_now: 实时有功功率 (P1)
 * @param  pf_now: 实时功率因数 (P3)
 * @param  i_max: 采样周期内的峰值电流 (C1_Max)
 * @param  v_now: 实时电压 (V1)
 * @param  i_steady: 采样结束时的稳态电流 (C1)
 * @return 匹配到的设备名称 (返回静态字符串指针)
 */
char* Identify_Appliance_In_SD(float p_now, float pf_now, float i_max, float v_now, float i_steady)
{
    static FIL db_fil;
    char line[128];
    static char best_match_name[20]; 
    float min_distance = 999999.0f;
    
    // 初始化返回名称
    strcpy(best_match_name, "Unknown Device");

    // 1. 计算当前测得的派生特征值
    float s_now = v_now * i_steady;                                // 视在功率 S
    float surge_ratio_now = (i_steady > 0.05f) ? (i_max / i_steady) : 1.0f; // 激增比 (避开分母过小)
    float q_now = sqrtf(fabsf(s_now * s_now - p_now * p_now));     // 无功功率 Q

    // 2. 打开 SD 卡数据库
    if (f_open(&db_fil, DEVICE_DB_PATH, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        return "SD Error";
    }

    // 3. 逐行搜索特征库
    while (f_gets(line, sizeof(line), &db_fil))
    {
        if (strstr(line, "ID")) continue; // 跳过 CSV 表头

        Appliance_Data_t lib;
        // 严格按照：ID,Name,Power,PF,SurgeRatio,Q_Reactive 解析
        int count = sscanf(line, "%hu,%19[^,],%f,%f,%f,%f", 
                           &lib.id, lib.name, &lib.power, 
                           &lib.pf, &lib.i_surge_ratio, &lib.q_reactive);

        if (count == 6)
        {
            /* 4. 计算加权欧几里得距离 */
            // 为了让不同量级的物理量可以比较，我们进行简单的归一化(Normalization)
            
            // 维度1: 有功功率 (权重 0.4) - 以100W为基准缩放
            float d_p  = powf((p_now - lib.power) / 100.0f, 2) * 0.40f;
            
            // 维度2: 功率因数 (权重 0.25) - PF本身在0-1之间
            float d_pf = powf(pf_now - lib.pf, 2) * 0.25f;
            
            // 维度3: 激增比 (权重 0.2)
            float d_sr = powf(surge_ratio_now - lib.i_surge_ratio, 2) * 0.20f;
            
            // 维度4: 无功功率 (权重 0.1) - 以100Var为基准缩放
            float d_q  = powf((q_now - lib.q_reactive) / 100.0f, 2) * 0.10f;
            
            // 维度5: 视在功率 (权重 0.05) - 综合电压波动影响
            float s_lib = lib.power / (lib.pf + 0.001f); // 估算库中设备的视在功率
            float d_s  = powf((s_now - s_lib) / 100.0f, 2) * 0.05f;

            // 计算总距离
            float total_distance = sqrtf(d_p + d_pf + d_sr + d_q + d_s);

            // 5. 寻找最优解
            if (total_distance < min_distance) {
                min_distance = total_distance;
                strncpy(best_match_name, lib.name, sizeof(best_match_name) - 1);
            }
        }
    }
    f_close(&db_fil);

    // 6. 判定门限：如果最小距离还是很大，说明库里没这玩意
    // 0.5f 是一个经验值，距离越小匹配度越高
    if (min_distance > 0.6f) { 
        return "Unknown Device";
    }

    return best_match_name;
}

