#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include "circle_buf.h"
#include "cJSON_handle.h"
#include "bsp_wifi_esp8266.h"
#include "Systick.h"
#include "appliance_identification.h"

/* ESP8266 UART 回调里会把接收到的数据写入这个环形缓冲区。 */
extern circle_buf_t g_rx_buf;

/* 单次 JSON 解析缓冲区大小。 */
#define MAX_JSON_SIZE 512

/*
 * 串口流式解析缓冲区。
 * 用于把散落的字符拼成一条完整的 MQTT 下行文本。
 */
static char json_process_buf[MAX_JSON_SIZE];

/*
 * 主循环里的“立即上报”标志。
 * 由命令处理成功后置 1，下一轮循环会立刻 upload_strip_status()。
 */
extern uint8_t g_force_upload_flag;

void request_status_upload(void)
{
    g_force_upload_flag = 1;
}

/*
 * 全局排插状态：
 * - 由电参采样链路持续更新功率/电压/电流
 * - 由识别引擎更新 device_name / pending
 * - 由 upload_strip_status() 统一打包上报
 */
PowerStrip_t g_strip = {
    .voltage = 220.0f,
    .total_current = 0.0f,
    .total_power = 0.0f,
    .sockets = {
        {false, 0.0f, "None", false, 0},
        {false, 0.0f, "None", false, 0},
        {false, 0.0f, "None", false, 0},
        {false, 0.0f, "None", false, 0}
    }
};

/*
 * 网页联调测试函数：
 * - 首次调用时给4路插孔写入默认测试状态；
 * - 后续每次调用按 on 状态刷新功率与总量；
 * - 不修改命令解析链路，ON/OFF 仍然通过正常流程生效。
 */
void web_mqtt_test_fill_mock(void)
{
    static bool inited = false;
    static const float k_mock_power_w[4] = {85.0f, 46.0f, 23.5f, 12.0f};
    static const char *k_mock_name[4] = {"DeskLamp", "Fan", "Charger", "Speaker"};

    if (!inited)
    {
        /* 初始给两路通电，便于网页上线后马上看到功率与开关状态。 */
        g_strip.sockets[0].on = true;
        g_strip.sockets[1].on = true;
        g_strip.sockets[2].on = false;
        g_strip.sockets[3].on = false;

        for (int i = 0; i < 4; i++)
        {
            strncpy(g_strip.sockets[i].device_name, k_mock_name[i], sizeof(g_strip.sockets[i].device_name) - 1);
            g_strip.sockets[i].device_name[sizeof(g_strip.sockets[i].device_name) - 1] = '\0';
            g_strip.sockets[i].pending_valid = false;
            g_strip.sockets[i].pending_id = 0;
        }
        inited = true;
    }

    g_strip.voltage = 220.8f;
    g_strip.total_power = 0.0f;

    for (int i = 0; i < 4; i++)
    {
        if (g_strip.sockets[i].on)
        {
            /* ON 时按预设功率上报，便于前端观察开关与功率联动。 */
            g_strip.sockets[i].power = k_mock_power_w[i];

            /* 如果被其他流程改成 Idle/None，测试模式下恢复为固定测试名。 */
            if ((strcmp(g_strip.sockets[i].device_name, "Idle") == 0) ||
                (strcmp(g_strip.sockets[i].device_name, "None") == 0))
            {
                strncpy(g_strip.sockets[i].device_name, k_mock_name[i], sizeof(g_strip.sockets[i].device_name) - 1);
                g_strip.sockets[i].device_name[sizeof(g_strip.sockets[i].device_name) - 1] = '\0';
            }
        }
        else
        {
            g_strip.sockets[i].power = 0.0f;
        }

        g_strip.total_power += g_strip.sockets[i].power;
    }

    g_strip.total_current = (g_strip.voltage > 1.0f) ? (g_strip.total_power / g_strip.voltage) : 0.0f;
}

/**
 * @brief 发送命令 ACK 回执
 * @param cmd_id  命令 ID（网页下发时提供）
 * @param status  "success" / "failed"
 * @param reason  失败原因；成功可传 NULL
 * @param cost_ms 耗时统计（当前写固定值）
 * @return true 发送流程走通；false 参数不合法或构造失败
 */
