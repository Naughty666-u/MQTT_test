# MCU-Web 协议与代码映射说明（Shadow Learning 版）

> 目标：让单片机端和网页端都能明确知道“发什么、收什么、每条指令做什么、对应哪段代码”。

---

## 1. 主题（Topic）约定

定义位置：`src/bsp_esp8266/bsp_wifi_esp8266.h`

- 下行命令（网页 -> MCU）
  - `dorm/{ROOM_ID}/{DEVICE_NAME}/cmd`
  - 宏：`MQTT_SUB_TOPIC`（见 `bsp_wifi_esp8266.h:35`）
- 上行状态（MCU -> 网页）
  - `dorm/{ROOM_ID}/{DEVICE_NAME}/status`
  - 宏：`MQTT_PUB_STATUS`（见 `bsp_wifi_esp8266.h:38`）
- 回执 ACK（MCU -> 网页）
  - `dorm/{ROOM_ID}/{DEVICE_NAME}/ack`
  - 宏：`MQTT_PUB_ACK`（见 `bsp_wifi_esp8266.h:41`）

---

## 2. 上行状态协议（status）

生成位置：`src/cJSON_handle/cJSON_handle.c` -> `upload_strip_status()`（见 `:63`）

## 2.1 JSON 结构

```json
{
  "ts": 123456789,
  "online": true,
  "total_power_w": 128.6,
  "voltage_v": 220.1,
  "current_a": 0.58,
  "sockets": [
    {
      "id": 1,
      "on": true,
      "power_w": 42.3,
      "device": "DeskLamp"
    },
    {
      "id": 2,
      "on": true,
      "power_w": 38.7,
      "device": "Unknown",
      "pendingId": 1024
    },
    {
      "id": 3,
      "on": false,
      "power_w": 0.0,
      "device": "None"
    },
    {
      "id": 4,
      "on": true,
      "power_w": 2.1,
      "device": "LowPower"
    }
  ]
}
```

## 2.2 字段说明

- `ts`：当前 tick（`HAL_GetTick()`）
- `online`：固定 `true`
- `total_power_w / voltage_v / current_a`：整排插统计值
- `sockets[i].id`：1..4
- `sockets[i].on`：该插孔是否通电
- `sockets[i].power_w`：该插孔功率
- `sockets[i].device`：当前识别结果（可能为 `None/LowPower/Unknown/已识别名`）
- `sockets[i].pendingId`（可选）：仅当 `device=="Unknown"` 且 pending 有效时上报（见 `cJSON_handle.c:85`）

## 2.3 网页端接收建议

- 若 `device != "Unknown"`：正常展示，不显示命名入口
- 若 `device == "Unknown"` 且存在 `pendingId`：显示“命名并加入设备库”输入框
- 建议前端按 `socket.id` 维护每路状态机

---

## 3. 下行命令协议（cmd）

入口函数：`src/cJSON_handle/cJSON_handle.c` -> `process_cloud_cmd()`（见 `:118`）

所有命令推荐都带 `cmdId`（字符串）。

- 原因：ACK 需要 `cmdId` 关联请求；如果你不带 `cmdId`，MCU 侧不会发有效 ACK 给你。

## 3.1 开关控制

### ON

```json
{"cmdId":"c1001","type":"ON","socketId":1}
```

作用：闭合指定插孔继电器并开始新的识别事务。

代码路径：

1. `process_cloud_cmd()` 识别 `type=ON`（`cJSON_handle.c:161`）
2. 调 `Socket_Command_Handler(index,true)`（`cJSON_handle.c:169`）
3. 进入继电器控制 + AI reset（`appliance_identification.c:129`）
4. 置 `g_force_upload_flag=1`，触发下一轮立即上报（`cJSON_handle.c:233`）

### OFF

```json
{"cmdId":"c1002","type":"OFF","socketId":1}
```

作用：断开继电器，清理该插孔识别锁和 pending 缓存。

代码路径同 ON，对应 `Socket_Command_Handler(index,false)`。

## 3.2 Unknown 样本命名提交（核心）

### LEARN_COMMIT

```json
{
  "cmdId":"c2001",
  "type":"LEARN_COMMIT",
  "socketId":1,
  "pendingId":1024,
  "name":"Reading_Lamp"
}
```

作用：把该插孔 pending 样本写入 SD 数据库，并把 `device` 更新为 `name`。

代码路径：

1. `process_cloud_cmd()` 读取 `pendingId/name`（`cJSON_handle.c:173`）
2. 调 `AI_Commit_Pending(index,pendingId,name)`（`cJSON_handle.c:189`）
3. `AI_Commit_Pending()` 做严格校验（`appliance_identification.c:167`）
4. 调 `Save_Appliance_Data()` 写入 `Device.csv`
5. 成功后清 pending + 更新设备名

