#include "appliance_identification.h"
#include "Systick.h"
#include "stdio.h"
#include "cJSON_handle.h"
#include <math.h>
#include <string.h>
#include "ff.h"
#include "sdcard_data_handle.h"
#include "Relay.h"

/* 系统支持的插孔路数。 */
#define AI_SOCKET_NUM 4
/* 单次暂态采样窗口时长（ms）。 */
#define AI_SAMPLING_MS 8000U
/* 软件插拔时 OFF 保持时间（ms）。 */
#define AI_REPLUG_OFF_MS 500U
/* 软件插拔重新上电后的稳定等待时间（ms）。 */
#define AI_REPLUG_ON_SETTLE_MS 300U
/* 未识别设备统一显示名。 */
#define UNKNOWN_NAME "Unknown"

/* 识别失败时缓存到内存的影子样本。 */
typedef struct {
    /* true 表示该插孔存在一个待 LEARN_COMMIT 的样本。 */
    bool valid;
    /* 上报云端的令牌，用于把 name 绑定到正确样本。 */
    uint32_t pending_id;
    /* 样本特征快照，只有用户确认命名后才会写入 SD。 */
    Appliance_Data_t feat;
    /* 可选调试时间戳，记录样本捕获时刻。 */
    uint32_t captured_tick;
} Socket_Pending_t;

/* 每个插孔独立维护一个 AI 运行状态。 */
static Socket_AI_Ctrl_t g_ai_ctrl[AI_SOCKET_NUM] = {0};
/* 每个插孔独立维护一个 pending 样本槽。 */
static Socket_Pending_t g_pending[AI_SOCKET_NUM] = {0};
static uint32_t g_pending_seed = 1;

extern PowerStrip_t g_strip;

static bool is_unknown_name(const char *name)
{
    /*
     * 设计意图：
     * 识别结果来自多个路径（SD匹配、错误返回、历史兼容字符串），
     * 这里把“未知”统一归一，避免上层分支重复写多个 strcmp。
     *
     * 为什么这样做：
     * 1) 降低调用方心智负担：调用方只关心“是否未知”，不关心未知来源；
     * 2) 兼容旧版本字符串（如 "Unknown Device"）；
     * 3) 遇到 "SD Error" 也走未知流程，系统可继续运行而不是中断。
     */
    if (name == NULL) return true;
    return (strcmp(name, "Unknown Device") == 0) ||
           (strcmp(name, UNKNOWN_NAME) == 0) ||
           (strcmp(name, "SD Error") == 0);
}

static uint32_t next_pending_id(void)
{
    /*
     * 设计意图：
     * 为每个 Unknown 样本分配一个“事务号”（pending_id），
     * 用于网页命名提交时精确绑定到正确样本。
     *
     * 为什么这样做：
     * - 防止“晚到命令”把名字写到新样本上（错绑风险）；
     * - 0 预留为“无 pending”，便于前后端统一判空。
     */
    uint32_t id = g_pending_seed++;
    if (id == 0)
    {
        id = g_pending_seed++;
    }
    return id;
}

static void clear_pending(uint8_t index)
{
    if (index >= AI_SOCKET_NUM) return;

    /*
     * 设计意图：
     * 原子化清理该插孔的 pending 上下文，避免“内存状态”和“上报状态”不一致。
     *
     * 清理范围：
     * 1) 本地影子样本槽（valid/id/feat/time）；
     * 2) 上报给云端的镜像字段（pending_valid/pending_id）。
     *
     * 为什么这样做：
     * Unknown->命名流程是事务模型，事务结束（成功/取消/重置）后必须彻底出栈。
     */
    g_pending[index].valid = false;
    g_pending[index].pending_id = 0;
    memset(&g_pending[index].feat, 0, sizeof(g_pending[index].feat));
    g_pending[index].captured_tick = 0;

    g_strip.sockets[index].pending_valid = false;
    g_strip.sockets[index].pending_id = 0;
}

