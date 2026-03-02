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



void AI_Trigger_Sampling(uint8_t index)
{
    if (index >= 4) return;
    Socket_AI_Ctrl_t *p_ai = &g_ai_ctrl[index];

    // 只在空闲时触发，防止重复触发干扰一次识别
    if (p_ai->state == AI_IDLE)
    {
        p_ai->state = AI_SAMPLING;
        p_ai->start_tick = HAL_GetTick();
        p_ai->i_max = 0.0f;
        strncpy(g_strip.sockets[index].device_name, "Detecting...", 15);
    }
}

void AI_Reset(uint8_t index)
{
    if (index >= 4) return;
    Socket_AI_Ctrl_t *p_ai = &g_ai_ctrl[index];
    p_ai->state = AI_IDLE;
    p_ai->i_max = 0.0f;
    p_ai->start_tick = 0;
}


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
        
        // 3. 状态更新
        g_strip.sockets[index].on = true;
        strncpy(g_strip.sockets[index].device_name, "Idle", 15); // 或者 "On"
		AI_Reset(index);
    }
    else 
    {
        // 硬件：断开继电器
        Relay_Set_OFF(index);
        
        // 状态更新
        g_strip.sockets[index].on = false;
        strncpy(g_strip.sockets[index].device_name, "None", 15);
		AI_Reset(index);
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
            if (HAL_GetTick() - p_ai->start_tick > 8000)
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
            if (HAL_GetTick() - p_ai->start_tick > 8000)
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

            p_ai->state = AI_LOCKED; 
		    
		    break;
		
		case AI_LOCKED:
			 // 什么都不做，等 OFF 时由外部逻辑 Reset
	
            break;
    }

}



char* Identify_Appliance_In_SD(float p_now, float pf_now, float i_max, float v_now, float i_steady)
{
    // 显式忽略暂时不用的 2D 参数，防止编译器产生警告
    (void)i_max; (void)v_now; (void)i_steady; 

    static FIL fil;
    static char best_name[20]; // 加上 static，确保函数返回后字符串内存依然有效
    char line[160];

    // 初始化：将最小距离（best）和第二小距离（second）设为极大值
    float best = 999999.0f;
    float second = 999999.0f;

    // 默认返回“未知设备”
    strcpy(best_name, "Unknown Device");

    /* --- 1. 打开数据库 --- */
    FRESULT res = f_open(&fil, DEVICE_DB_PATH, FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK)
    {
        printf("[REC] open failed path=%s res=%d\r\n", DEVICE_DB_PATH, res);
        return "SD Error";
    }

    // 先读一行（跳过表头），若读取失败则说明是空文件
    if (!f_gets(line, sizeof(line), &fil))
    {
        f_close(&fil);
        printf("[REC] empty file\r\n");
        return "Unknown Device";
    }

    int parsed_ok = 0;
    int parsed_bad = 0;

    /* --- 2. 遍历所有样本进行比对 --- */
    while (f_gets(line, sizeof(line), &fil))
    {
        // 过滤空行和重复出现的表头
        if (line[0] == '\0' || line[0] == '\r' || line[0] == '\n') continue;
        if (strstr(line, "ID,Name,Power") != NULL) continue;

        unsigned long id;
        char name[20] = {0};
        float p_lib, pf_lib, sr, q;

        // 尝试解析 6 个维度的数据
        int cnt = sscanf(line, "%lu,%19[^,],%f,%f,%f,%f",
                         &id, name, &p_lib, &pf_lib, &sr, &q);

        if (cnt != 6) // 如果这一行格式不对，记录并报错
        {
            parsed_bad++;
            if (parsed_bad <= 3) // 仅打印前3条，防止串口日志刷屏
            {
                printf("[REC] parse fail line: %s\r\n", line);
            }
            continue;
        }

        parsed_ok++;

        // 清理字符串：去掉末尾可能存在的 Windows 回车符
        size_t len = strlen(name);
        if (len && name[len - 1] == '\r') name[len - 1] = '\0';

        /* --- 3. 核心算法：计算加权欧式距离 --- */
        // 归一化处理：功率(P)差值除以 100，使其与功率因数(PF, 0-1之间)在数量级上可比
        float dp  = (p_now - p_lib) / 100.0f;
        float dpf = (pf_now - pf_lib);
        
        // 计算 2D 距离公式：
        // $d = \sqrt{\Delta P^2 \times 0.85 + \Delta PF^2 \times 0.15}$
        // 这里的 0.85 和 0.15 是权重，说明该算法更看重功率的准确性
        float d = sqrtf(dp * dp * 0.85f + dpf * dpf * 0.15f);

        // 维护一个“最近”和“次近”的记录（用于后续可能的置信度判断）
        if (d < best)
        {
            second = best;
            best = d;
            // 找到了更像的设备，拷贝其名称
            strncpy(best_name, name, sizeof(best_name) - 1);
            best_name[sizeof(best_name) - 1] = '\0';
        }
        else if (d < second)
        {
            second = d;
        }
    }

    f_close(&fil); // 关闭文件

    // 计算最小距离与次小距离的差值（边际误差）
    float margin = second - best;

    // 打印详细识别过程，方便现场调试
    printf("[REC] p=%.2f pf=%.2f parsed_ok=%d bad=%d d1=%.3f d2=%.3f margin=%.3f best=%s\r\n",
           p_now, pf_now, parsed_ok, parsed_bad, best, second, margin, best_name);

    if (parsed_ok == 0)
    {
        return "Unknown Device";
    }

    /* --- 4. 最终裁定 --- */
    // 识别门限：如果最小距离 d1 依然大于 0.12，说明数据库里最像的电器其实也不怎么像
    const float THRESH_D1 = 0.12f;

    if (best > THRESH_D1)
    {
        // 距离太远，宁愿说不知道，也不要乱猜
        return "Unknown Device";
    }

    return best_name; // 成功匹配！
}