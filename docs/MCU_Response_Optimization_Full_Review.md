# MCU与网页响应优化全流程回顾（完整学习版）

> 这份文档从你提出“重新阅读代码结构和执行流程”之后开始，按真实开发过程复盘：
> - 我们每一步在解决什么问题
> - 怎么定位问题（看哪些日志）
> - 改了哪些代码（文件/函数）
> - 为什么这样改
> - 最终效果如何

---

## 0. 初始背景：为什么会“状态慢、偶发卡住”

你当时的核心痛点有两个：

1. **命令响应不稳定**
- 网页下发 ON/OFF，单片机能执行，但偶发不回 ACK。
- 网页因为“上一条命令未完成”被锁住，后续不能操作。

2. **状态更新不够及时**
- 固定 3 秒上报，用户操作后网页反馈慢。
- 前端还会遇到状态跳变（乐观更新后又被旧状态包覆盖）。

---

## 1. 第一步：重新梳理系统执行链路（先看结构，不盲改）

我们先做的是“读代码结构”，明确时序主链：

1. `hal_entry.c` 主循环
- `handle_uart_json_stream()` 处理云端命令
- 周期上报 `upload_strip_status()`
- BL0942 数据到达后 `Data_Processing()`

2. `uart_hlw.c` 电参处理
- 计算功率/电流/电压
- 事件检测触发 AI 采样
- 低功率分流

3. `event_detector.c` 事件检测
- baseline + 去抖 + 迟滞 + 冷却

4. `appliance_identification.c` AI 状态机
- `IDLE -> SAMPLING -> READY -> LOCKED`
- unknown 进 pending，commit 后落库

5. `cJSON_handle.c` 命令解析与 ACK/status 上报

### 这一步解决了什么
- 避免“头痛医头”：先知道到底是命令链路慢、上报慢，还是发送层丢包。

---

## 2. 第二步：把“阻塞操作”改成“非阻塞任务推进”

## 2.1 问题

`RELEARN_REPLUG` 里有 `R_BSP_SoftwareDelay()`，会卡主循环：

```c
R_BSP_SoftwareDelay(...)
```

主循环被卡住时：
- ACK 变慢
- 状态上报变慢
- 串口 JSON 处理也会滞后

## 2.2 观察方法（日志）

初期通过：
- 命令下发后 ACK 时间波动大
- 某些阶段串口输出“停顿”

## 2.3 修改方案

把 `RELEARN_REPLUG` 拆成状态机，不等待，只记录“下一步时间点”。

### 关键代码（`appliance_identification.c`）

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
    // 只做启动，不阻塞等待
    Relay_Set_OFF(index);
    g_strip.sockets[index].on = false;

    g_replug[index].state = REPLUG_WAIT_OFF;
    g_replug[index].deadline_tick = HAL_GetTick() + AI_REPLUG_OFF_MS;
    request_status_upload();
}
```

```c
void AI_Replug_Task(void)
{
    // 主循环每轮推进一步
    if (state == REPLUG_WAIT_OFF && now >= deadline) {
        Relay_Set_ON(i);
        state = REPLUG_WAIT_ON_SETTLE;
        deadline = now + AI_REPLUG_ON_SETTLE_MS;
    }
    else if (state == REPLUG_WAIT_ON_SETTLE && now >= deadline) {
        AI_Trigger_Sampling(i);
        state = REPLUG_IDLE;
    }
}
```

### 主循环接入（`hal_entry.c`）

```c
handle_uart_json_stream();
AI_Replug_Task();
```

## 2.4 结果
- RELEARN_REPLUG 不再卡主循环。
- 命令响应与上报明显更平滑。

---

## 3. 第三步：上报机制从“固定3s”升级为“事件+心跳+节流”

## 3.1 问题

原来主要靠固定 3 秒上报，用户操作后网页反馈慢。

## 3.2 修改方案

在 `hal_entry.c` 改为双通道：
- 心跳保底：`1500ms`
- 事件触发：`g_force_upload_flag`
- 事件最小间隔节流：`200ms`

### 关键代码

```c
#define HEARTBEAT_MS 1500U
#define UPLOAD_MIN_GAP_MS 200U

