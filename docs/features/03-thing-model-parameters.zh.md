# 物模型参数 (Thing-Model Parameters)

> 能力：读取机库的物模型（TSL）属性，并用"累积—提交"模式推送属性更新。这是机库的实时状态面
> ——工作模式、返航点、电池、充电器状态等。

| | |
|---|---|
| **设备** | dt5（第三方机库） |
| **方向** | 上行——第三方调用机库（`get_tsl_params` / `set_tsl_params`） |
| **头文件** | `edge_dock/config.h` |

---

## 1. 功能概述

两个方向：

- **读**——`dock_config_get_tsl_params()` 拉取当前物模型值。
- **写**——用类型安全的 `dock_config_tsl_set_*()` setter 累积一批属性变更，再用
  `dock_config_set_tsl_params_commit()` 在一条 `set_tsl_params` 消息里一次性发送。只有你 set 过的
  属性才会上线。

"累积—提交"设计让你精确推送发生变化的属性，每次 commit 一条批量消息。

---

## 2. 快速示例

```c
#include "edge_dock/edge_dock.h"

DockConfig_t *cf = dock_config_create(s);

/* 读当前值 */
dock_config_get_tsl_params(cf);

/* 暂存几个属性变更，再作为一条 set_tsl_params 提交 */
dock_config_tsl_set_home_point(cf, 113.95, 22.53, 100);  /* lon, lat, height */
dock_config_tsl_set_drone_is_open(cf, true);
dock_config_tsl_set_battery_voltage(cf, 12600.0);
dock_config_set_tsl_params_commit(cf);                   /* 发送 + 清空累积器 */

dock_config_destroy(cf);
```

---

## 3. API 参考

### `DockConfig_t *dock_config_create(ThingModelClientSession_t *session)` / `void dock_config_destroy(DockConfig_t *c)`
创建 / 释放 config 对象。（与「机库配置」能力共用。）

### `int dock_config_get_tsl_params(DockConfig_t *c)`
请求当前物模型值。收到确认回复返回 `0`，否则 `<0`。

### 暂存 setter —— `int dock_config_tsl_set_*(DockConfig_t *c, …)`
每个把一个 `{属性: 值}` 压入待提交累积器。成功返回 `0`，累积器满返回 `<0`。可用 setter：

| Setter | 值类型 |
|---|---|
| `..._home_point_longitude` / `_latitude` | `double` |
| `..._home_point_height` | `int32_t` |
| `..._alt_land_longitude` / `_latitude` | `double` |
| `..._alt_land_height` | `int32_t` |
| `..._station_work_mode` / `_third_dock_work_mode` | `int32_t` |
| `..._drone_in_dock` / `_charger_state` | `int32_t` |
| `..._drone_is_open` / `_drone_is_bt_connect` | `bool` |
| `..._battery_voltage` / `_acdc_temperature` | `double` |

便捷组合（串接几个 setter，不 commit）：
- `dock_config_tsl_set_home_point(c, lon, lat, height)`
- `dock_config_tsl_set_work_mode(c, station_mode, third_dock_mode)`

### `void dock_config_tsl_reset(DockConfig_t *c)`
清空累积器但不发送（commit 后会自动清空）。

### `int dock_config_set_tsl_params_commit(DockConfig_t *c)`
把所有暂存属性打包进一条 `set_tsl_params` 的 `data.params` 数组，发送，并清空累积器。确认成功返回
`0`，否则 `<0`。

### `int dock_config_set_tsl_params(DockConfig_t *c)`
demo 辅助：暂存几个示例属性并在一次调用里提交（供 sample 使用）。

---

## 4. 交互时序

```
  你的应用                                   edge-sdk              O4 机库
    │ dock_config_tsl_set_home_point(...)  ─▶│ (已暂存，未发送)     │
    │ dock_config_tsl_set_drone_is_open(...)─▶│ (已暂存)             │
    │ dock_config_set_tsl_params_commit()  ─▶│ set_tsl_params{params[...]} ─▶│
    │                                        │◀── requests_reply{result, output}
    │◀── 0 (或 <0) ──────────────────────────│                      │
```

---

## 5. 数据结构

`DockConfig_t` 不透明。暂存的属性与线级物模型属性名一一对应（如 `drone_home_point_longitude`、
`station_work_mode`、`battery_voltage`）。未 set 的属性不进 `params` 数组。

---

## 6. 错误处理

- setter 返回 `<0` 表示累积器已满——先 commit（或 `reset`）再重试。
- `commit` / `get` 在传输失败或回复 `result` 非零时返回 `<0`。
- 暂存后务必 `commit`（或 `reset`）——否则暂存值会残留到下一次 commit。
