#include "uart_hlw.h"
#include "stdbool.h"
#include "bsp_debug_uart.h"
#include "circle_buf.h"
#include "cJSON_handle.h"
#include "stdlib.h"
#include <time.h>   // 用于 srand()
#include "appliance_identification.h"
#include "event_detector.h"
#include "Systick.h"
static EventDetector_t g_evt_det[4];
static bool g_evt_inited = false;
#define MAX_C 20
#define LOW_POWER_THRESHOLD_W 6.0f
#define MAX_VALID_POWER_W 5000.0f
static uint32_t g_lowpower_log_tick[4] = {0};


//BL0942波特率为9600

// 定义模式
#define MODE_RECOGNITION 0
#define MODE_LEARNING    1

uint8_t g_ai_mode = MODE_RECOGNITION; // 默认是识别模式


static volatile int g_uart3_tx_complete = 0;
extern circle_buf_t g_BL0942_rx_buf;
uint8_t com_data[2]={0x58,0xAA};//读取BL0942模块数据的一个指令
volatile bool g_uart3_rx_end = 0;
extern PowerStrip_t g_strip;
void GPTDrvInit_elc(void)
{
    fsp_err_t err = g_timer0.p_api->open(g_timer0.p_ctrl, g_timer0.p_cfg);
    if (FSP_SUCCESS != err)
        printf("timer0 open failed\r\n");
    err = g_timer0.p_api->enable(g_timer0.p_ctrl);
    if (FSP_SUCCESS != err)
        printf("timer0 enable failed\r\n");
}
void ELCDrvInit(void)
{
    fsp_err_t err = g_elc.p_api->open(g_elc.p_ctrl, g_elc.p_cfg);
    if (FSP_SUCCESS != err)
    {
        printf("Elc open failed\r\n");
        return;
    }
    err = g_elc.p_api->enable(g_elc.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        printf("Elc enable failed\r\n");
        return;
    }
}

/* 电能计量模块 UART3 初始化 */
void BL0942_UART3_Init(void)
{
    fsp_err_t err;
    
    // 配置串口
    err = g_uart3.p_api->open(g_uart3.p_ctrl, g_uart3.p_cfg);
	if (FSP_SUCCESS != err)
        printf("uart_hlw open failed\r\n");
	else
    {
		printf("uart_hlw open success\r\n");
	
	}
}


void gpt_uart_callback(timer_callback_args_t *p_args)
{
    if (p_args->event == TIMER_EVENT_CYCLE_END)
    {
        g_uart3_rx_end = 1;
    }
}
void uart3_callback(uart_callback_args_t *p_args)
{
    switch (p_args->event)
    {
    case UART_EVENT_TX_COMPLETE:
    {
        g_uart3_tx_complete = 1;
        break;
    }
    case UART_EVENT_RX_CHAR:
    {
        if (g_uart3_rx_end)
            return;
        // 修改为将字符送入环形缓冲区
        g_BL0942_rx_buf.put(&g_BL0942_rx_buf, (uint8_t)p_args->data);
        break;
    }
    default:
        break;
    }
}


