#include "Key.h"
#include "stdio.h"

key_struct key[4] = {0};
int num = 0;

/* 10ms 定时中断下，2 个采样点去抖 = 20ms */
#define KEY_COUNT          4U
#define KEY_DEBOUNCE_TICKS 2U

/* 上拉输入：松开=1，按下=0 */
static const bsp_io_port_pin_t g_key_pins[KEY_COUNT] =
{
    BSP_IO_PORT_00_PIN_07, /* KEY1 */
    BSP_IO_PORT_00_PIN_06, /* KEY2 */
    BSP_IO_PORT_00_PIN_05, /* KEY3 */
    BSP_IO_PORT_00_PIN_04  /* KEY4 */
};

/* 每路按键单击事件标志，由定时器中断置位，主循环读取后清零 */
static volatile uint8_t g_key_single_event[KEY_COUNT] = {0};

void key_timHandler_callback(void)
{
    int i;

    for (i = 0; i < (int)KEY_COUNT; i++)
    {
        g_ioport.p_api->pinRead(g_ioport.p_ctrl, g_key_pins[i], &key[i].key_state);

        switch (key[i].judge_state)
        {
            case 0: /* RELEASED_STABLE */
                if (key[i].key_state == BSP_IO_LEVEL_LOW)
                {
                    key[i].judge_state = 1; /* PRESS_DEBOUNCE */
                    key[i].key_time = 0;
                }
                break;

            case 1: /* PRESS_DEBOUNCE */
                if (key[i].key_state == BSP_IO_LEVEL_LOW)
                {
                    key[i].key_time++;
                    if (key[i].key_time >= KEY_DEBOUNCE_TICKS)
                    {
                        key[i].judge_state = 2; /* PRESSED_STABLE */
                        g_key_single_event[i] = 1;
                    }
                }
                else
                {
                    key[i].judge_state = 0;
                }
                break;

            case 2: /* PRESSED_STABLE */
                if (key[i].key_state == BSP_IO_LEVEL_HIGH)
                {
                    key[i].judge_state = 3; /* RELEASE_DEBOUNCE */
                    key[i].key_time = 0;
                }
                break;

            case 3: /* RELEASE_DEBOUNCE */
                if (key[i].key_state == BSP_IO_LEVEL_HIGH)
                {
                    key[i].key_time++;
                    if (key[i].key_time >= KEY_DEBOUNCE_TICKS)
                    {
                        key[i].judge_state = 0;
                    }
                }
                else
                {
                    key[i].judge_state = 2;
                }
                break;

            default:
                key[i].judge_state = 0;
                key[i].key_time = 0;
                break;
        }
    }
}

int key_control(void)
{
    num = 0;

    if (g_key_single_event[0] == 1U)
    {
        num |= (1 << 0);
        g_key_single_event[0] = 0;
    }

    if (g_key_single_event[1] == 1U)
    {
        num |= (1 << 1);
        g_key_single_event[1] = 0;
    }

    if (g_key_single_event[2] == 1U)
    {
        num |= (1 << 2);
        g_key_single_event[2] = 0;
    }

    if (g_key_single_event[3] == 1U)
    {
        num |= (1 << 3);
        g_key_single_event[3] = 0;
    }

    return num;
}

void Key_GPT_Init(void)
{
    fsp_err_t err = g_timer1.p_api->open(g_timer1.p_ctrl, g_timer1.p_cfg);
    if (FSP_SUCCESS != err)
    {
        printf("timer1 open failed\r\n");
    }

    err = g_timer1.p_api->start(g_timer1.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        printf("timer1 start failed\r\n");
    }
}

/* 定时器溢出中断回调（10ms 周期） */
void gpt_timer1_callback(timer_callback_args_t *p_args)
{
    switch (p_args->event)
    {
        case TIMER_EVENT_CYCLE_END:
            key_timHandler_callback();
            break;

        default:
            break;
    }
}
