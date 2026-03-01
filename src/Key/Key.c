#include "Key.h"

/* 按键按下标志 */
volatile bool key1_sw2_press = false;
volatile bool key2_sw3_press = false;

void Key_IRQ_Init(void)
{
   fsp_err_t err = FSP_SUCCESS;

   /* Open ICU module */
   err = R_ICU_ExternalIrqOpen(&g_external_irq6_ctrl, &g_external_irq6_cfg);
   err = R_ICU_ExternalIrqOpen(&g_external_irq7_ctrl, &g_external_irq7_cfg);
   /* 允许中断 */
   err = R_ICU_ExternalIrqEnable(&g_external_irq6_ctrl);
   err = R_ICU_ExternalIrqEnable(&g_external_irq7_ctrl);
}


/* 按键中断回调函数 */
void key_externel_irq_callback(external_irq_callback_args_t *p_args)
{
   /* 判断中断通道 */
   if (6 == p_args->channel)
   {
      key1_sw2_press = true;   // 按键KEY1_SW2按下
   }
   else if (7 == p_args->channel)
   {
      key2_sw3_press = true;   // 按键KEY2_SW3按下
   }
}
//}

