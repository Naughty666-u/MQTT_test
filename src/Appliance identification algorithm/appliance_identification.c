#include "appliance_identification.h"
#include "Systick.h"
#include "stdio.h"
#include "cJSON_handle.h"
#include <math.h>
#include <string.h>
#include "ff.h"
#include "sdcard_data_handle.h"
#include "Relay.h"
#include "log.h"

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

/* 软插拔重学习状态机：
 * - REPLUG_IDLE：空闲
 * - REPLUG_WAIT_OFF：已执行断电，等待 OFF 保持时间
 * - REPLUG_WAIT_ON_SETTLE：已重新上电，等待电参稳定后触发采样
 */
typedef enum {
    REPLUG_IDLE = 0,
    REPLUG_WAIT_OFF,
    REPLUG_WAIT_ON_SETTLE,
} Replug_State_t;

/* 每路插座的软插拔控制上下文 */
typedef struct {
    Replug_State_t state;   /* 当前软插拔阶段 */
    uint32_t deadline_tick; /* 当前阶段截止时刻（毫秒 tick） */
} Socket_Replug_Ctrl_t;

static Socket_Replug_Ctrl_t g_replug[AI_SOCKET_NUM] = {0};

/* LEARN_COMMIT 请求的轻量任务描述：
 * 只在这里缓存“要提交什么”，具体 SD 写入由后台状态机执行。
 */
typedef struct {
    bool valid;         /* 该槽位是否有待处理任务 */
    uint32_t pending_id;/* 需要提交的 pending 样本 ID */
    char name[20];      /* 用户命名 */
} Socket_Commit_Task_t;

static Socket_Commit_Task_t g_commit_task[AI_SOCKET_NUM] = {0};

/* SD 提交状态机：
 * 目标是把原先一次性阻塞的 f_open/f_write/f_close 拆到多轮主循环里推进。
 */
typedef enum {
    COMMIT_SM_IDLE = 0,   /* 空闲态 */
    COMMIT_SM_OPEN,       /* 打开/创建 Device.csv */
    COMMIT_SM_SCAN_PREPARE,/* 扫描前准备（定位到文件头） */
    COMMIT_SM_SCAN_STEP,  /* 每次扫描一行，检查是否重名 */
    COMMIT_SM_WRITE_HEADER,/* 文件为空时写入表头 */
    COMMIT_SM_SEEK_END,   /* 定位到文件末尾准备追加 */
    COMMIT_SM_WRITE_ROW,  /* 追加一行设备数据 */
    COMMIT_SM_CLOSE,      /* 关闭文件 */
    COMMIT_SM_DONE        /* 收尾：更新状态、打日志、释放上下文 */
} Commit_Sm_State_t;

/* SD 提交状态机运行时上下文（全局单实例） */
typedef struct {
    bool active;            /* 状态机是否在运行 */
    bool file_opened;       /* 文件是否已打开，决定是否需要 close */
    bool need_write_header; /* 目标文件是否为空，是否要写表头 */
    bool duplicate_found;   /* 扫描到同名记录时置位（跳过追加） */
    uint8_t index;          /* 当前处理的插座索引 */
    uint32_t pending_id;    /* 提交事务对应的 pending ID */
    uint32_t start_tick;    /* 事务开始时间，用于耗时统计 */
    Appliance_Data_t feat;  /* 待写入（或比对）的特征快照 */
    Commit_Sm_State_t state;/* 当前状态机阶段 */
    FRESULT res;            /* 当前/最终 FatFs 结果码 */
    FIL fil;                /* FatFs 文件句柄 */
    char line[160];         /* 扫描 CSV 用的行缓冲 */
    char out[160];          /* 追加写入用的输出缓冲 */
    UINT bw;                /* 本次写入字节数 */
} Commit_Sm_Ctx_t;

static Commit_Sm_Ctx_t g_commit_sm = {0};

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
    g_commit_task[index].valid = false;
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
        request_status_upload();
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
        g_replug[index].state = REPLUG_IDLE;
        request_status_upload();
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
        g_replug[index].state = REPLUG_IDLE;
        request_status_upload();
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