static void store_pending(uint8_t index, const Appliance_Data_t *feat)
{
    if (index >= AI_SOCKET_NUM || feat == NULL) return;

    /*
     * 设计意图：
     * 识别未命中时不立即写库，而是先进入“影子学习”缓存。
     *
     * 为什么这样做：
     * 1) 防污染：误触发/噪声样本不会直接污染 Device.csv；
     * 2) 可追踪：通过 pending_id 把网页命名动作与本次样本绑定；
     * 3) 体验自然：界面显示 Unknown，并提供“命名并加入设备库”入口。
     */
    g_pending[index].valid = true;
    g_pending[index].pending_id = next_pending_id();
    g_pending[index].feat = *feat;
    g_pending[index].captured_tick = HAL_GetTick();

    g_strip.sockets[index].pending_valid = true;
    g_strip.sockets[index].pending_id = g_pending[index].pending_id;
    strncpy(g_strip.sockets[index].device_name, UNKNOWN_NAME, sizeof(g_strip.sockets[index].device_name) - 1);
    g_strip.sockets[index].device_name[sizeof(g_strip.sockets[index].device_name) - 1] = '\0';
}

static Appliance_Data_t build_feature(uint8_t index, float p_now, float v_now, float i_now, float pf_now, float i_max)
{
    Appliance_Data_t feat;
    memset(&feat, 0, sizeof(feat));

    /*
     * 这里构造的是“本轮识别快照”，用于 pending 缓存或后续落库：
     * - name 先给临时名，LEARN_COMMIT 时再改成用户命名；
     * - i_surge_ratio / q_reactive 作为扩展特征预留给后续模型优化。
     *
     * 为什么不在这里直接写库：
     * 采样层只负责“记录事实”，是否落库由上层命名事务决定。
     */
    feat.id = (uint16_t)(HAL_GetTick() & 0xFFFFU);
    snprintf(feat.name, sizeof(feat.name), "Socket%d_tmp", index + 1);
    feat.power = p_now;
    feat.pf = pf_now;
    feat.i_surge_ratio = i_max / (i_now + 0.001f);
    feat.q_reactive = sqrtf(fabsf((v_now * i_now) * (v_now * i_now) - p_now * p_now));

    return feat;
}

void AI_Trigger_Sampling(uint8_t index)
{
    if (index >= AI_SOCKET_NUM) return;

    Socket_AI_Ctrl_t *p_ai = &g_ai_ctrl[index];
    /*
     * 设计意图：
     * 只允许在 IDLE 进入采样窗口，保证每个插孔同一时刻只有一个识别事务。
     *
     * 为什么这样做：
     * 避免重入导致 start_tick / i_max 被中途覆盖，最终特征失真。
     */
    if (p_ai->state == AI_IDLE)
    {
        p_ai->state = AI_SAMPLING;
        p_ai->start_tick = HAL_GetTick();
        p_ai->i_max = 0.0f;

        strncpy(g_strip.sockets[index].device_name, "Detecting...", sizeof(g_strip.sockets[index].device_name) - 1);
        g_strip.sockets[index].device_name[sizeof(g_strip.sockets[index].device_name) - 1] = '\0';
    }
}

void AI_Reset(uint8_t index)
{
    if (index >= AI_SOCKET_NUM) return;

    /* 复位仅影响本路状态机，不触碰继电器状态。 */
    Socket_AI_Ctrl_t *p_ai = &g_ai_ctrl[index];
    p_ai->state = AI_IDLE;
    p_ai->i_max = 0.0f;
    p_ai->start_tick = 0;
}

void Socket_Command_Handler(uint8_t index, bool target_on)
{
    if (index >= AI_SOCKET_NUM) return;

    if (target_on)
    {
        /*
         * ON路径：
         * 继电器上电后先把显示置为 Idle，并清空历史识别上下文。
         * 这样可以确保下一次事件触发时是“干净会话”。
         */
        Relay_Set_ON(index);
        g_strip.sockets[index].on = true;
        strncpy(g_strip.sockets[index].device_name, "Idle", sizeof(g_strip.sockets[index].device_name) - 1);
        g_strip.sockets[index].device_name[sizeof(g_strip.sockets[index].device_name) - 1] = '\0';
        AI_Reset(index);
        clear_pending(index);
    }
    else
    {
        /*
         * OFF路径：
         * 断电后该路设备语义上已结束，需同步清掉锁定识别结果与 pending 事务。
         */
        Relay_Set_OFF(index);
        g_strip.sockets[index].on = false;
        strncpy(g_strip.sockets[index].device_name, "None", sizeof(g_strip.sockets[index].device_name) - 1);
        g_strip.sockets[index].device_name[sizeof(g_strip.sockets[index].device_name) - 1] = '\0';
        AI_Reset(index);
        clear_pending(index);
    }
}

bool AI_Is_Pending(uint8_t index)
{
    if (index >= AI_SOCKET_NUM) return false;
    return g_pending[index].valid;
}

uint32_t AI_Get_PendingId(uint8_t index)
{
    if (index >= AI_SOCKET_NUM) return 0;
    return g_pending[index].valid ? g_pending[index].pending_id : 0;
}

