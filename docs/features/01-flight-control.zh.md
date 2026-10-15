# 飞行控制 (Flight Control)

> 能力：响应机库的起飞 / 降落 / 异常恢复服务调用，并向机库推送 `0 → 100` 进度。
> 飞机在地面站侧的各飞行阶段就是通过它与你的机库协同的。

| | |
|---|---|
| **设备** | dt5（第三方机库） |
| **方向** | 下行——机库下发 service，你回复并推送进度事件 |
| **头文件** | `edge_dock/flight.h` |

---

## 1. 功能概述

飞行控制是 **server 角色**：由 O4 机库驱动。五个流程都遵循同样的三段式：

1. **service**（机库 → 你）——机库调用某个 service（如 `airport_takeoff_prepare`）。
2. **services_reply**（你 → 机库）——SDK 已注册的 handler 回复 `in_progress`。
3. **events**（你 → 机库）——你推送 `percent` 进度事件，直到该阶段完成。

五个流程：`takeoff_prepare`、`takeoff_end`、`landing_prepare`、`landing_end`、`abnormal_recovery`。
`flight_create()` 一次性注册全部五个 service handler；你只需驱动进度推送。

---

## 2. 快速示例

```c
#include "edge_dock/edge_dock.h"

/* 已有有效会话 `s` 且已上报 dt5 能力 */
Flight_t *fl = flight_create(s, dock_session_account(s));   /* 注册 5 个 service handler */

/* 起飞准备阶段运行时，推送 0 -> 100 进度 */
flight_push_takeoff_prepare(fl);

flight_destroy(fl);
```

---

## 3. API 参考

### `Flight_t *flight_create(ThingModelClientSession_t *session, const char *account)`
创建飞行控制对象，并在 `session` 上注册五个 service handler。`account` 是会话账号 / topic 属主
（`dock_session_account(s)`），不是原始 `device_sn`。机库下发对应 service 时，handler 会自动
回复 `in_progress`。失败返回 `NULL`。

### `void flight_destroy(Flight_t *f)`
注销 handler 并释放对象。

### `void flight_push_takeoff_prepare(Flight_t *f)` 及同族
便捷推送器，为某个流程自动扫过 `0 → 100` 的进度事件流：
- `flight_push_takeoff_prepare(f)`
- `flight_push_takeoff_end(f)`
- `flight_push_landing_prepare(f)`
- `flight_push_landing_end(f)`
- `flight_push_abnormal_recovery(f)`

### `void flight_push_single(Flight_t *f, int action, const char *status, int percent)`
推送**单条**自定义进度事件，用于对单步精细控制。
- `action`：`0`=takeoff_prepare、`1`=takeoff_end、`2`=landing_prepare、`3`=landing_end、
  `4`=abnormal_recovery。
- `status`：schema 枚举——`canceled` / `failed` / `in_progress` / `ok` / `paused` /
  `rejected` / `sent` / `timeout` 之一。
- `percent`：`0`–`100`。（上面的 `flight_push_*` 便捷函数就是反复调用它来扫 0→100。）

### `void flight_selftest_inject_services(Flight_t *f)`
仅自测：把五个 service 请求注入本地订阅的 `services` 主题以触发 handler。**只在本地 mock broker 有效**
——真机上由机库驱动 service。

---

## 4. 交互时序

```
  O4 机库                         edge-sdk（你的 handler）              你的应用
    │  service: airport_takeoff_prepare ──▶│                              │
    │◀── services_reply: in_progress ──────│  （由 SDK 自动回复）          │
    │                                       │◀── flight_push_takeoff_prepare(fl)
    │◀── event: percent=0 ──────────────────│                              │
    │◀── event: percent=... ────────────────│                              │
    │◀── event: percent=100 ────────────────│                              │
```

---

## 5. 数据结构

无公开结构体——`Flight_t` 是不透明句柄。你通过 `flight_push_single` 传入的线级枚举：

| 参数 | 取值 |
|---|---|
| `action` | `0` takeoff_prepare · `1` takeoff_end · `2` landing_prepare · `3` landing_end · `4` abnormal_recovery |
| `status` | `canceled` · `failed` · `in_progress` · `ok` · `paused` · `rejected` · `sent` · `timeout` |
| `percent` | `0`–`100` |

---

## 6. 错误处理

- 若 handler 注册失败（通常是会话无效/已关闭），`flight_create()` 返回 `NULL`。
- 进度推送器返回 `void`——尽力发布；某次发布丢失会在扫描的下一条事件时补上。
- 及时回复 service。SDK 会代你自动回 `in_progress`；当你需要非 `ok` 结果时，用带终态 `status`
  （`ok` / `failed` / `rejected`）的 `flight_push_single` 显式收尾某阶段。