void Send_com(void)
{
	g_uart3.p_api->write(g_uart3.p_ctrl,com_data,2);
}
/*
数据解析函数
*/
void Data_Processing(unsigned char *data,uint8_t index)
{
	if (index >= 4) return;
	uint32_t now = HAL_GetTick();
	// ... 此处保留你原有的校验和 24 位转 32 位计算逻辑 ...
    // 假设计算出的局部变量为：V_val, C_val, P_val, E_val
	uint8_t i=0,check_num=0;
	uint32_t count=88;
	uint32_t V_REG=0,PF_COUNT=0;
	int32_t C_REG=0,P_REG=0;
	double V1=0,C1=0,P1=0,P2=0,P3=0,E_con=0;
	
//	printf("\r\nRaW_data:");
	for(i=0;i<22;i++)//求和，用来计算校验码
	{
		count=count+data[i];
//		printf("%02X ",data[i]);
	}
//	printf("%02X\r\n",data[22]);
	check_num=~(count&0xFF);//取最后一个字节，然后按位取反
	//printf("\r\nChecknum=%02X;Data_num=%02X\r\n",check_num,USART2_RX_BUF[22]);
	
	
	if(check_num==data[22])//校验数据是正确
	{
		//printf("Check_OK\r\n");
		C_REG=data[3]*65536+data[2]*256+data[1];//计算电流寄存器
		if(data[3]&0x80)//高字节的最高位如果为1，说明电流为负数
		{
			C_REG=-(16777216-C_REG);
		}
		if(MAX_C==10)//电流最大值模块类型，你购买的是10A还是20A模块
		{
			C1=C_REG*1.218/(305978*3);//计算有效电流
		}
		else
		{
			C1=C_REG*1.218/(305978);//计算有效电流
		}
		
		V_REG=data[6]*65536+data[5]*256+data[4];//计算电压寄存器
		V1=V_REG*1.218*1950.51/37734390;//计算有效电压
		
		P2=V1*C1;//P2为视在功率，视在功率=有效电压*有效电流
		
		P_REG=data[12]*65536+data[11]*256+data[10];//计算有功功率寄存器
		if(data[12]&0x80)//高字节的最高位如果为1，说明有功功率为负数
		{
			P_REG=-(16777216-P_REG);
		}
		if(MAX_C==10)//电流最大值模块类型，你购买的是10A还是20A模块
		{
			P1=P_REG*1.218*1.218*1950.51/5411610;//计算有功功率/
		}
		else
		{
			P1=P_REG*1.218*1.218*1950.51/1803870;//计算有功功率
		}
		if(P2!=0)
			P3=P1/P2;//计算功率因数；功率因数=有功功率/视在功率
		
		PF_COUNT=data[15]*65536+data[14]*256+data[13];//计算已用电量脉冲数
		if(MAX_C==10)//电流最大值模块类型，你购买的是10A还是20A模块
		{
			E_con=PF_COUNT/16051.896;//计算已用电量，16051.896为固定值，和选购的量程有关
		}
		else
		{
			E_con=PF_COUNT/5350.632;//计算已用电量，5350.632为固定值，和选购的量程有关
		}
//		printf("C=%0.3fA;V=%0.2fV;P1=%0.2fW;P2=%0.2fW;P3=%0.1f;E_con=%0.4fkWh\r\n",C1,V1,P1,P2,P3,E_con);
		
		
	}
	else
	{
		printf("Check Error\r\n");//校验数据有误，校验数据出错
	}
	
	if(check_num == data[22]) 
    {
        float p_used = (P1 >= 0.0) ? (float)P1 : (float)(-P1);

        /* 过滤明显异常功率，避免污染触发和识别。 */
        if (p_used > MAX_VALID_POWER_W)
        {
            printf("[PWR] drop abnormal power: %.2fW\r\n", p_used);
            AI_Reset(index);
            EventDetector_Init(&g_evt_det[index]);
            return;
        }

        // 1. 更新该路功率
        g_strip.sockets[index].power = p_used; 
        
        // 2. 更新全局电压 (因为4路电压物理上是一致的，取最后一次解析的结果即可)
        g_strip.voltage = (float)V1;

        // 3. 重新计算总和 (关键：每次解析完一路，都要同步更新总物理量)
        float temp_p_sum = 0;
        float temp_c_sum = 0;
        for(int i=0; i<4; i++) {
            temp_p_sum += g_strip.sockets[i].power;
            // 假设 BL0942 测的是该分路的电流，总电流即为求和
            // 注意：这里需要考虑测量误差，建议做个微小的消噪处理
            temp_c_sum += (g_strip.sockets[i].power / g_strip.voltage); 
        }
        g_strip.total_power = temp_p_sum;
        g_strip.total_current= temp_c_sum;

        /* 小功率统一归类，不触发学习和识别。 */
        if (g_strip.sockets[index].on &&
            g_strip.sockets[index].power < LOW_POWER_THRESHOLD_W)
        {
            uint32_t now_tick = HAL_GetTick();
            if (now_tick - g_lowpower_log_tick[index] >= 1000U)
            {
                g_lowpower_log_tick[index] = now_tick;
                printf("[LP] socket=%d power=%.2fW (skip AI)\r\n", index, g_strip.sockets[index].power);
            }
            strncpy(g_strip.sockets[index].device_name, "LowPower", 15);
            AI_Reset(index);
            EventDetector_Init(&g_evt_det[index]);
            return;
        }
		
		// ② 事件检测：OFF->ON 触发 AI 采样（不动继电器）
    bool need_trigger = EventDetector_Update(&g_evt_det[index],
                                             g_strip.sockets[index].on,
                                             p_used,
                                             HAL_GetTick());
    if (need_trigger)
    {
        AI_Trigger_Sampling(index);
    }

    // ③ 再推进 AI 状态机（这样触发后的同一帧就能计 i_max）
    if (g_ai_mode == MODE_LEARNING) {
        AI_Learning_Engine(index, p_used, (float)V1, (float)C1, (float)P3);
    } else {
        AI_Recognition_Engine(index, p_used, (float)V1, (float)C1, (float)P3);
    }
	
	// ================== 【新增】关断检测与AI复位 ==================
	if (g_strip.sockets[index].on && g_evt_det[index].state == EVT_STATE_OFF)
	{
		// 低功率稳定OFF时，把显示回到 None
		if (g_strip.sockets[index].power < 1.0f&& g_evt_det[index].off_cnt >= 3)
		{
			strncpy(g_strip.sockets[index].device_name, "None", 15);
			AI_Reset(index);
		}
	}
		
		
    }
}


