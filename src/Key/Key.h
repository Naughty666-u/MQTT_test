#ifndef __KEY_H
#define __KEY_H
#include "hal_data.h"

void key_timHandler_callback(void);
/* 返回按键事件位图：bit0~bit3 对应 KEY1~KEY4 单击事件 */
int key_control(void);
void Key_GPT_Init(void);
typedef struct key_body
  {
    unsigned char judge_state;
    bsp_io_level_t key_state;
    bsp_io_level_t single_flag;
    bsp_io_level_t long_flag;
    bsp_io_level_t double_flag;
    bsp_io_level_t double_EN;
    uint32_t key_time;
    uint32_t key_doubletime;

  } key_struct;
  
  extern key_struct key[4];
  



extern uint8_t task_mode;
extern int num;

#endif