static bool send_ack(const char *cmd_id, const char *status, const char *reason, int cost_ms)
{
    /* 没有 cmdId 时不发 ACK，避免网页无法关联回执。 */
    if (cmd_id == NULL || cmd_id[0] == '\0')
    {
        return false;
    }

    cJSON *ack = cJSON_CreateObject();
    if (!ack)
    {
        return false;
    }

    /* 构造 ACK JSON。 */
    cJSON_AddStringToObject(ack, "cmdId", cmd_id);
    /*
     * 与后端联调契约对齐：
     * - 成功: success
     * - 失败: failed
     */
    cJSON_AddStringToObject(ack, "status", status ? status : "failed");
    cJSON_AddNumberToObject(ack, "costMs", cost_ms);

    /* 固定输出 errorMsg 字段：成功为空串，失败填失败原因。 */
    cJSON_AddStringToObject(ack, "errorMsg", reason ? reason : "");

    /* 序列化并发布到 ACK 主题。 */
    char *ack_out = cJSON_PrintUnformatted(ack);
    bool sent_ok = false;
    if (ack_out)
    {
        sent_ok = Send_Data_Raw(MQTT_PUB_ACK, ack_out);
        if (!sent_ok)
        {
            /* ACK 丢失会导致前端卡住，失败时快速重试一次。 */
            R_BSP_SoftwareDelay(20, BSP_DELAY_UNITS_MILLISECONDS);
            sent_ok = Send_Data_Raw(MQTT_PUB_ACK, ack_out);
        }
        free(ack_out);
    }

    cJSON_Delete(ack);
    if (!sent_ok)
    {
        printf("[ACK] send failed cmdId=%s status=%s\r\n",
               (cmd_id ? cmd_id : "null"),
               (status ? status : "null"));
    }
    return sent_ok;
}

/**
 * @brief 上报当前排插状态（status 主题）
 * @note 网页侧应以该包为状态真值来源
 */
void upload_strip_status(void)
{
    static uint32_t s_last_upload_tick = 0;
    uint32_t now_tick = HAL_GetTick();
    uint32_t delta_ms = (s_last_upload_tick == 0U) ? 0U : (now_tick - s_last_upload_tick);
    s_last_upload_tick = now_tick;

    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    /* 顶层统计信息。 */
    cJSON_AddNumberToObject(root, "ts", HAL_GetTick());
    cJSON_AddBoolToObject(root, "online", true);
    cJSON_AddNumberToObject(root, "total_power_w", g_strip.total_power);
    cJSON_AddNumberToObject(root, "voltage_v", g_strip.voltage);
    cJSON_AddNumberToObject(root, "current_a", g_strip.total_current);

    /* 逐路构建插孔状态数组。 */
    cJSON *sockets = cJSON_CreateArray();
    for (int i = 0; i < 4; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", i + 1);
        cJSON_AddBoolToObject(item, "on", g_strip.sockets[i].on);
        cJSON_AddNumberToObject(item, "power_w", g_strip.sockets[i].on ? g_strip.sockets[i].power : 0.0f);
        cJSON_AddStringToObject(item, "device", g_strip.sockets[i].device_name);

        /*
         * 同时满足以下两个条件才上报 pendingId：
         * 1) pending_valid=true：该插孔确实存在待命名样本；
         * 2) device_name=="Unknown"：当前识别状态为未识别。
         * 这样网页端可据此显示“命名并加入设备库”入口。
         */
        if (g_strip.sockets[i].pending_valid && (strcmp(g_strip.sockets[i].device_name, "Unknown") == 0))
        {
            cJSON_AddNumberToObject(item, "pendingId", g_strip.sockets[i].pending_id);
        }

        cJSON_AddItemToArray(sockets, item);
    }
    cJSON_AddItemToObject(root, "sockets", sockets);

    /* 发布状态。 */
    char *out = cJSON_PrintUnformatted(root);
    if (out)
    {
        bool ok = Send_Data_Raw(MQTT_PUB_STATUS, out);
        if (!ok)
        {
            printf("[TX] status publish failed\r\n");
        }
        free(out);
    }

    printf("[PERF] status upload interval=%lu ms\r\n", (unsigned long)delta_ms);
    cJSON_Delete(root);
}

/**
 * @brief 解析 socketId 并转换为内部索引
 * @param root cJSON 根节点
 * @param index_out 输出索引（0..3）
 * @return true 解析成功，false 失败
 */
static bool parse_socket_index(cJSON *root, int *index_out)
{
    /* 云端协议 socketId 使用 1..4。 */
    cJSON *socketId = cJSON_GetObjectItem(root, "socketId");
    if (!cJSON_IsNumber(socketId)) return false;

    int id_val = socketId->valueint;
    int index = id_val - 1;

    /* 越界保护。 */
    if (index < 0 || index >= 4) return false;

    *index_out = index;
    return true;
}

