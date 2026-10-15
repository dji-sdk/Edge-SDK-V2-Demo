# 监视上报 (Surveillance: RID / ADS-B)

> 能力：把探测到的邻近飞机——Remote ID（RID）与 ADS-B 目标——上报给机库，可单发或按批周期上报。
> 这是 RID 设备的核心工作。

| | |
|---|---|
| **设备** | dt7（RID 设备）——需先上报 `dt7` 能力 |
| **方向** | 上行——第三方推送 `events` |
| **头文件** | `edge_dock/surveillance.h` |

---

## 1. 功能概述

监视上报的是你的设备探测到的**其它**飞机（不是自身定位）。两种上报方式：

- **单发**——`surveillance_report_rid[_ex]()` / `surveillance_report_adsb[_ex]()`：立即发送单个目标
  （调试 / 低频率）。
- **周期批量**——探测到目标时先缓存，后台线程每 2s 作为一条批量事件刷出（匹配机库 5Hz 限速）。
  这是生产路径。

> 前置：先上报 `dt7` 能力（`auth_report_capability_dt7`），监视事件才会被接受——见《设备接入与信息》。
> RID 设备用 `dock_session_default_opts_rid()` 连接——它会带上 RID 自己的 SN 和 `"@rid"` 用户名后缀。

---

## 2. 快速示例

```c
#include "edge_dock/edge_dock.h"

/* 会话 `s` 已以 RID 身份连接，且已上报 dt7 能力 */
Surveillance_t *sv = surveillance_create(s);

/* 单发 RID 目标：lat, lon, height(m), speed(m/s), heading(deg) */
surveillance_report_rid_ex(sv, 22.53, 113.95, 120, 8.0, 90);

/* 生产：从雷达回调缓存，周期刷出 */
surveillance_start_periodic(sv);
/*   ... 探测到时 surveillance_cache_rid_target(sv, &target) ... */
surveillance_stop_periodic(sv);

surveillance_destroy(sv);
```

---

## 3. API 参考

### `Surveillance_t *surveillance_create(ThingModelClientSession_t *session)` / `void surveillance_destroy(Surveillance_t *s)`
创建 / 释放监视对象。`destroy` 同时会停止周期上报。

### 单发上报
- `void surveillance_report_rid(Surveillance_t *s)` / `void surveillance_report_adsb(Surveillance_t *s)`
  —— 上报一个 demo 目标（调试/自测）。
- `void surveillance_report_rid_ex(Surveillance_t *s, double lat, double lon, int height, double speed, int heading)`
  —— `lat`/`lon` 度，`height` 相对起飞点（m），`speed` 水平速度（m/s），`heading` 0–359°。
- `void surveillance_report_adsb_ex(Surveillance_t *s, const char *icao, const char *callsign, double lat, double lon, int height, double speed, int heading)`
  —— `icao` 24 位十六进制 id 字符串，`callsign`，`lat`/`lon`（度），`height` 海拔 MSL（m），
  `speed`（**km/h**——注意与上面 RID 目标的 m/s 不是同一单位），`heading`（度）。
  司空 2 会把 `speed` 除以 3.6 再按 m/s 显示，所以默认值 `941` 到界面上是 `261.4`——按 km/h 发，按 m/s 读。
  只覆盖关键字段，其余非关键字段保留 demo 默认值。每次调用同时会成为周期线程的空缓存兜底值（见下）。

### 周期批量
- `void surveillance_start_periodic(Surveillance_t *s)` / `void surveillance_stop_periodic(Surveillance_t *s)`
  —— 启停每 2s 批量刷出的后台线程。缓存为空时重发**最近一次单发的目标**兜底（在你发出别的目标之前
  就是 demo 目标），这样菜单里输入的值会持续上报，而不会在下一拍被覆盖。
- `void surveillance_cache_rid_target(Surveillance_t *s, const void *rid_target)` /
  `void surveillance_cache_adsb_target(Surveillance_t *s, const void *adsb_target)`
  —— 缓存一个探测目标供下次刷出。生产中从雷达回调调用。指针是生成的 `T_RidTarget` / `T_AdsbTarget`
  （`surveillance/surveillance_types.h`）；用 `void *` 避免把生成类型泄进本头文件。

---

## 4. 交互时序

```
  雷达回调              edge-sdk（Surveillance）             O4 机库
    │ cache_rid_target(sv,&t) ─▶│ (已缓冲)                      │
    │ cache_rid_target(sv,&t) ─▶│ (已缓冲)                      │
    │           (每 2s)         │ event: rid_infos {targets[...]} ─▶│
    │                           │ event: adsb_infos {targets[...]} ─▶│
```

---

## 5. 数据结构

- 单发 `_ex` 参数为普通标量/字符串（见上方 API 参考）。
- 缓存目标是 `surveillance/surveillance_types.h` 里生成的 `T_RidTarget` / `T_AdsbTarget` 结构体；
  填好一个后把它的地址传给 `cache_*`。

---

## 6. 错误处理

- 上报器返回 `void`——尽力发布。可靠性来自周期批量节奏（2s），而非逐次返回检查。
- 遵守 5Hz 限速：优先用 `start_periodic` + `cache_*`，别手写紧循环单发上报，那会被机库限流/丢弃。
- 若事件被机库静默忽略，先确认 `dt7` 能力上报已成功。