bool heartbeat_due = (now_tick - last_report >= HEARTBEAT_MS);
bool event_due = g_force_upload_flag && (now_tick - last_upload_tick >= UPLOAD_MIN_GAP_MS);
if (heartbeat_due || event_due) {
    g_force_upload_flag = 0;
    upload_strip_status();
}
```

并引入统一触发接口（`cJSON_handle.c/.h`）：

```c
void request_status_upload(void)
{
    g_force_upload_flag = 1;
}
```

## 3.3 在哪些场景触发事件上报

我们在这些逻辑点调用 `request_status_upload()`：
- 开关状态变化（`Socket_Command_Handler`）
- 进入检测态（`AI_Trigger_Sampling`）
- 识别命中/未命中（`AI_Recognition_Engine`）
- pending commit 成功
- 功率变化超过阈值（见下一节）

## 3.4 结果
- 心跳稳定在约 1500ms。
- 操作后常见 200~500ms 内就有新状态。

---

## 4. 第四步：功率变化触发（10W阈值）+ 低功耗日志节流

## 4.1 问题

- 不希望每帧都上报（太频繁）。
- 但又希望有明显变化时尽快上报。

## 4.2 修改（`uart_hlw.c`）

```c
#define POWER_UPLOAD_DELTA_W 10.0f