/**
 * @brief 处理一条云端命令 JSON
 *
 * 支持指令：
 * 1. ON/OFF
 * 2. LEARN_COMMIT
 * 3. RELEARN_REPLUG
 * 4. CORRECT（前端纠错指令，等价 RELEARN_REPLUG）
 */
void process_cloud_cmd(const char *json_str)
{
    /*
     * 输入可能是整行文本：+MQTTSUBRECV:...,{...}
     * 因此先截取最外层 JSON 对象区域。
     */
    char *json_start = strchr(json_str, '{');
    if (!json_start) return;

    char *json_end = strrchr(json_start, '}');
    if (!json_end) return;

    /* 临时封口，确保 cJSON 只看到对象本体。 */
    char backup = *(json_end + 1);
    *(json_end + 1) = '\0';

    cJSON *root = cJSON_Parse(json_start);
    if (!root)
    {
        *(json_end + 1) = backup;
        return;
    }
    uint32_t cmd_start_tick = HAL_GetTick();

    /* cmdId 用于 ACK 关联；若顶层缺失则回退查 payload.cmdId。 */
    cJSON *cmdId = cJSON_GetObjectItem(root, "cmdId");
    if (!cJSON_IsString(cmdId))
    {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload))
        {
            cmdId = cJSON_GetObjectItem(payload, "cmdId");
        }
    }
    const char *cmd_id_str = cJSON_IsString(cmdId) ? cmdId->valuestring : NULL;

    /* type 是必须字段。 */
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type))
    {
        uint32_t ack_cost_total = HAL_GetTick() - cmd_start_tick;
        int ack_cost_ms = (ack_cost_total == 0U) ? 1 : (int)ack_cost_total;
        bool ack_ok = send_ack(cmd_id_str, "failed", "missing_type", ack_cost_ms);
        printf("[PERF] cmd=<missing> ack=%lu ms result=failed sent=%d\r\n",
               (unsigned long)ack_cost_total, ack_ok ? 1 : 0);
        cJSON_Delete(root);
        *(json_end + 1) = backup;
        return;
    }

    const char *type_str = type->valuestring;
    int index = -1;
    bool ok = false;
    const char *fail_reason = "bad_request";

    /* 分支 1：开关控制 */
    if ((strcmp(type_str, "ON") == 0) || (strcmp(type_str, "OFF") == 0))
    {
        /*
         * ON/OFF 统一走这个分支：
         * - ON  -> 继电器吸合 + 清理旧 pending + 进入新识别事务
         * - OFF -> 继电器断开 + 清理识别锁/pending
         */
        if (!parse_socket_index(root, &index))
        {
            /* socketId 不合法（不在 1..4 或类型不对） */
            fail_reason = "bad_socket";
        }
        else
        {
            /* 按 type 计算目标通断状态，并交给统一硬件控制入口处理。 */
            bool is_on = (strcmp(type_str, "ON") == 0);
            Socket_Command_Handler((uint8_t)index, is_on);
            ok = true;
        }
    }
    /* 分支 2：Unknown 命名提交 */
    else if (strcmp(type_str, "LEARN_COMMIT") == 0)
    {
        /* 从命令中取出待命名样本ID和用户输入的设备名。 */
        cJSON *pendingId = cJSON_GetObjectItem(root, "pendingId");
        cJSON *name = cJSON_GetObjectItem(root, "name");
        cJSON *payload = cJSON_GetObjectItem(root, "payload");

        /*
         * 兼容后端不同打包方式：
         * - 优先使用顶层 pendingId/name；
         * - 若缺失，则回退到 payload.pendingId/payload.name。
         */
        if ((!cJSON_IsNumber(pendingId) || !cJSON_IsString(name)) && cJSON_IsObject(payload))
        {
            if (!cJSON_IsNumber(pendingId))
            {
                pendingId = cJSON_GetObjectItem(payload, "pendingId");
            }
            if (!cJSON_IsString(name))
            {
                name = cJSON_GetObjectItem(payload, "name");
            }
        }

        /* 第一步：先校验 socketId 是否有效（1..4）。 */
        if (!parse_socket_index(root, &index))
        {
            fail_reason = "bad_socket";
        }
        /* 第二步：校验 pendingId 和 name 字段类型是否正确。 */
        else if (!cJSON_IsNumber(pendingId) || !cJSON_IsString(name))
        {
            fail_reason = "missing_field";
        }
        else
        {
            /*
             * 第三步：执行提交动作。
             * AI_Commit_Pending 内部会做严格匹配：
             * - index 对应插孔必须存在 pending
             * - pendingId 必须与缓存样本一致
             * - name 合法后才会写入 Device.csv
             */
            bool accepted = AI_Request_Commit_Pending((uint8_t)index,
                                                      (uint32_t)pendingId->valuedouble,
                                                      name->valuestring);
            if (accepted)
            {
                /* 快速受理：实际写 SD 在主循环后台任务执行。 */
                ok = true;
            }
            else
            {
                fail_reason = "commit_rejected";
            }
        }
    }
    /* 分支 3：软件插拔重采样（兼容 CORRECT） */
    else if ((strcmp(type_str, "RELEARN_REPLUG") == 0) || (strcmp(type_str, "CORRECT") == 0))
    {
        /*
         * 该命令用于“强纠错”：
         * 通过 OFF->delay->ON 重建启动暂态，再重新采样识别。
         */
        if (!parse_socket_index(root, &index))
        {
            fail_reason = "bad_socket";
        }
        else
        {
            /* 适用于需要更干净启动特征的场景。 */
            AI_Request_Relearn_Replug((uint8_t)index);
            ok = true;
        }
    }
    else
    {
        /* 未支持的 type，统一返回 unsupported_type。 */
        fail_reason = "unsupported_type";
    }

    if (ok)
    {
        /* 成功后强制下一轮立即上报，网页尽快看到状态变化。 */
        uint32_t ack_cost_total = HAL_GetTick() - cmd_start_tick;
        int ack_cost_ms = (ack_cost_total == 0U) ? 1 : (int)ack_cost_total;
        bool ack_ok = send_ack(cmd_id_str, "success", NULL, ack_cost_ms);
        printf("[PERF] cmd=%s ack=%lu ms result=success sent=%d\r\n",
               type_str, (unsigned long)ack_cost_total, ack_ok ? 1 : 0);
        request_status_upload();
    }
    else
    {
        uint32_t ack_cost_total = HAL_GetTick() - cmd_start_tick;
        int ack_cost_ms = (ack_cost_total == 0U) ? 1 : (int)ack_cost_total;
        bool ack_ok = send_ack(cmd_id_str, "failed", fail_reason, ack_cost_ms);
        printf("[PERF] cmd=%s ack=%lu ms result=failed reason=%s\r\n",
               type_str, (unsigned long)ack_cost_total, fail_reason);
        if (!ack_ok)
        {
            printf("[PERF] cmd=%s ack send failed\r\n", type_str);
        }
    }

    cJSON_Delete(root);
    *(json_end + 1) = backup;
}

