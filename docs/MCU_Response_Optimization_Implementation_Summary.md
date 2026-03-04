# MCU及时响应实现总结（基于当前落地代码）

## 1. 文档目的

这份文档用于回答三个问题：

1. 我们是如何把单片机响应做“更及时”的？
2. 相比历史版本（纠错 + 识别重命名），我们具体优化了哪里？
3. 代码到底改在什么文件、什么函数、为什么这样改？

> 说明：本总结以当前工程中的真实代码为准，不是方案草图。

---

## 2. 历史版本的主要痛点

历史版本（已有纠错 + 重命名 + shadow learning）在功能上是完整的，但时序上有三个典型问题：

1. 上报策略偏“慢”
- 状态上报主要依赖固定周期（3s）+ 强制标志。
- 用户操作后，网页容易感觉“慢半拍”。

2. 命令链路有阻塞点
- `RELEARN_REPLUG` 在 MCU 内部使用 `R_BSP_SoftwareDelay()`。
- 阻塞期间主循环无法及时处理其它消息，导致 ACK / 状态推送延迟。

3. ACK 发送可靠性不足
- 旧版本发送函数出现超时时会静默失败（上层不知道 ACK 是否真的发出）。
- 前端会出现“上一条命令未完成”的卡住体验。

---

## 3. 本次优化总览（做了什么）

我们做了四层优化，形成一整套闭环：

1. 非阻塞任务化
- 把“重学习软件插拔”改为状态机推进，去掉阻塞延时。

2. 上报机制升级
- 从“固定 3s”改为“事件触发 + 心跳保底 + 节流”。
- 参数：`HEARTBEAT_MS=1500`，`UPLOAD_MIN_GAP_MS=200`。

3. LEARN_COMMIT 快速受理
- 命令线程只做轻量校验 + 入队，后台任务再写 SD。

4. ACK 可靠性增强
- 发送函数返回 success/fail，ACK 失败自动重试一次，并输出失败原因日志。

---

## 4. 具体优化点与代码位置

## 4.1 上报从“固定轮询”到“事件+心跳”

### 修改位置
- `src/hal_entry.c`

### 关键改动

```c
#define HEARTBEAT_MS 1500U
#define UPLOAD_MIN_GAP_MS 200U

static uint32_t last_upload_tick = 0;
...
uint32_t now_tick = HAL_GetTick();
bool heartbeat_due = (now_tick - last_report >= HEARTBEAT_MS);
bool event_due = g_force_upload_flag && (now_tick - last_upload_tick >= UPLOAD_MIN_GAP_MS);
if (heartbeat_due || event_due)
{
    g_force_upload_flag = 0;
    upload_strip_status();
    last_report = now_tick;
    last_upload_tick = now_tick;
}
```

### 为什么这么改
- `heartbeat_due`：保证“没事件也有保底状态同步”。
- `event_due`：有变化就快发，提升网页体感。
- `UPLOAD_MIN_GAP_MS`：避免变化频繁时刷屏/阻塞串口。

---

## 4.2 重学习（RELEARN_REPLUG）非阻塞化

### 修改位置
- `src/Appliance identification algorithm/appliance_identification.h`
- `src/Appliance identification algorithm/appliance_identification.c`
- `src/hal_entry.c`

### 历史问题
旧逻辑在 `AI_Request_Relearn_Replug()` 中直接调用延时函数：

```c
R_BSP_SoftwareDelay(...);
```

这会卡住主循环。

### 当前改动（状态机）

```c
typedef enum {
    REPLUG_IDLE = 0,
    REPLUG_WAIT_OFF,
    REPLUG_WAIT_ON_SETTLE,
} Replug_State_t;

typedef struct {
    Replug_State_t state;
    uint32_t deadline_tick;
} Socket_Replug_Ctrl_t;
```

```c
void AI_Request_Relearn_Replug(uint8_t index)
{
    clear_pending(index);
    AI_Reset(index);

    Relay_Set_OFF(index);
    g_strip.sockets[index].on = false;
    ...
    g_replug[index].state = REPLUG_WAIT_OFF;
    g_replug[index].deadline_tick = HAL_GetTick() + AI_REPLUG_OFF_MS;
    request_status_upload();
}
```

```c
void AI_Replug_Task(void)
{
    uint32_t now = HAL_GetTick();
    for (...) {
        switch (g_replug[i].state) {
            case REPLUG_WAIT_OFF:
                // 到点上电 -> 转下个阶段
                break;
            case REPLUG_WAIT_ON_SETTLE:
                // 到点触发 AI_Trigger_Sampling
                break;
        }
    }
}
```

主循环接入：

```c
handle_uart_json_stream();
AI_Replug_Task();
```

### 为什么这么改
- 命令处理返回更快，ACK 不被“延时操作”拖慢。
- 重学习过程仍然完整，但不阻塞其它链路。

---

## 4.3 LEARN_COMMIT：从“同步写SD”改为“快速受理+后台写SD”

### 修改位置
- `src/Appliance identification algorithm/appliance_identification.h`
- `src/Appliance identification algorithm/appliance_identification.c`
- `src/cJSON_handle/cJSON_handle.c`
- `src/hal_entry.c`

### 新增接口

```c
bool AI_Request_Commit_Pending(uint8_t index, uint32_t pending_id, const char *name);
void AI_Commit_Task(void);
```

### 核心逻辑

1) 命令线程快速受理：

```c
bool accepted = AI_Request_Commit_Pending(...);
if (accepted) ok = true;
else fail_reason = "commit_rejected";
```