void uart_hlw_init(void)
{
    ELCDrvInit();
    GPTDrvInit_elc();
    BL0942_UART3_Init();
	
	 if (!g_evt_inited)
    {
        for (int i = 0; i < 4; i++) EventDetector_Init(&g_evt_det[i]);
        g_evt_inited = true;
    }
   
}

void Simulation_Random_Load_Test(void)
{
    // 1. 模拟市电电压波动 (218V ~ 222V 之间)
    // rand() % 40 -> 0~39, 再除以 10.0 -> 0.0~3.9
    g_strip.voltage = 218.0f + (float)(rand() % 41) / 10.0f;

    // 2. 定义四种电器的基准功率和波动范围
    float base_p[4]  = {150.0f, 15.0f, 2.0f, 40.0f}; // 基准功率 (W)
    float range_p[4] = {20.0f,  2.0f,  0.5f, 5.0f};  // 波动范围 (W)

    float power_sum = 0;

    for(int i = 0; i < 4; i++) 
    {
        // 只有在开关打开时才有功率
        if(g_strip.sockets[i].on) 
        {
            // 计算随机波动：基准 + (随机偏移 - 范围的一半)
            // 例如 PC: 150 + (0~20 - 10) -> 140W ~ 160W
            float noise = (float)(rand() % (int)(range_p[i] * 10)) / 10.0f;
            g_strip.sockets[i].power = base_p[i] + (noise - (range_p[i] / 2.0f));
            
            power_sum += g_strip.sockets[i].power;
        } 
        else 
        {
            g_strip.sockets[i].power = 0.0f;
        }
    }

    // 3. 计算物理总和
    g_strip.total_power = power_sum;
    // I = P / U
    g_strip.total_current = g_strip.total_power / g_strip.voltage;

   
}

//void uart_hlw_test(void)
//{
//	int time=0;
//	Key_IRQ_Init();
//    ELCDrvInit();
//    GPTDrvInit_elc();
//    fsp_err_t err;
//    g_uart0.p_api->open(g_uart0.p_ctrl, g_uart0.p_cfg);
//    circlebuf_init();
//    // 配置串口
//    err = g_uart3.p_api->open(g_uart3.p_ctrl, g_uart3.p_cfg);
//	printf("hello\r\n");
//    if (FSP_SUCCESS != err)
//        printf("uart_hlw open failed\r\n");
//		//Send_com();
//		
//    while (1)
//    {

//        if (g_uart3_rx_end == 1)
//        {
//            g_uart3_rx_end = 0;
//            
//            Data_Processing(g_rx_buf.buffer);
//            circlebuf_clear();
//           
//        }
//			if (key1_sw2_press)
//      {
//            key1_sw2_press = false; //标志位清零
//						sw_ctrl(true);
//						printf("key0 is pressed\r\n");
//						g_ioport.p_api->pinWrite(g_ioport.p_ctrl,BSP_IO_PORT_01_PIN_02,BSP_IO_LEVEL_LOW);
//      }

//      /* 判断按键 KEY2_SW3 是否被按下 */
//      if (key2_sw3_press)
//      {
//            key2_sw3_press = false; //标志位清零
//						sw_ctrl(false);
//						printf("key1 is pressed\r\n");
//						g_ioport.p_api->pinWrite(g_ioport.p_ctrl,BSP_IO_PORT_01_PIN_02,BSP_IO_LEVEL_HIGH);
//      }
//				Send_com();
//				R_BSP_SoftwareDelay(500, BSP_DELAY_UNITS_MILLISECONDS);

//				
//    }
//}