bool AI_Request_Commit_Pending(uint8_t index, uint32_t pending_id, const char *name)
{
    if (index >= AI_SOCKET_NUM || name == NULL || name[0] == '\0') return false;
    if (!g_pending[index].valid) return false;
    if (g_pending[index].pending_id != pending_id) return false;
    if (strlen(name) >= sizeof(g_pending[index].feat.name)) return false;

    g_commit_task[index].valid = true;
    g_commit_task[index].pending_id = pending_id;
    strncpy(g_commit_task[index].name, name, sizeof(g_commit_task[index].name) - 1);
    g_commit_task[index].name[sizeof(g_commit_task[index].name) - 1] = '\0';
    return true;
}

static bool commit_sm_start(uint8_t index, uint32_t pending_id, const char *name)
{
    if (index >= AI_SOCKET_NUM || name == NULL || name[0] == '\0') return false;
    if (!g_pending[index].valid) return false;
    if (g_pending[index].pending_id != pending_id) return false;
    if (strlen(name) >= sizeof(g_pending[index].feat.name)) return false;

    memset(&g_commit_sm, 0, sizeof(g_commit_sm));
    g_commit_sm.active = true;
    g_commit_sm.index = index;
    g_commit_sm.pending_id = pending_id;
    g_commit_sm.start_tick = HAL_GetTick();
    g_commit_sm.feat = g_pending[index].feat;
    strncpy(g_commit_sm.feat.name, name, sizeof(g_commit_sm.feat.name) - 1);
    g_commit_sm.feat.name[sizeof(g_commit_sm.feat.name) - 1] = '\0';
    g_commit_sm.state = COMMIT_SM_OPEN;
    g_commit_sm.res = FR_OK;
    return true;
}

static void commit_sm_finalize(void)
{
    uint32_t cost_ms = HAL_GetTick() - g_commit_sm.start_tick;

    if (g_commit_sm.res == FR_OK)
    {
        if (g_commit_sm.duplicate_found)
        {
            LOGI("[SD] duplicate name='%s', skip append\r\n", g_commit_sm.feat.name);
        }

        strncpy(g_strip.sockets[g_commit_sm.index].device_name,
                g_commit_sm.feat.name,
                sizeof(g_strip.sockets[g_commit_sm.index].device_name) - 1);
        g_strip.sockets[g_commit_sm.index].device_name[sizeof(g_strip.sockets[g_commit_sm.index].device_name) - 1] = '\0';
        clear_pending(g_commit_sm.index);
        request_status_upload();

        LOGI("[PERF] learn commit socket=%u cost=%lu ms result=ok\r\n",
             g_commit_sm.index, (unsigned long)cost_ms);
    }
    else
    {
        LOGW("[PERF] learn commit socket=%u cost=%lu ms result=fail res=%d\r\n",
             g_commit_sm.index, (unsigned long)cost_ms, g_commit_sm.res);
        request_status_upload();
    }

    memset(&g_commit_sm, 0, sizeof(g_commit_sm));
}