FRESULT AI_Commit_Pending(uint8_t index, uint32_t pending_id, const char *name)
{
    /*
     * 事务提交函数：
     * 把 Unknown 阶段缓存的影子样本，升级为“正式设备条目”写入 SD。
     *
     * 提交成功的必要条件：
     * 1) index 合法；
     * 2) 当前确实存在 pending；
     * 3) pending_id 与缓存一致（防错绑）；
     * 4) name 合法且不超长。
     *
     * 这样做的深意：
     * 将“识别”和“学习入库”解耦，默认识别可自动运行，入库必须由用户确认。
     */

    /* 1) 基础参数校验：插孔范围 + 名称非空。 */
    if (index >= AI_SOCKET_NUM || name == NULL || name[0] == '\0') return FR_INVALID_PARAMETER;

    /* 2) pending 必须存在：没有 pending 说明当前没有可提交的 Unknown 样本。 */
    if (!g_pending[index].valid) return FR_INVALID_OBJECT;

    /* 3) pending_id 必须与缓存中的样本ID一致，防止提交错样本。 */
    if (g_pending[index].pending_id != pending_id) return FR_INVALID_OBJECT;

    /* 4) 名称长度保护：不能超过 CSV 字段容量。 */
    if (strlen(name) >= sizeof(g_pending[index].feat.name)) return FR_INVALID_NAME;

    /* 5) 将用户名称写入待提交样本。 */
    strncpy(g_pending[index].feat.name, name, sizeof(g_pending[index].feat.name) - 1);
    g_pending[index].feat.name[sizeof(g_pending[index].feat.name) - 1] = '\0';

    /* 6) 真正执行写库：写入 Device.csv。 */
    FRESULT res = Save_Appliance_Data(&g_pending[index].feat);
    if (res == FR_OK)
    {
        /*
         * 7) 写库成功后同步运行态：
         * - 设备名切换为用户命名；
         * - 清空 pending，避免重复提交同一条样本。
         */
        strncpy(g_strip.sockets[index].device_name, name, sizeof(g_strip.sockets[index].device_name) - 1);
        g_strip.sockets[index].device_name[sizeof(g_strip.sockets[index].device_name) - 1] = '\0';
        clear_pending(index);
    }

    /* 上层依据返回值回 ACK（success/fail + reason）。 */
    return res;
}

void AI_Request_Relearn_Replug(uint8_t index)
{
    if (index >= AI_SOCKET_NUM) return;

    /*
     * RELEARN_REPLUG：
     * 通过 OFF->ON 人工制造“可重复、可对齐”的上电瞬态，
     * 适合纠错或设备长期待机场景下重新学习。
     */
    clear_pending(index);
    AI_Reset(index);

    if (g_strip.sockets[index].on)
    {
        Relay_Set_OFF(index);
        R_BSP_SoftwareDelay(AI_REPLUG_OFF_MS, BSP_DELAY_UNITS_MILLISECONDS);
        Relay_Set_ON(index);
        R_BSP_SoftwareDelay(AI_REPLUG_ON_SETTLE_MS, BSP_DELAY_UNITS_MILLISECONDS);
    }
    else
    {
        Relay_Set_ON(index);
        g_strip.sockets[index].on = true;
        R_BSP_SoftwareDelay(AI_REPLUG_ON_SETTLE_MS, BSP_DELAY_UNITS_MILLISECONDS);
    }

    AI_Trigger_Sampling(index);
}

void AI_Learning_Engine(uint8_t index, float p_now, float v_now, float i_now, float pf_now)
{
    /* 兼容入口：当前学习流程与识别流程共享同一状态机。 */
    AI_Recognition_Engine(index, p_now, v_now, i_now, pf_now);
}

/* --- 5. 核心算法引擎 (主循环调用) --- */

/**
 * 识别引擎状态机：由电参采样链路每隔几十毫秒调用一次
 */
