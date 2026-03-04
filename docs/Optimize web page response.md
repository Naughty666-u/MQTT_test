# Optimize web page response

## 1. 目标与现状

### 1.1 你当前的现状（基于现有代码）
- 状态上报在 `hal_entry.c` 里固定 `3000ms` 周期（或 `g_force_upload_flag` 触发）。
- 命令处理在 `process_cloud_cmd()` 内直接执行动作，`RELEARN_REPLUG` 会进入阻塞延时。
- `AI_Request_Relearn_Replug()` 使用 `R_BSP_SoftwareDelay()`，会卡住主循环，导致：
  - ACK 变慢
  - 状态上报变慢
  - 串口流解析滞后

### 1.2 优化目标（建议）
- 命令 ACK：尽量 `<100ms`
- 关键状态变化上报：`100~300ms` 可见
- 保底心跳上报：`1000~2000ms`

---

## 2. 总体优化思路（裸机，不上 RTOS）

核心思路只有两句：
1. **快路径立即返回**：收到命令先 ACK，不做耗时等待。
2. **耗时动作状态机化**：把延时动作拆成“分阶段 + 到点执行”，每次只做很短动作。

对应你工程就是：
- 把 `AI_Request_Relearn_Replug()` 的阻塞延时改成非阻塞状态机。
- 把上报从“纯 3s 轮询”改成“事件触发 + 心跳保底 + 节流”。

---

## 3. 优化一：阻塞重学习改为非阻塞状态机

### 3.1 需要改哪些文件
- `src/Appliance identification algorithm/appliance_identification.h`
- `src/Appliance identification algorithm/appliance_identification.c`
- `src/hal_entry.c`

### 3.2 新增状态机结构（建议）

在 `appliance_identification.h` 增加：

```c
// 重学习流程状态（软件插拔）
typedef enum {
    REPLUG_IDLE = 0,
    REPLUG_WAIT_OFF,
    REPLUG_WAIT_ON_SETTLE,
    REPLUG_TRIGGER_SAMPLE
} AI_ReplugState_t;

void AI_Replug_Task(void);  // 主循环里高频调用
```

在 `appliance_identification.c` 增加每路上下文：

```c
typedef struct {
    AI_ReplugState_t state;
    uint32_t deadline_tick;   // 下一步允许执行的时间点
    bool target_on_before;    // 记录重学习前是否本来就是 ON
} AI_ReplugCtrl_t;

static AI_ReplugCtrl_t g_replug[AI_SOCKET_NUM] = {0};
```

### 3.3 改造 `AI_Request_Relearn_Replug()`（关键）

把“立即 OFF + delay + ON + delay + trigger”改成“只启动流程，不等待”：

```c
void AI_Request_Relearn_Replug(uint8_t index)
{
    if (index >= AI_SOCKET_NUM) return;

    clear_pending(index);
    AI_Reset(index);

    g_replug[index].target_on_before = g_strip.sockets[index].on;

    // 第一步先断电（若当前已经ON）
    if (g_strip.sockets[index].on) {
        Relay_Set_OFF(index);
        g_strip.sockets[index].on = false;
    }

    g_replug[index].state = REPLUG_WAIT_OFF;
    g_replug[index].deadline_tick = HAL_GetTick() + AI_REPLUG_OFF_MS;
}
```

通俗解释：
- 以前：函数里“站着等 500ms + 300ms”。
- 现在：函数只“记个闹钟”，然后马上返回，主循环不会被卡住。

### 3.4 新增 `AI_Replug_Task()` 分阶段推进

```c
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
                }
                break;

            case REPLUG_WAIT_ON_SETTLE:
                if ((int32_t)(now - g_replug[i].deadline_tick) >= 0)
                {
                    g_replug[i].state = REPLUG_TRIGGER_SAMPLE;
                }
                break;

            case REPLUG_TRIGGER_SAMPLE:
                AI_Trigger_Sampling(i);
                g_replug[i].state = REPLUG_IDLE;
                break;
        }
    }
}
```

在 `hal_entry.c` 主循环增加调用：

```c
while (1)
{
    handle_uart_json_stream();

    AI_Replug_Task();   // 新增：非阻塞重学习推进器

    ...
}
```

---

## 4. 优化二：状态上报改成“事件触发 + 心跳保底 + 节流”

### 4.1 需要改哪些文件
- `src/hal_entry.c`
- `src/cJSON_handle/cJSON_handle.c/.h`
- `src/uart_hlw/uart_hlw.c`
- `src/Appliance identification algorithm/appliance_identification.c`