static void commit_sm_step(void)
{
    if (!g_commit_sm.active) return;

    /* 事务一致性保护：pending 上下文失效时立即中止 */
    if ((g_commit_sm.state != COMMIT_SM_DONE) &&
        (g_commit_sm.state != COMMIT_SM_CLOSE) &&
        ((g_commit_sm.index >= AI_SOCKET_NUM) ||
         (!g_pending[g_commit_sm.index].valid) ||
         (g_pending[g_commit_sm.index].pending_id != g_commit_sm.pending_id)))
    {
        g_commit_sm.res = FR_INVALID_OBJECT;
        g_commit_sm.state = g_commit_sm.file_opened ? COMMIT_SM_CLOSE : COMMIT_SM_DONE;
    }

    switch (g_commit_sm.state)
    {
        case COMMIT_SM_OPEN:
            g_commit_sm.res = f_open(&g_commit_sm.fil, DEVICE_DB_PATH, FA_OPEN_ALWAYS | FA_WRITE | FA_READ);
            if (g_commit_sm.res != FR_OK)
            {
                g_commit_sm.state = COMMIT_SM_DONE;
                break;
            }
            g_commit_sm.file_opened = true;
            g_commit_sm.need_write_header = (f_size(&g_commit_sm.fil) == 0U);
            g_commit_sm.state = COMMIT_SM_SCAN_PREPARE;
            break;

        case COMMIT_SM_SCAN_PREPARE:
            if (g_commit_sm.need_write_header)
            {
                g_commit_sm.state = COMMIT_SM_WRITE_HEADER;
                break;
            }
            g_commit_sm.res = f_lseek(&g_commit_sm.fil, 0U);
            if (g_commit_sm.res != FR_OK)
            {
                g_commit_sm.state = COMMIT_SM_CLOSE;
                break;
            }
            (void)f_gets(g_commit_sm.line, sizeof(g_commit_sm.line), &g_commit_sm.fil); /* skip header line */
            g_commit_sm.state = COMMIT_SM_SCAN_STEP;
            break;

        case COMMIT_SM_SCAN_STEP:
        {
            char *line = f_gets(g_commit_sm.line, sizeof(g_commit_sm.line), &g_commit_sm.fil);
            if (line == NULL)
            {
                g_commit_sm.state = COMMIT_SM_SEEK_END;
                break;
            }

            if (line[0] == '\0' || line[0] == '\r' || line[0] == '\n')
            {
                break;
            }
            if (strstr(line, "ID,Name,Power") != NULL)
            {
                break;
            }

            unsigned long id_dummy;
            char lib_name[20] = {0};
            float p, pf, sr, q;
            int cnt = sscanf(line, "%lu,%19[^,],%f,%f,%f,%f", &id_dummy, lib_name, &p, &pf, &sr, &q);
            if (cnt == 6)
            {
                size_t len = strlen(lib_name);
                if (len && lib_name[len - 1] == '\r') lib_name[len - 1] = '\0';
                if (strcmp(lib_name, g_commit_sm.feat.name) == 0)
                {
                    g_commit_sm.duplicate_found = true;
                    g_commit_sm.res = FR_OK;
                    g_commit_sm.state = COMMIT_SM_CLOSE;
                }
            }
            break;
        }

        case COMMIT_SM_WRITE_HEADER:
        {
            const char *header = "ID,Name,Power,PF,SurgeRatio,Q_Reactive\r\n";
            g_commit_sm.res = f_write(&g_commit_sm.fil, header, (UINT)strlen(header), &g_commit_sm.bw);
            if (g_commit_sm.res != FR_OK)
            {
                g_commit_sm.state = COMMIT_SM_CLOSE;
                break;
            }
            g_commit_sm.state = COMMIT_SM_SEEK_END;
            break;
        }

        case COMMIT_SM_SEEK_END:
            if (g_commit_sm.duplicate_found)
            {
                g_commit_sm.state = COMMIT_SM_CLOSE;
                break;
            }
            g_commit_sm.res = f_lseek(&g_commit_sm.fil, f_size(&g_commit_sm.fil));
            if (g_commit_sm.res != FR_OK)
            {
                g_commit_sm.state = COMMIT_SM_CLOSE;
                break;
            }
            snprintf(g_commit_sm.out, sizeof(g_commit_sm.out), "%lu,%s,%.2f,%.2f,%.2f,%.2f\r\n",
                     (unsigned long)g_commit_sm.feat.id,
                     g_commit_sm.feat.name,
                     g_commit_sm.feat.power,
                     g_commit_sm.feat.pf,
                     g_commit_sm.feat.i_surge_ratio,
                     g_commit_sm.feat.q_reactive);
            g_commit_sm.state = COMMIT_SM_WRITE_ROW;
            break;

        case COMMIT_SM_WRITE_ROW:
            g_commit_sm.res = f_write(&g_commit_sm.fil,
                                      g_commit_sm.out,
                                      (UINT)strlen(g_commit_sm.out),
                                      &g_commit_sm.bw);
            g_commit_sm.state = COMMIT_SM_CLOSE;
            break;

        case COMMIT_SM_CLOSE:
            if (g_commit_sm.file_opened)
            {
                FRESULT close_res = f_close(&g_commit_sm.fil);
                if ((g_commit_sm.res == FR_OK) && (close_res != FR_OK))
                {
                    g_commit_sm.res = close_res;
                }
                g_commit_sm.file_opened = false;
            }
            g_commit_sm.state = COMMIT_SM_DONE;
            break;

        case COMMIT_SM_DONE:
            commit_sm_finalize();
            break;

        case COMMIT_SM_IDLE:
        default:
            break;
    }
}

