# 核心概念

先建个心智模型，后面看 API 就顺了。五个概念覆盖全部。

## 1. 会话（Session）

所有东西都走**会话**——一条连到 O4 机库 broker 的、已鉴权的 MQTT 连接。`dock_session_connect()`
给你一个 `ThingModelClientSession_t *`，这个句柄是每个能力对象（`flight_create`、`charging_create`……）
的第一个参数。没会话，就没能力，就这么简单。

## 2. 设备类型：dt5 vs dt7

你的设备靠"能力上报"声明*自己是啥*：

- **dt5** —— 第三方机库（`device_type=5`）：飞行控制、充电、配置。
- **dt7** —— RID 设备（`device_type=7`）：监视（RID / ADS-B）。

这步是前置：某类型的能力，得先上报了那个类型，机库才认。

## 3. 上行 vs 下行

两个方向，决定"谁先开口"：

- **上行**（你 → 机库）——*你*主动调机库：充电、配置、物模型参数。像同步的请求/回复。
- **下行**（机库 → 你）——*机库*来调你：飞行控制。你挂好 handler，负责回复。

搞清方向，你就知道这件事是你去调、还是等别人调你。

## 4. service / reply / event

下行流程（飞行）分三段：

| 段 | 谁 | 含义 |
|---|---|---|
| **service** | 机库 → 你 | 机库让你干件事 |
| **services_reply** | 你 → 机库 | 你应答（SDK 自动回 `in_progress`） |
| **event** | 你 → 机库 | 你推进度（`percent` 0→100）直到完成 |

所以一次起飞不是一条消息——是你先接下一个 service，再推一串进度事件上去。

## 5. 信封：bid / tid

每条消息都裹在一个信封里，带 `bid`（业务/能力 id）和 `tid`（单次调用的追踪 id）。回复靠 `tid` 跟请求配对
——一条请求和它的回复 `tid` 对得上，就说明这一来一回成了。这俩 SDK 会替你生成（libuuid 出 UUIDv4，
或退回 `SN + 计数器`），平时你基本不用碰，但**排查线上问题时，第一个要看的就是它**。

---

有了这五个，功能文档就能直接读：挑一个会话、认准设备类型、跟着方向走，飞行的话再照 service→reply→event
的节奏处理。怎么串起来看[快速上手](01-quickstart.zh.md)。

---

## 附：几种 SN，别搞混

"SN" 这个词被好几个不同的值共用，混淆它们是真机报错的头号来源（比如 broker 回「client ID 与 topic 不匹配」）。

| 概念 | SDK 里的命名 | 示例 | 出现位置 |
|---|---|---|---|
| 原始三方机库 SN（dt5 连接原料） | `device_sn` | `mqtt_test` | `dock_session_default_opts()` 从 `DOCK_DEVICE_SN` 填入——**连接前**用 |
| 原始 RID 设备 SN（dt7 连接原料） | `device_sn` | `mqtt_test_rid` | `dock_session_default_opts_rid()` 从 `DOCK_RID_DEVICE_SN` 填入 |
| 账号 = MQTT 用户名 = topic 属主段 | `dock_session_account(s)` | `195_esdk_mqtt_test` / `195_esdk_mqtt_test_rid`（`app_id"_esdk_"device_sn`） | runtime 的 `GetSn()` 返回值；**连接后**一切拼 topic / 喂给能力对象（如 `flight_create`）的都用它 |
| 机库自己的真实 SN | （不对外暴露） | — | 固件内部；三方侧永远看不到 |

**dt5 和 dt7 是网关眼里的两台设备，必须用两个不同的 SN。** 账号同时充当 MQTT client_id，
共用一个 SN 时**后连的一方**会被拒：`CONNACK rc=5 (not authorised)`。
`DOCK_RID_SN` 留空时默认派生 `<DOCK_DEVICE_SN>_rid`。

口诀：**`device_sn` 只在连接前存在；连上之后，一切与 topic 有关的都用 `dock_session_account(s)`。**
注意 wire 协议本身把 *account* 值塞进了名为 `device_sn` 的字段——这个 overload 是从协议层继承的，
SDK 层改不了名，读 wire 日志时要留个心眼。