2) 主循环后台执行写 SD：

```c
AI_Commit_Task();
```

3) 后台任务实际写入：

```c
FRESULT res = AI_Commit_Pending(i, g_commit_task[i].pending_id, g_commit_task[i].name);
```

### 为什么这么改
- `Save_Appliance_Data()` 属于慢操作（文件系统写入）。
- 若在命令处理路径同步执行，会拉长 ACK 时延，前端容易超时。
- 改成后台任务后，命令路径轻量、可预测。

---

## 4.4 统一事件上报入口

### 修改位置
- `src/cJSON_handle/cJSON_handle.h`
- `src/cJSON_handle/cJSON_handle.c`
- 多处调用点（AI状态变化、开关变化、电参变化）

### 新增函数

```c
void request_status_upload(void)
{
    g_force_upload_flag = 1;
}
```

### 调用场景（已接入）
- 开关状态变化（`Socket_Command_Handler`）
- 进入检测态（`AI_Trigger_Sampling`）
- 识别命中/未命中（`AI_Recognition_Engine`）
- pending commit 成功
- 功率大幅变化（`uart_hlw.c`）

### 为什么这么改
- 统一入口便于维护，不再散落直接操作 `g_force_upload_flag`。
- 让“状态变化 -> 尽快上报”路径更一致。

---

## 4.5 功率变化触发上报（10W阈值）

### 修改位置
- `src/uart_hlw/uart_hlw.c`

### 关键代码

```c
#define POWER_UPLOAD_DELTA_W 10.0f
...
float p_prev = g_strip.sockets[index].power;
g_strip.sockets[index].power = p_used;
if (fabsf(p_used - p_prev) >= POWER_UPLOAD_DELTA_W)
{
    request_status_upload();
}
```

### 为什么这么改
- 避免每帧都上报（太频繁）。
- 保留“明显变化时快速通知网页”的能力。

---

## 4.6 ACK可靠性增强（重点）

### 修改位置
- `src/bsp_esp8266/bsp_wifi_esp8266.h`
- `src/bsp_esp8266/bsp_wifi_esp8266.c`
- `src/cJSON_handle/cJSON_handle.c`

### 关键变化

1) `Send_Data_Raw` 返回值改为 `bool`

```c
bool Send_Data_Raw(char *topics, char *data)
```

2) ACK发送失败自动重试一次

```c
sent_ok = Send_Data_Raw(MQTT_PUB_ACK, ack_out);
if (!sent_ok) {
    R_BSP_SoftwareDelay(20, BSP_DELAY_UNITS_MILLISECONDS);
    sent_ok = Send_Data_Raw(MQTT_PUB_ACK, ack_out);
}
```

3) cmdId 兼容解析
- 顶层无 `cmdId` 时，回退查 `payload.cmdId`。

4) 发送路径错误可观测
- 打印 `[ACK] send failed ...`
- 打印 `[TX] RAW ... timeout` 系列日志

### 为什么这么改
- 以前“发送失败但上层不知”是导致前端卡命令的关键原因。
- 现在 ACK 发送成功/失败是显式可见、可重试、可定位的。

---

## 4.7 性能打点（便于验证）

### 修改位置
- `src/cJSON_handle/cJSON_handle.c`
- `src/Appliance identification algorithm/appliance_identification.c`

### 打点内容
- 命令到 ACK 总耗时：`[PERF] cmd=... ack=...ms`
- 状态上报间隔：`[PERF] status upload interval=...ms`
- 后台 commit 写 SD 耗时：`[PERF] learn commit ... cost=...ms`

### 结果解读
- 1500ms 左右：心跳正常
- 200~500ms：事件触发快速上报正常
- ack 通常几十毫秒以内：命令链路健康

---

## 5. 与“历史版本（纠错+识别重命名）”的关系

历史版本已经有：
- Unknown -> pendingId -> LEARN_COMMIT
- CORRECT/RELEARN_REPLUG

本次不是推翻，而是“时序优化升级”：

1. 功能层面保持兼容
- 识别、纠错、重命名流程保持原语义。

2. 执行层面从“阻塞串行”升级为“非阻塞任务化”
- 关键慢路径（replug、commit）后台化。

3. 协议层可观测性增强
- ACK 结果更稳定可追踪。
- 上报节奏更贴近用户操作。

---

## 6. 当前参数与建议

已落地参数：
- `HEARTBEAT_MS = 1500`
- `UPLOAD_MIN_GAP_MS = 200`
- `POWER_UPLOAD_DELTA_W = 10W`
- 低功耗日志节流：3s

建议：
- 若网络较差，可将 `UPLOAD_MIN_GAP_MS` 提到 250~300ms。
- 若前端仍偶发跳变，优先配合前端做 pending 状态机（见 `docs/Web_State_Flicker_Fix_Plan.md`）。

---

## 7. 验收清单（你可以直接照着测）

1. 连续 ON/OFF 快速点击
- 观察 `sent=1` 是否稳定
- 观察前端是否不再卡住“上一条命令未完成”

2. 心跳与事件上报
- 心跳是否 ~1500ms
- 操作后是否出现 200~500ms 的快速上报

3. RELEARN_REPLUG
- 触发时主循环日志是否持续输出（说明无阻塞）

4. LEARN_COMMIT
- 下发后 ACK 快速返回
- 后台打印 commit 耗时日志

---

## 8. 一句话总结

我们没有改“识别产品逻辑”，而是把它从“能用”提升到“响应快、不卡顿、可观测、可定位问题”的工程级实现。
