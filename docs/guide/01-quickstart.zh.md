# 快速上手 —— 五步跑通

这篇带你用 edge-sdk 把第三方机库接到 O4 地面站，走一遍最短路径。SDK 是按功能组织的，代码里只要
`#include "edge_dock/edge_dock.h"` 这一句，所有能力就都有了。

想深入某个功能，去看对应的[功能文档](../features/)。如果下面出现的"会话""上下行""service/reply/event"
你还不熟，建议先扫一眼[核心概念](03-concepts.zh.md)，再回来。

## 第 0 步 —— 环境

装好这些就行：CMake ≥ 3.14、一个 C 编译器、libmosquitto、OpenSSL（cJSON 我们已经打包进来了，不用管）。

编译就一句（release 模式，自包含，不依赖任何内网/工具）：

```bash
cmake -B build && cmake --build build -j
```

host 还是 aarch64、凭据怎么填、只做单设备怎么裁——都在[构建与部署](02-build-and-deploy.zh.md)里，这里先不展开。

## 第 1 步 —— 填凭据

把你的开发者凭据填成 CMake 变量（或者 `-D` 传进去），`dock_app_info.h` 会在 configure 时自动生成，
你不用手写：

```bash
cmake -B build \
  -DDOCK_APP_ID=xxx -DDOCK_APP_KEY=xxx -DDOCK_APP_LICENSE=xxx \
  -DDOCK_APP_NAME=xxx -DDOCK_DEVICE_SN=你的机库SN
```

（还要出 RID 设备？它需要自己的 SN——`DOCK_RID_SN`，留空则默认 `<DOCK_DEVICE_SN>_rid`。
见[构建与部署](02-build-and-deploy.zh.md)。）

## 第 2 步 —— 连上

```c
#include "edge_dock/edge_dock.h"

DockSessionOpts opts;
dock_session_default_opts(&opts);          /* 默认开着 UDP(18830) 自动发现 O4 broker */
ThingModelClientSession_t *s = dock_session_connect(&opts);
/* 失败会返回 NULL；发现不到就回退到你配的 fallback IP */
```

记得判一下 `s` 是不是 `NULL`，是的话多半是 broker 没发现到、也连不上 fallback。
细节看 → [设备接入与信息](../features/00-device-access-and-info.zh.md)

## 第 3 步 —— 上线：上报能力

告诉机库"我是谁"：

```c
auth_report_capability_dt5(s);   /* device_type=5，机库。RID 设备用 dt7 */
```

一句提醒：**这步不做，后面的能力机库不认**。RID 设备尤其注意，得先上报 dt7，监视才生效。

## 第 4 步 —— 挂下行 handler（机库下发的流程）

```c
Flight_t *fl = flight_create(s, dock_session_account(s));  /* 一次性注册 5 个 service handler */
/* 机库下发 airport_takeoff_prepare 之类时，SDK 会自动帮你回 in_progress */
```

这步就是"挂个电话等机库打进来"，接起来的活 SDK 替你干了。→ [飞行控制](../features/01-flight-control.zh.md)

## 第 5 步 —— 主动调用 / 推送

```c
/* 充电与飞机开关机——主动调机库 */
Charging_t *ch = charging_create(s);
charging_charge_open(ch);

/* 推飞行进度（percent 0→100） */
flight_push_takeoff_prepare(fl);

/* 监视上报（RID 设备） */
Surveillance_t *sv = surveillance_create(s);
surveillance_report_rid(sv);

/* 物模型参数 */
DockConfig_t *cf = dock_config_create(s);
dock_config_get_tsl_params(cf);
```

各功能细节：[充电与供电](../features/02-charging-and-power.zh.md)、
[物模型参数](../features/03-thing-model-parameters.zh.md)、
[机库配置](../features/04-dock-configuration.zh.md)、
[监视上报](../features/05-surveillance-rid.zh.md)。

## 收尾

用完记得挨个释放、断开，别漏：

```c
charging_destroy(ch); dock_config_destroy(cf);
flight_destroy(fl); surveillance_destroy(sv);
dock_session_disconnect(s);
```

## 调试：看线上到底发了啥

```bash
THING_MODEL_LOG_LEVEL=debug ./build/dock_cli 2>&1 | grep -E 'PUB|RCV'
```
一条请求和它的回复，`tid` 对得上，就说明这一来一回配对成功了。排查问题先看这个。

## 能直接跑的例子

- `samples/quickstart/main.c` —— 连接 + 上报能力 + 一次 `charge_open`，最小闭环。
- `samples/cli/main.c` —— 交互菜单，按功能一个一个手动触发，适合摸接口。