/**
 * @brief 从 UART 环形缓冲区流式提取并处理 JSON 命令
 *
 * 解析策略：
 * 1. 按字节读取
 * 2. 用大括号计数判断是否拼到一个完整 JSON
 * 3. 超时未闭合则清空缓冲
 */
void handle_uart_json_stream(void)
{
    static uint16_t pos = 0;
    static int brace_count = 0;
    static uint32_t last_byte_time = 0;
    uint8_t temp_byte;

    while (g_rx_buf.get(&g_rx_buf, &temp_byte) == 0)
    {
        uint32_t now = HAL_GetTick();

        /*
         * 超时保护：
         * 如果上一条半包超过 1s 还没闭合，认为脏包，直接重置解析状态。
         */
        if ((pos > 0U) && (last_byte_time != 0U) && ((now - last_byte_time) > 1000U))
        {
            pos = 0;
            brace_count = 0;
            memset(json_process_buf, 0, MAX_JSON_SIZE);
        }

        /* 写入线性缓冲区。 */
        if (pos < MAX_JSON_SIZE - 1)
        {
            json_process_buf[pos++] = (char)temp_byte;
            json_process_buf[pos] = '\0';
        }

        /* 大括号计数，用于判断 JSON 是否闭合。 */
        if (temp_byte == '{')
        {
            brace_count++;
        }
        else if (temp_byte == '}')
        {
            if (brace_count > 0) brace_count--;

            if (brace_count == 0 && pos > 0)
            {
                /* 只处理 MQTT 下行文本帧，忽略其它 AT 回显/系统日志。 */
                if (strstr(json_process_buf, "+MQTTSUBRECV"))
                {
                    process_cloud_cmd(json_process_buf);
                }

                /* 一帧处理完成，清空缓冲准备下一帧。 */
                pos = 0;
                memset(json_process_buf, 0, MAX_JSON_SIZE);
            }
        }

        last_byte_time = now;
    }
}