void AI_Commit_Task(void)
{
    if (!g_commit_sm.active)
    {
        for (uint8_t i = 0; i < AI_SOCKET_NUM; i++)
        {
            if (!g_commit_task[i].valid) continue;
            if (commit_sm_start(i, g_commit_task[i].pending_id, g_commit_task[i].name))
            {
                g_commit_task[i].valid = false;
                break;
            }
            else
            {
                g_commit_task[i].valid = false;
                LOGW("[PERF] learn commit socket=%u cost=0 ms result=fail res=%d\r\n",
                     i, FR_INVALID_OBJECT);
                request_status_upload();
            }
        }
    }

    /* 每轮只推进一步，避免 SD 操作长时间独占主循环。 */
    commit_sm_step();
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
        request_status_upload();
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

    Relay_Set_OFF(index);
    g_strip.sockets[index].on = false;
    strncpy(g_strip.sockets[index].device_name, "Idle", sizeof(g_strip.sockets[index].device_name) - 1);
    g_strip.sockets[index].device_name[sizeof(g_strip.sockets[index].device_name) - 1] = '\0';

    g_replug[index].state = REPLUG_WAIT_OFF;
    g_replug[index].deadline_tick = HAL_GetTick() + AI_REPLUG_OFF_MS;
    request_status_upload();
}

void AI_Replug_Task(void)
{
    uint32_t now = HAL_GetTick();

    for (uint8_t i = 0; i < AI_SOCKET_NUM; i++)
    {
        switch (g_replug[i].state)
        {
            case REPLUG_IDLE:
                break;

            case REPLUG_WAIT_OFF:
                if ((int32_t)(now - g_replug[i].deadline_tick) >= 0)
                {
                    Relay_Set_ON(i);
                    g_strip.sockets[i].on = true;
                    g_replug[i].state = REPLUG_WAIT_ON_SETTLE;
                    g_replug[i].deadline_tick = now + AI_REPLUG_ON_SETTLE_MS;
                    request_status_upload();
                }
                break;

            case REPLUG_WAIT_ON_SETTLE:
                if ((int32_t)(now - g_replug[i].deadline_tick) >= 0)
                {
                    AI_Trigger_Sampling(i);
                    g_replug[i].state = REPLUG_IDLE;
                }
                break;

            default:
                g_replug[i].state = REPLUG_IDLE;
                break;
        }
    }
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
                request_status_upload();
            } else {
                clear_pending(index);        // 搜到了：直接更新显示
                strncpy(g_strip.sockets[index].device_name, match_name, sizeof(g_strip.sockets[index].device_name) - 1);
                g_strip.sockets[index].device_name[sizeof(g_strip.sockets[index].device_name) - 1] = '\0';
                request_status_upload();
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
        LOGW("[REC] open failed path=%s res=%d\r\n", DEVICE_DB_PATH, res);
        return "SD Error";
    }

    /* 2) 读取并跳过首行表头。 */
    if (!f_gets(line, sizeof(line), &fil))
    {
        f_close(&fil);
        LOGI("[REC] empty file\r\n");
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
                LOGW("[REC] parse fail line: %s\r\n", line);
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

    LOGI("[REC] p=%.2f pf=%.2f parsed_ok=%d bad=%d d1=%.3f d2=%.3f margin=%.3f best=%s\r\n",
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
