# 后端 MQTT 收发协议（中文）

本文档描述当前后端实现中的 MQTT Topic 与 Payload 约定。

代码依据：
- `web/back_end/app/mqtt_bridge.py`
- `web/back_end/app/main.py`
- `web/back_end/app/schemas.py`

## 1. 全局配置

- Topic 前缀：`MQTT_TOPIC_PREFIX`（默认 `dorm`）
- Broker 地址：`MQTT_HOST:MQTT_PORT`

以下示例都以前缀 `dorm` 说明。

---

## 2. 后端下发给设备（Backend -> Device）

触发方式：
- APP 调用 `POST /api/strips/{device_id}/cmd` 后，后端发布 MQTT 命令。

### 2.1 下发 Topic

后端会发布到 1 个或 2 个命令 Topic：

1. 主 Topic（一定会发）：
- `dorm/{deviceId}/cmd`

2. 分段 Topic（仅当 `deviceId` 中包含一个空格，例如 `A-303 strip01`）：
- `dorm/{room}/{device}/cmd`

示例（`deviceId = "A-303 strip01"`）：
- `dorm/A-303 strip01/cmd`
- `dorm/A-303/strip01/cmd`

### 2.2 通用命令 Payload

所有命令都包含以下字段：

```json
{
  "cmdId": "cmd_1772_ab12cd34",
  "ts": 1772000000,
  "type": "ON",
  "socketId": 1,
  "payload": {},
  "mode": null,
  "duration": null,
  "source": "web"
}
```

字段说明：
- `cmdId`：后端生成，命令唯一标识
- `ts`：后端下发时间（Unix 秒）
- `type`：由 HTTP 的 `action` 转大写得到
- `socketId`：来自 HTTP 请求中的 `socket`（可为空）
- `payload`：来自 HTTP 请求中的 `payload`
- `mode`：来自 HTTP 请求中的 `mode`（可为空）
- `duration`：来自 HTTP 请求中的 `duration`（可为空）
- `source`：固定 `"web"`

### 2.3 LEARN_COMMIT 的特殊处理

当 `action = "learn_commit"`（即 `type = "LEARN_COMMIT"`）时，后端会把下面两个字段从 `payload` 提升到顶层：

- `payload.pendingId` -> 顶层 `pendingId`
- `payload.name` -> 顶层 `name`

示例：

```json
{
  "cmdId": "cmd_1772_ab12cd34",
  "ts": 1772000000,
  "type": "LEARN_COMMIT",
  "socketId": 1,
  "pendingId": 1024,
  "name": "Reading_Lamp",
  "payload": {
    "pendingId": 1024,
    "name": "Reading_Lamp"
  },
  "mode": null,
  "duration": null,
  "source": "web"
}
```

### 2.4 单一纠错命令（当前前端）

当前前端使用单一纠错命令：
- `action = "correct"` -> 下发 `type = "CORRECT"`

示例：

```json
{
  "cmdId": "cmd_1772_ef56gh78",
  "ts": 1772000100,
  "type": "CORRECT",
  "socketId": 1,
  "payload": {},
  "mode": null,
  "duration": null,
  "source": "web"
}
```

---

## 3. 后端接收设备上报（Device -> Backend）

后端会同时订阅一段式与两段式设备路径：

- `dorm/+/status`
- `dorm/+/telemetry`
- `dorm/+/ack`
- `dorm/+/event`
- `dorm/+/+/status`
- `dorm/+/+/telemetry`
- `dorm/+/+/ack`
- `dorm/+/+/event`

### 3.1 Topic 解析规则

去掉前缀后，尾段必须是：
- `status` / `telemetry` / `ack` / `event`

设备 ID 规则：
- 只有一段设备标识：`device_id = token`
- 两段设备标识：`device_id = "{part1} {part2}"`

所以两种都支持：
- `dorm/strip01/status` -> `device_id = "strip01"`
- `dorm/A-303/strip01/status` -> `device_id = "A-303 strip01"`