### 重要：同名/自增命名分支当前行为

当前实现并没有“自动重命名为 _1/_2”的分支。

- 位置：`src/sdcard_data_handle/sdcard_data_handle.c` -> `Save_Appliance_Data()`
- 行为：如果同名已存在，函数直接“跳过写入并返回 FR_OK”
- 结果：`LEARN_COMMIT` 仍会走 success，设备名会更新，但数据库不会新增新行

这就是你提到的“自增命名分支说明”现状。

建议网页端改造：

1. 命名提交前先做名称唯一性提示（可选）
2. 或者增加一个后端策略：自动追加 `_1/_2/...` 后再提交
3. 或 MCU 端后续加“自动重命名”逻辑（当前版本未实现）

## 3.3 纠错命令（无需命名）

### RECOG_RETRY（不掉电重识别）

```json
{"cmdId":"c3001","type":"RECOG_RETRY","socketId":1}
```

作用：清 pending，复位 AI 状态，重新进入采样识别；不切继电器。

代码：

- `cJSON_handle.c:200` -> `AI_Request_Recog_Retry()`
- `appliance_identification.c:189`

### RELEARN_REPLUG（软件插拔重学习）

```json
{"cmdId":"c3002","type":"RELEARN_REPLUG","socketId":1}
```

作用：执行 OFF->delay->ON，抓更干净的启动暂态，再触发采样识别。

代码：

- `cJSON_handle.c:212` -> `AI_Request_Relearn_Replug()`
- `appliance_identification.c:200`

---

## 4. ACK 协议（ack）

生成函数：`send_ack()`（`cJSON_handle.c:31`）

## 4.1 成功 ACK

```json
{"cmdId":"c1001","status":"success","costMs":50}
```

## 4.2 失败 ACK

```json
{"cmdId":"c2001","status":"fail","costMs":50,"reason":"commit_failed"}
```

## 4.3 reason 取值（当前实现）

- `missing_type`
- `bad_socket`
- `missing_field`
- `commit_failed`
- `unsupported_type`
- `bad_request`

网页端应按 `cmdId` 关联请求，再根据 `status/reason` 做提示。

---

## 5. 端到端时序（推荐网页流程）

## 5.1 自动识别主流程

1. 网页下发 `ON`
2. MCU 通电 + 事件触发 + 采样识别
3. 若命中：`status.device=已识别名`
4. 若未命中：`status.device="Unknown"` 且 `pendingId` 出现

## 5.2 Unknown 命名流程

1. 网页看到 `Unknown + pendingId`
2. 弹出输入框，用户输入 `name`
3. 网页下发 `LEARN_COMMIT`
4. MCU ACK `success`
5. 下一帧 `status` 显示新 `device=name`

## 5.3 纠错流程

- 快速重试：发 `RECOG_RETRY`
- 高质量重采：发 `RELEARN_REPLUG`

---

## 6. 代码映射总表（命令 -> MCU函数）

| 指令 type | MCU 入口 | 核心处理函数 | 主要副作用 |
|---|---|---|---|
| ON | `process_cloud_cmd` | `Socket_Command_Handler(index,true)` | 合闸、清 pending、触发上报 |
| OFF | `process_cloud_cmd` | `Socket_Command_Handler(index,false)` | 断闸、清 pending、触发上报 |
| LEARN_COMMIT | `process_cloud_cmd` | `AI_Commit_Pending` | pending 落库、更新设备名 |
| RECOG_RETRY | `process_cloud_cmd` | `AI_Request_Recog_Retry` | 不掉电重采样识别 |
| RELEARN_REPLUG | `process_cloud_cmd` | `AI_Request_Relearn_Replug` | 软件插拔后重采样识别 |

---

## 7. 当前实现边界（务必同步给前端）

1. 继电器目前仅 index=0（插孔1）有真实引脚映射；2~4 路是占位。
2. `hal_entry.c` 中 BL0942 解析当前固定传 `index=0`，多路电参还未打通。
3. `LEARN_COMMIT` 同名不会自动自增，当前是“跳过写入但返回成功”。
4. ACK 依赖 `cmdId`；前端如果不带 `cmdId`，无法可靠做请求-回执匹配。

---

## 8. 你可以直接给网页同学的最小清单

1. 订阅 `status`，渲染 `sockets[]`。
2. 订阅 `ack`，按 `cmdId` 做请求结果提示。
3. 发布 `cmd`，至少支持 5 个 type：`ON/OFF/LEARN_COMMIT/RECOG_RETRY/RELEARN_REPLUG`。
4. 当 `device=="Unknown"` 且有 `pendingId` 时，显示命名入口并调用 `LEARN_COMMIT`。
5. 所有命令都必须带 `cmdId`。