void AI_Recognition_Engine(uint8_t index, float p_now, float v_now, float i_now, float pf_now)
{
    if (index >= AI_SOCKET_NUM) return;
    Socket_AI_Ctrl_t *p_ai = &g_ai_ctrl[index];

    switch (p_ai->state)
    {
        case AI_IDLE: break; // 空闲态：什么都不做

        case AI_SAMPLING: // 采样态：这 8 秒内，疯狂寻找电流的最大值
            if (i_now > p_ai->i_max) p_ai->i_max = i_now;
            if ((HAL_GetTick() - p_ai->start_tick) > AI_SAMPLING_MS) {
                p_ai->state = AI_READY; // 8 秒到，进入对比阶段
            }
            break;

        case AI_READY: // 对比态：拿着指纹去库里找人
        {
            Appliance_Data_t feat = build_feature(index, p_now, v_now, i_now, pf_now, p_ai->i_max);
            // 核心：在 SD 卡里搜寻最接近的设备
            char *match_name = Identify_Appliance_In_SD(p_now, pf_now, p_ai->i_max, v_now, i_now);

            if (is_unknown_name(match_name)) {
                store_pending(index, &feat); // 没搜到：进入影子学习流程
            } else {
                clear_pending(index);        // 搜到了：直接更新显示
                strncpy(g_strip.sockets[index].device_name, match_name, 31);
            }
            p_ai->state = AI_LOCKED; // 识别完成，锁定状态防止跳变
            break;
        }

        case AI_LOCKED: break; // 锁定态：维持结果，直到下一次开关或复位
        default: break;
    }
}

char * Identify_Appliance_In_SD(float p_now, float pf_now, float i_max, float v_now, float i_steady)
{
    /*
     * 当前版本匹配仅使用 p_now + pf_now：
     * - i_max / v_now / i_steady 已保留参数位，后续可扩展到多特征加权匹配；
     * - 保留形参可避免未来扩展时改动调用链。
     */
    (void)i_max;
    (void)v_now;
    (void)i_steady;

    static FIL fil;
    static char best_name[20];
    char line[160];

    float best = 999999.0f;
    float second = 999999.0f;

    strcpy(best_name, UNKNOWN_NAME);

    /* 1) 打开设备库 CSV，失败即返回 SD Error。 */
    FRESULT res = f_open(&fil, DEVICE_DB_PATH, FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK)
    {
        printf("[REC] open failed path=%s res=%d\r\n", DEVICE_DB_PATH, res);
        return "SD Error";
    }

    /* 2) 读取并跳过首行表头。 */
    if (!f_gets(line, sizeof(line), &fil))
    {
        f_close(&fil);
        printf("[REC] empty file\r\n");
        return UNKNOWN_NAME;
    }

    int parsed_ok = 0;
    int parsed_bad = 0;

    while (f_gets(line, sizeof(line), &fil))
    {
        if (line[0] == '\0' || line[0] == '\r' || line[0] == '\n') continue;
        if (strstr(line, "ID,Name,Power") != NULL) continue;

        unsigned long id;
        char name[20] = {0};
        float p_lib, pf_lib, sr, q;

        /* 3) 逐行解析一条设备记录。 */
        int cnt = sscanf(line, "%lu,%19[^,],%f,%f,%f,%f", &id, name, &p_lib, &pf_lib, &sr, &q);

        if (cnt != 6)
        {
            parsed_bad++;
            if (parsed_bad <= 3)
            {
                printf("[REC] parse fail line: %s\r\n", line);
            }
            continue;
        }

        parsed_ok++;

        size_t len = strlen(name);
        if (len && name[len - 1] == '\r') name[len - 1] = '\0';

        /*
         * 4) 计算距离分数：
         * - 功率差按 100W 缩放，避免量纲过大淹没 PF；
         * - 当前权重：功率 0.85，功率因数 0.15。
         */
        float dp = (p_now - p_lib) / 100.0f;
        float dpf = (pf_now - pf_lib);
        float d = sqrtf(dp * dp * 0.85f + dpf * dpf * 0.15f);

        if (d < best)
        {
            second = best;
            best = d;
            strncpy(best_name, name, sizeof(best_name) - 1);
            best_name[sizeof(best_name) - 1] = '\0';
        }
        else if (d < second)
        {
            second = d;
        }
    }

    f_close(&fil);

    float margin = second - best;

    printf("[REC] p=%.2f pf=%.2f parsed_ok=%d bad=%d d1=%.3f d2=%.3f margin=%.3f best=%s\r\n",
           p_now, pf_now, parsed_ok, parsed_bad, best, second, margin, best_name);

    if (parsed_ok == 0)
    {
        return UNKNOWN_NAME;
    }

    /*
     * 5) 置信度门限：
     * - d1 越小越像；
     * - 大于阈值则判定为 Unknown，交给 shadow learning 命名流程。
     */
    const float THRESH_D1 = 0.12f;
    if (best > THRESH_D1)
    {
        return UNKNOWN_NAME;
    }

    return best_name;
}