float p_prev = g_strip.sockets[index].power;
g_strip.sockets[index].power = p_used;
if (fabsf(p_used - p_prev) >= POWER_UPLOAD_DELTA_W)
{
    request_status_upload();
}
```

低功耗日志从 1s 放宽到 3s，减少刷屏：

```c
if (now_tick - g_lowpower_log_tick[index] >= 3000U)
```

## 4.3 结果
- 上报既快又不刷屏。
- 日志更利于观察核心时序。

---

## 5. 第五步：LEARN_COMMIT 改为“快ACK + 后台写SD”

## 5.1 问题

`LEARN_COMMIT` 若直接写 SD，命令路径会被慢 IO 拉长，ACK 容易超时。

## 5.2 修改方案

新增“受理队列 + 后台任务”：

### 头文件接口（`appliance_identification.h`）

```c
bool AI_Request_Commit_Pending(uint8_t index, uint32_t pending_id, const char *name);
void AI_Commit_Task(void);
```

### 命令线程快速受理（`cJSON_handle.c`）

```c
bool accepted = AI_Request_Commit_Pending(...);
if (accepted) ok = true;
else fail_reason = "commit_rejected";
```

### 主循环后台执行（`hal_entry.c`）

```c
AI_Commit_Task();
```

### 后台真正写库（`appliance_identification.c`）

```c
FRESULT res = AI_Commit_Pending(i, g_commit_task[i].pending_id, g_commit_task[i].name);
```

## 5.3 结果
- 命令链路更轻，ACK 更快。
- SD 写入不再阻塞命令处理。

---

## 6. 第六步：性能打点日志（定位问题的核心手段）

这是这次优化最重要的“方法论”。

## 6.1 打了哪些日志

1. 命令到ACK总耗时（`cJSON_handle.c`）

```c
printf("[PERF] cmd=%s ack=%lu ms result=success sent=%d\r\n", ...)
```

2. 状态上报间隔（`upload_strip_status`）

```c
printf("[PERF] status upload interval=%lu ms\r\n", ...)
```

3. 后台 commit 写 SD 耗时（`appliance_identification.c`）

```c
printf("[PERF] learn commit socket=%u cost=%lu ms result=ok\r\n", ...)
```

## 6.2 这些日志如何读

- `status interval ~1500ms`：心跳正常
- `status interval ~200~500ms`：事件触发快速上报正常
- `ack xx ms sent=1`：ACK 发送成功
- `sent=0` + TX timeout：发送链路问题

---

## 7. 第七步：关键漏点补齐——ESP8266发送链路优化（你提醒得非常对）

你这次指出的非常关键：
> 之前总结没有完整体现 `bsp_wifi_esp8266.c` 的优化。

这里是完整补充。

## 7.1 当时出现的问题

日志里大量出现：

- `[TX] RAW publish timeout topic=.../status`
- `[ACK] send failed ...`

说明：
- 不是命令分支逻辑错，而是**发送层**在超时。

## 7.2 观察与定位

我们发现 `Send_Data_Raw` 里只清空了 `At_Rx_Buff`，但没有重置 `Uart2_Num`。

后果：
- 回调里写入 `At_Rx_Buff` 的索引越来越大，接近上限后新字符无法正常写入。
- 后续 `OK` 很难被捕获，导致持续超时。

## 7.3 具体修改（`bsp_wifi_esp8266.c/.h`）

1) `Send_Data_Raw` 返回 `bool`

```c
bool Send_Data_Raw(char *topics, char *data)
```

2) 明确失败分支并打印原因

```c
printf("[TX] RAW prompt timeout ...");
printf("[TX] RAW publish timeout ...");
```

3) 关键修复：统一使用 `Clear_Buff()`

```c
Clear_Buff();
```

> `Clear_Buff()` 不仅清内容，也重置 `Uart2_Num`，这点是核心。

4) 不再“盲目吞环形缓冲区”
- 避免影响后续命令流处理。

5) ACK发送失败自动重试一次（`cJSON_handle.c`）

```c
sent_ok = Send_Data_Raw(...);
if (!sent_ok) {
    R_BSP_SoftwareDelay(20, ...);
    sent_ok = Send_Data_Raw(...);
}
```

## 7.4 结果（从你的日志看）

你最终日志已经很说明问题：
- 大量 `RAW发送成功`
- `cmd=ON/OFF ... sent=1` 稳定出现
- ACK 卡死问题基本消失

这说明发送层修复是有效的。

---

## 8. 第八步：ACK统计口径修正

你后来看到 `ack=0ms`，我们又做了修正：
- 改成“命令处理到 send_ack 返回后的总耗时”
- 并设置下限 `1ms`

```c
uint32_t ack_cost_total = HAL_GetTick() - cmd_start_tick;
int ack_cost_ms = (ack_cost_total == 0U) ? 1 : (int)ack_cost_total;
```

这样日志解释更准确。

---

## 9. 你最关心的“网页状态跳变”结论

这个问题的最终根因是“命令流/状态流乱序”，不是 MCU 单侧可完全解决。

已给出前后端协同文档：
- `docs/Web_State_Flicker_Fix_Plan.md`

核心措施：
- 前端 pending 状态机（必须）
- 后端 cmdId 关联与去重
- MCU 保持及时上报 + 可观测日志

---

## 10. 本次学习可以沉淀的方法（最重要）

你这次其实已经掌握了工程级处理思路：

1. **先画链路再动代码**
- 主循环、命令链路、发送链路、状态上报链路分开看。

2. **先加观测点再优化**
- 没日志就没证据。
- `[PERF]` / `[TX]` / `[ACK]` 是这次成功关键。

3. **优先去阻塞**
- 延时和慢IO不要放在命令快路径。

4. **把“偶发问题”变成“可重复、可定位”**
- 通过日志把“偶发不回ACK”定位到发送层。

5. **最后再调参数**
- 先正确，再快。

---

## 11. 当前版本状态（结论）

截至你最新日志，这个版本已经达到“可实用”状态：

- MCU 命令响应快
- ACK 基本稳定
- 网页状态更新及时（心跳+事件）
- 识别/纠错/重命名流程保持可用
- 关键链路可观测、可定位问题

如果后续要继续精进，下一步方向是：
- 前端 pending 状态机落地
- status 增加 `stateVersion`（抗乱序）
- 根据现场网络质量微调上报节流参数

---

## 12. 相关文档索引

- `docs/Optimize web page response.md`
- `docs/MCU_Response_Optimization_Implementation_Summary.md`
- `docs/Web_State_Flicker_Fix_Plan.md`