### 4.2 新增统一上报请求接口

在 `cJSON_handle.h` 增加：

```c
void request_status_upload(void);
```

在 `cJSON_handle.c` 实现：

```c
extern uint8_t g_force_upload_flag;

void request_status_upload(void)
{
    g_force_upload_flag = 1;
}
```

### 4.3 改 `hal_entry.c` 上报条件

把当前：
```c
if ((HAL_GetTick() - last_report >= 3000U) || g_force_upload_flag)
```

改为建议参数：
```c
#define HEARTBEAT_MS      1500U   // 心跳保底
#define UPLOAD_MIN_GAP_MS 200U    // 事件上报最小间隔（节流）

static uint32_t last_upload_tick = 0;

uint32_t now = HAL_GetTick();
bool heartbeat_due = (now - last_report >= HEARTBEAT_MS);
bool event_due = g_force_upload_flag && (now - last_upload_tick >= UPLOAD_MIN_GAP_MS);

if (heartbeat_due || event_due)
{
    g_force_upload_flag = 0;
    upload_strip_status();
    last_report = now;
    last_upload_tick = now;
}
```

通俗解释：
- 有变化就尽快报（最快 200ms 一次）。
- 没变化也定期报（比如 1.5s 一次）。
- 防止变化太多把串口/网络刷爆（节流）。

### 4.4 在关键状态变化处触发 `request_status_upload()`

建议加在以下位置：
- `Socket_Command_Handler()`：开关状态变化后
- `AI_Trigger_Sampling()`：进入 `Detecting...`
- `AI_Recognition_Engine()`：识别结果从 `Unknown/Detecting` 切到设备名时
- `AI_Commit_Pending()`：命名学习成功写库后
- `Data_Processing()`：功率跨阈值变化时（不要每帧都触发）

示例（功率变化触发）：

```c
// uart_hlw.c
static float g_last_report_power[4] = {0};

if (fabsf(g_strip.sockets[index].power - g_last_report_power[index]) >= 2.0f)
{
    g_last_report_power[index] = g_strip.sockets[index].power;
    request_status_upload();
}
```

---

## 5. 优化三：命令快收快回，减少网页超时

### 5.1 需要改哪些文件
- `src/cJSON_handle/cJSON_handle.c`

### 5.2 处理策略
- `process_cloud_cmd()` 只做：
  - 参数校验
  - 投递动作（置状态）
  - 立即 ACK
- 不在命令解析函数里做阻塞动作。

你当前最关键就是 `RELEARN_REPLUG`：
- 现在调用后会进入延时（阻塞）
- 改为调用“非阻塞启动函数”，然后立即 `send_ack(...success...)`

示例逻辑：

```c
else if (strcmp(type_str, "RELEARN_REPLUG") == 0)
{
    if (!parse_socket_index(root, &index)) {
        fail_reason = "bad_socket";
    } else {
        AI_Request_Relearn_Replug((uint8_t)index); // 现在是非阻塞启动
        ok = true;                                 // 可以立即ACK success
    }
}
```

---

## 6. 你会得到的直接收益

1. 网页状态更新明显更快
- 开关、识别态变化基本在 `100~300ms` 可见（取决于网络与串口）。

2. 命令超时明显减少
- ACK 不再被 `SoftwareDelay` 卡住。

3. 主循环稳定性提升
- 长动作不阻塞，采样、解析、上报互不拖累。

---

## 7. 落地顺序（建议按此执行）

1. 先做“非阻塞重学习状态机”
2. 再做“事件触发上报 + 心跳保底 + 节流”
3. 最后微调参数：
- `HEARTBEAT_MS`: 1000~2000
- `UPLOAD_MIN_GAP_MS`: 150~300
- 功率触发阈值：1~3W

---

## 8. 风险与注意事项

1. 事件触发太频繁会导致消息风暴
- 一定要有 `UPLOAD_MIN_GAP_MS`。

2. SD 写入仍可能慢
- `LEARN_COMMIT` 如果未来仍超时，可升级为“先 ACK accepted，后发完成事件”。

3. 日志打印太多也会拖慢
- `printf` 建议保留关键点，避免高频循环里大量打印。

---

## 9. 一句话总结

你的优化方向是对的：
- **把“等待”从函数里拿掉，改成状态机推进**；
- **把“固定周期上报”升级为“事件优先 + 心跳兜底”**；
- **命令处理先回执，重动作后台执行**。

这三点做好，裸机就能把网页响应速度提升到一个很实用的水平。
