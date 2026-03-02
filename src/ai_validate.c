#include "ai_validate.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define DEVICE_DB_PATH "1:Device.csv"

typedef enum
{
    LAB_UNKNOWN = 0,
    LAB_EGG,
    LAB_STEAM,
    LAB_FAN
} Label_t;

static const char * label_to_str(Label_t l)
{
    switch (l)
    {
        case LAB_EGG:   return "EGG";
        case LAB_STEAM: return "STEAM";
        case LAB_FAN:   return "FAN";
        default:        return "UNK";
    }
}

static Label_t str_to_label(const char *s)
{
    if (0 == strcmp(s, "EGG"))   return LAB_EGG;
    if (0 == strcmp(s, "STEAM")) return LAB_STEAM;
    if (0 == strcmp(s, "FAN"))   return LAB_FAN;
    return LAB_UNKNOWN;
}

// B格式：Label,Name,Power,PF,Startup,Weight
typedef struct
{
    Label_t label;     // EGG/STEAM/FAN
    char name[20];     // Socket0_x
    float power;       // W
    float pf;          // 0~1
    float startup;     // 你文件里是 Startup
    float weight;      // 你文件里是 Weight
} Row_t;

static int parse_row_B(const char * line, Row_t * out)
{
    char lab[8] = {0};
    int count = sscanf(line, "%7[^,],%19[^,],%f,%f,%f,%f",
                       lab, out->name, &out->power, &out->pf, &out->startup, &out->weight);
    if (count != 6) return -1;

    out->label = str_to_label(lab);
    return (out->label == LAB_UNKNOWN) ? -1 : 0;
}

/**
 * @brief 2维距离（P+PF），当前阶段最稳（不依赖电流链路）
 */
static float dist_2d(const Row_t * a, const Row_t * b)
{
    float dp  = (a->power - b->power) / 100.0f;
    float dpf = (a->pf - b->pf);
    float s = dp * dp * 0.85f + dpf * dpf * 0.15f;
    return sqrtf(s);
}

void Run_LOO_Test_From_DeviceCSV(void)
{
    static FIL fil;
    static char line[128];
    static Row_t rows[128];

    int n = 0;

    FRESULT res = f_open(&fil, DEVICE_DB_PATH, FA_READ | FA_OPEN_EXISTING);
    printf("[LOO] open res=%d\r\n", res);
    if (res != FR_OK)
    {
        printf("[LOO] open %s failed\r\n", DEVICE_DB_PATH);
        return;
    }

    // 读表头并打印确认
    if (f_gets(line, sizeof(line), &fil))
    {
        printf("[LOO] header=%s\r\n", line);
    }

    while (f_gets(line, sizeof(line), &fil))
    {
        if (n >= (int)(sizeof(rows)/sizeof(rows[0]))) break;

        if (parse_row_B(line, &rows[n]) == 0)
        {
            n++;
        }
        else
        {
            // 若需要排查格式问题，打开这行
            // printf("[LOO] parse fail: %s\r\n", line);
        }
    }
    f_close(&fil);

    if (n < 6)
    {
        printf("[LOO] not enough samples: %d\r\n", n);
        return;
    }

    printf("\r\n================ LOO VALIDATION (B-format, P+PF 2D) ================\r\n");
    printf("Samples loaded: %d\r\n", n);
    printf("Print: true/pred d1 d2 margin (name)\r\n");
    printf("---------------------------------------------------------------------\r\n");

    int correct = 0;
    int total = 0;

    int tot_egg = 0, ok_egg = 0;
    int tot_steam = 0, ok_steam = 0;
    int tot_fan = 0, ok_fan = 0;

    float correct_max_d1 = 0.0f;
    float wrong_min_d1   = 999.0f;

    for (int i = 0; i < n; i++)
    {
        Label_t true_lab = rows[i].label;

        float best = 999.0f, second = 999.0f;
        int best_j = -1;

        for (int j = 0; j < n; j++)
        {
            if (j == i) continue;

            float d = dist_2d(&rows[i], &rows[j]);
            if (d < best)
            {
                second = best;
                best = d;
                best_j = j;
            }
            else if (d < second)
            {
                second = d;
            }
        }

        Label_t pred = (best_j >= 0) ? rows[best_j].label : LAB_UNKNOWN;
        float margin = second - best;

        total++;
        int is_ok = (pred == true_lab);
        if (is_ok)
        {
            correct++;
            if (best > correct_max_d1) correct_max_d1 = best;
        }
        else
        {
            if (best < wrong_min_d1) wrong_min_d1 = best;
        }

        if (true_lab == LAB_EGG)   { tot_egg++;   if (is_ok) ok_egg++; }
        if (true_lab == LAB_STEAM) { tot_steam++; if (is_ok) ok_steam++; }
        if (true_lab == LAB_FAN)   { tot_fan++;   if (is_ok) ok_fan++; }

        printf("true=%-5s pred=%-5s d1=%.3f d2=%.3f margin=%.3f  (%s)\r\n",
               label_to_str(true_lab),
               label_to_str(pred),
               best, second, margin,
               rows[i].name);
    }

    float acc = (total > 0) ? (100.0f * (float)correct / (float)total) : 0.0f;

    printf("---------------------------------------------------------------------\r\n");
    printf("[LOO] ACC = %d/%d = %.1f%%\r\n", correct, total, acc);
    if (tot_egg)   printf("[LOO] EGG   = %d/%d = %.1f%%\r\n", ok_egg,   tot_egg,   100.0f*ok_egg/tot_egg);
    if (tot_steam) printf("[LOO] STEAM = %d/%d = %.1f%%\r\n", ok_steam, tot_steam, 100.0f*ok_steam/tot_steam);
    if (tot_fan)   printf("[LOO] FAN   = %d/%d = %.1f%%\r\n", ok_fan,   tot_fan,   100.0f*ok_fan/tot_fan);

    printf("[LOO] correct_max_d1 = %.3f\r\n", correct_max_d1);
    printf("[LOO] wrong_min_d1   = %.3f\r\n", wrong_min_d1);
    printf("[LOO] suggestion: THRESH between them if separated; add margin gate if needed\r\n");
    printf("=====================================================================\r\n\r\n");
}