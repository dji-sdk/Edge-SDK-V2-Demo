# 充电与供电 (Charging & Power)

> 能力：在机库内给飞机上电/下电、开始/停止充电。四个简单的请求/回复操作，由第三方主动调用机库。

| | |
|---|---|
| **设备** | dt5（第三方机库） |
| **方向** | 上行——第三方调用机库（`requests` / `requests_reply`） |
| **头文件** | `edge_dock/charging.h` |

---

## 1. 功能概述

四个操作，两对：

| 操作 | API | 含义 |
|---|---|---|
| 飞机上电 | `charging_drone_open()` | 给机库内飞机**上电** |
| 飞机下电 | `charging_drone_close()` | 给机库内飞机**下电** |
| 开始充电 | `charging_charge_open()` | 开始充电 |
| 停止充电 | `charging_charge_close()` | 停止充电 |

每次调用都是一次 `requests` → `requests_reply` 往返；回复带 `status`。

> 说明：这里的"供电"是**飞机**的开关机（`drone_open`/`drone_close`），不是舱盖——本能力里机库没有舱盖操作。

---

## 2. 快速示例

```c
#include "edge_dock/edge_dock.h"

/* 已有有效会话 `s` */
Charging_t *ch = charging_create(s);

charging_drone_open(ch);      /* 飞机上电 */
charging_charge_open(ch);     /* 开始充电 */
/* ... */
charging_charge_close(ch);    /* 停止充电 */
charging_drone_close(ch);     /* 飞机下电 */

charging_destroy(ch);
```

---

## 3. API 参考

### `Charging_t *charging_create(ThingModelClientSession_t *session)`
创建绑定到 `session` 的充电对象。失败返回 `NULL`。

### `void charging_destroy(Charging_t *c)`
释放对象。

### `int charging_drone_open(Charging_t *c)` / `int charging_drone_close(Charging_t *c)`
飞机上电 / 下电。确认成功返回 `0`，否则 `<0`。

### `int charging_charge_open(Charging_t *c)` / `int charging_charge_close(Charging_t *c)`
开始 / 停止充电。确认成功返回 `0`，否则 `<0`。

---

## 4. 交互时序

```
  你的应用            edge-sdk                 O4 机库
    │ charging_charge_open(ch) ─▶│                 │
    │                            │ request: charge_open ─▶│
    │                            │◀── requests_reply: {result, output:{status}}
    │◀── 0 (或 <0) ──────────────│                 │
```

---

## 5. 数据结构

`Charging_t` 不透明；四个操作均无参数。机库回复含 `status` 字段（充电/供电状态），通过返回码体现。

---

## 6. 错误处理

- 会话无效/已关闭时 `charging_create()` 返回 `NULL`。
- 只有机库回复成功时各操作才返回 `0`；返回 `<0` 表示请求失败或回复 `result` 非零。请合理编排上电/充电
  顺序（如先上电再充电），并在下一步前检查每次返回。