---

## 4. 上报消息格式（后端接收）

## 4.1 status

Topic：
- `dorm/{deviceId}/status` 或 `dorm/{room}/{device}/status`

推荐 Payload：

```json
{
  "ts": 1772000000,
  "online": true,
  "total_power_w": 128.6,
  "voltage_v": 220.9,
  "current_a": 0.58,
  "sockets": [
    { "id": 1, "on": true,  "power_w": 82.0, "device": "PC" },
    { "id": 2, "on": true,  "power_w": 46.6, "device": "Unknown", "pendingId": 1024 },
    { "id": 3, "on": false, "power_w": 0.0,  "device": "None" },
    { "id": 4, "on": false, "power_w": 0.0,  "device": "None" }
  ]
}
```

后端行为：
- 更新设备状态快照
- 同时写入一条 telemetry 历史点
- 广播 WS 事件 `DEVICE_STATUS`
- 存储时使用“后端接收时间”作为状态时间戳（不是设备 ts）

`sockets[]` 可用字段（按当前 schema）：
- `id`（必填，int）
- `on`（必填，bool）
- `power_w`（可选，float，默认 `0.0`）
- `device`（可选，string，默认 `"Unknown"`）
- `pendingId`（可选，int）

## 4.2 telemetry

Topic：
- `dorm/{deviceId}/telemetry` 或 `dorm/{room}/{device}/telemetry`

推荐 Payload：

```json
{
  "ts": 1772000001,
  "power_w": 126.3,
  "voltage_v": 221.0,
  "current_a": 0.57
}
```

后端行为：
- 写入 telemetry 历史点
- 同步更新状态快照中的 `total_power_w / voltage_v / current_a`
- 广播 WS 事件 `TELEMETRY`
- 存储时使用“后端接收时间”作为 telemetry 时间戳

## 4.3 ack

Topic：
- `dorm/{deviceId}/ack` 或 `dorm/{room}/{device}/ack`

Payload：

```json
{
  "cmdId": "cmd_1772_ab12cd34",
  "status": "success",
  "costMs": 120,
  "errorMsg": ""
}
```

字段说明：
- `cmdId`：命令关联键（必须）
- `status`：后端仅把 `"success"` 视为成功，其他值都按失败处理
- `costMs`：可选，耗时毫秒
- `errorMsg`：可选，失败原因文本

后端行为：
- 更新命令状态（`success` / `failed`）
- 若成功且命令为 ON/OFF，会本地更新插孔开关状态
- 广播 WS 事件 `CMD_ACK`

## 4.4 event

Topic：
- `dorm/{deviceId}/event` 或 `dorm/{room}/{device}/event`

当前后端行为：
- 会订阅并解析该 Topic
- 但当前业务逻辑中 **不落库、不广播、不做处理**

---

## 5. 在线/离线判定说明

- 设备在线状态采用超时判定：
  - `ONLINE_TIMEOUT_SECONDS`
- 在该时长内没收到 status/telemetry，设备会标记为离线
- 当前没有单独的 `DEVICE_OFFLINE` WebSocket 事件

---

## 6. 最小联调步骤（建议）

1) 先上报 status（包含 `Unknown + pendingId`）：
- Topic: `dorm/A-306/strip01/status`

2) 调用 `LEARN_COMMIT`：
- `POST /api/strips/A-306%20strip01/cmd`

```json
{
  "socket": 1,
  "action": "learn_commit",
  "payload": { "pendingId": 1024, "name": "Reading_Lamp" }
}
```

3) 订阅命令 Topic，确认收到顶层 `pendingId/name`：
- `dorm/A-306/strip01/cmd`

4) 设备回 ACK：
- Topic: `dorm/A-306/strip01/ack`

```json
{
  "cmdId": "cmd_...",
  "status": "success",
  "costMs": 100
}
```

