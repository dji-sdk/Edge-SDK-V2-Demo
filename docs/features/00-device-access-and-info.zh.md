# 设备接入与信息 (Device Access & Info)

> 能力：连接 O4 地面站、获得已鉴权的会话，并上报第三方设备的能力集（设备类型 / 机型）。
> 这是每个集成的起点——其余所有能力都需要先在这里拿到一个有效会话。

| | |
|---|---|
| **设备** | 通用（dt5 第三方机库 **与** dt7 RID 设备） |
| **方向** | 上行——第三方主动连接机库并上报状态 |
| **头文件** | `edge_dock/dock_session.h`、`edge_dock/auth.h`、`edge_dock/broker_discovery.h`、`edge_dock/dock_id_provider.h` |
| **聚合头文件** | `#include "edge_dock/edge_dock.h"` 一次引入以上全部 |

---

## 1. 功能概述

"设备上线"分两步：

1. **连接**——解析 O4 broker 地址（UDP 发现，或固定 IP），再打开已鉴权的 MQTT 会话。
   `dock_session_connect()` 封装了整个生命周期。
2. **能力上报**——声明*你是哪种设备*（`device_type = 5` 第三方机库，或 `device_type = 7`
   RID 设备），通过 `auth_report_capability_*()` 完成。**能力上报被确认之前，下游能力域不会生效**
   ——例如 RID/ADS-B 监视只有在 `dt7` 上报成功后才可用。

| 子能力 | API | 用途 |
|---|---|---|
| Broker 发现 | `dock_discover_broker()` | 监听 UDP `18830` 上的 O4 广播，解析 broker IP |
| 连接 / 断开 | `dock_session_connect()` / `dock_session_disconnect()` | 打开 / 关闭已鉴权会话 |
| 默认选项（机库 dt5） | `dock_session_default_opts()` | 用编译期凭据填充 `DockSessionOpts`，`device_sn` = `DOCK_DEVICE_SN` |
| 默认选项（RID dt7） | `dock_session_default_opts_rid()` | 同上，但 `device_sn` = `DOCK_RID_DEVICE_SN`、`username_suffix` = `"@rid"` |
| 能力上报（机库） | `auth_report_capability_dt5()` / `_sync()` | 声明 `device_type=5` 第三方机库 |
| 能力上报（RID） | `auth_report_capability_dt7()` / `_sync()` | 声明 `device_type=7` RID 设备 |
| ID 提供器 | `dock_id_provider_register()` / `_unregister()` | 提供信封 `bid`/`tid` 生成器（由会话封装自动管理） |

---

## 2. 快速示例

最小路径——连接后上报能力。取自 `samples/quickstart/main.c`：

```c
#include "edge_dock/edge_dock.h"
#include <stdio.h>

int main(void)
{
    DockSessionOpts opts;
    dock_session_default_opts(&opts);          /* 凭据 + UDP 发现，均来自 dock_app_info.h */

    ThingModelClientSession_t *s = dock_session_connect(&opts);
    if (!s) { fprintf(stderr, "connection failed\n"); return 1; }

    /* 声明本设备为第三方机库 (device_type = 5) */
    auth_report_capability_dt5(s);

    /* ... 用 `s` 使用其它能力（飞行 / 充电 / 配置 / ...） ... */

    dock_session_disconnect(s);
    return 0;
}
```

生产环境建议用 `auth_report_capability_dt5_sync()`：它会等待机库的确认，让你在继续之前确知设备已真正上线。

### 重连会自动补报能力

机库只在**一条** MQTT 会话的生命周期内记住一个三方设备的登记。SDK 会自行重连（`auto_reconnect` 默认打开），
而设备回来后处于*已连接但未登记*状态：peripheral 停在 `status:2`，机库随后会静默丢弃所有**点对点回发**的
`requests_reply`——即 SDR、网络、充电三个域（它们由机库的外设服务处理，而不是直接发到标准回执主题）。
过程中不会有任何报错，这些调用只是超时，而 `set_tsl_params` / `airport_organization_bind` 却一切正常。
这种「部分方法可用、部分超时」的不对称现象，就是该问题的指纹。

因此 `dock_session_default_opts()` / `_rid()` 会替你装好 `opts.on_reconnect`
（分别是 `auth_report_capability_dt5` / `_dt7`），`dock_session_connect()` 在每次重连后自动重跑它。
这是默认行为，无需你调用任何东西。两点注意：

- 该回调跑在运行时的重连工作线程上，而该线程还负责处理*下一次*掉线，所以必须保持非阻塞。
  **不要**换成 `auth_report_capability_dt5_sync()`。
- **首次**连接不在覆盖范围内，仍需像上面示例那样显式上报。

置 `opts.on_reconnect = NULL` 可退出该行为，或指向你自己的函数以重跑额外的会话级初始化。

---

## 3. API 参考

### `void dock_session_default_opts(DockSessionOpts *opts)`
用 `dock_app_info.h`（由 `DOCK_APP_ID` / `DOCK_APP_KEY` / … 等 CMake 缓存变量设置）编译进来的凭据与默认值
填充 `opts`。这是 **dt5（三方机库）** 身份：`device_sn` = `DOCK_DEVICE_SN`，无用户名后缀。
务必先调用它（或 `_rid` 变体），再按需覆盖单个字段。
- 运行期覆盖：`DOCK_DEVICE_SN` 环境变量。显式赋值 `opts.device_sn` 仍然优先。

### `void dock_session_default_opts_rid(DockSessionOpts *opts)`
同上，但填充 **dt7（RID）** 身份：`device_sn` = `DOCK_RID_DEVICE_SN`、`username_suffix` = `"@rid"`。
- dt5 与 dt7 在网关眼里是两台设备。账号 `"<app_id>_esdk_<device_sn>"` 同时是 MQTT client_id 和 topic
  属主段，**两者不能共用 SN**：共用时后连的一方会被拒（`CONNACK rc=5 not authorised`）。
- `DOCK_RID_DEVICE_SN` 默认为 `<DOCK_DEVICE_SN>_rid`（若是 RID-only 构建 `-DEDGE_DOCK_ENABLE_DOCK=OFF`，
  则直接等于 `DOCK_DEVICE_SN`）；要完全独立的值就传 `cmake -DDOCK_RID_SN=...`。
  运行期覆盖：`DOCK_RID_SN` 环境变量。

### `ThingModelClientSession_t *dock_session_connect(const DockSessionOpts *opts)`
打开已鉴权会话。内部依次：注册 `bid`/`tid` id 提供器 →（可选）UDP 发现 → 连接 broker。
- **返回** 成功返回会话句柄，失败返回 `NULL`。
- 返回的句柄是其余每个能力 API 的必需首参。
- 发现：当 `opts->use_discovery` 为 true 时，监听 UDP `18830` 达 `discovery_timeout_ms`；
  超时则回退到 `opts->fallback_ip`。当 `opts->broker_ip` 非空时，跳过发现直接使用该 IP。

### `void dock_session_disconnect(ThingModelClientSession_t *session)`
关闭会话并注销 id 提供器。退出前调用一次。

### `int auth_report_capability_dt5(ThingModelClientSession_t *session)`
向 `state` 主题上报 `capability_report(device_type=5)`——第三方机库。发送即返回（fire-and-forget）。
- **返回** 发送成功 `0`，出错 `<0`。

### `int auth_report_capability_dt5_sync(ThingModelClientSession_t *session, int timeout_ms)`
### `int auth_report_capability_dt7_sync(ThingModelClientSession_t *session, int timeout_ms)`
与上面的发送即返回版本相同，但会订阅 `state_reply` 并等待确认，校验 `data.result == 0`，
不为 0 时一并打印 `data.error_reason`。`timeout_ms <= 0` 使用默认 `5000` ms。

这是**唯一**能看见"上报被拒"的途径。无论如何 publish 都会成功，而机库是在**回复层**拒绝的——
例如网关中该 `device_type` 的槽位已被占满时回 `result=201 "Device type slot is full, max limit reached for this type"`。
若不读这个回执，设备会一直处于未登记状态，机库随后静默丢弃所有点对点 `requests_reply`
（SDR / 网络 / 充电），表现出来只是莫名其妙的超时。

该 `state_reply` 订阅**有意保留**不退订，使会话继续收到 `DockSessionOpts.on_reconnect`
在重连后发出的那些"发送即返回"上报的回执——挂一个会话监听器即可看到。
- **返回** `0` = 已确认；`THING_MODEL_RC_TIMEOUT (-3)` = 超时未收到回复；其它 `<0` = 收到回复但
  `result != 0`。

### `int auth_report_capability_dt7(ThingModelClientSession_t *session)`
上报 `capability_report(device_type=7)`——RID 设备。**必须先于** RID/ADS-B 监视能力
（`rid_infos` / `adsb_infos`）被机库接受。

### `int dock_discover_broker(char *out_ip, size_t out_ip_cap, int timeout_ms)`
底层发现接口，用于在不打开会话的情况下拿到 broker IP（`dock_session_connect` 会替你调用它）。
阻塞监听 UDP `18830`，直到收到合法的 `MQTTBRK|…|ip=<broker_ip>|…` 广播或超时
（`timeout_ms <= 0` = 永久等待）。建议 `out_ip_cap` ≥ 46。
- **返回** `1` = 找到（已填充 `out_ip`）；`0` = 超时/失败（调用方回退到默认 IP）。

### `void dock_id_provider_register(const char *dock_sn)` / `void dock_id_provider_unregister(void)`
注册/注销信封 `bid`/`tid` 生成器（优先 libuuid 生成 UUIDv4，不可用时回退到 `"<dock_sn>-<计数器>"`）。
**通常你不需要调用它们**——`dock_session_connect` / `_disconnect` 已代为管理。仅为直接驱动 runtime
的高级场景暴露。

---

## 4. 交互时序

```
  你的应用                          edge-sdk                         O4 机库
    │  dock_session_default_opts()     │                                │
    │─────────────────────────────────▶│                                │
    │  dock_session_connect(opts)       │  注册 bid/tid 提供器            │
    │─────────────────────────────────▶│                                │
    │                                   │  UDP 18830 发现 ───────────────▶ (广播 MQTTBRK|…|ip=…)
    │                                   │◀───────────────────────────────│
    │                                   │  MQTT 连接 + 鉴权 ─────────────▶│
    │◀── 会话句柄 (或 NULL) ─────────────│◀───────────────────────────────│
    │  auth_report_capability_dt5_sync()│  发布 state: capability_report
    │─────────────────────────────────▶│────────────────────────────────▶│
    │◀── 0 = 已上线 ────────────────────│◀─── state_reply {result:0} ─────│
                        (设备已上线；其余能力此时可用)
```

---

## 5. 数据结构 —— `DockSessionOpts`

用 `dock_session_default_opts()`（dt5）或 `dock_session_default_opts_rid()`（dt7）填充，再按需覆盖。

| 字段 | 类型 | 含义 |
|---|---|---|
| `app_id`、`app_key`、`app_license`、`app_name` | `const char *` | DJI 开发者凭据。为空 → 使用 `dock_app_info.h` 宏 |
| `device_sn` | `const char *` | 本设备序列号。dt5 用 `DOCK_DEVICE_SN`，dt7 用 `DOCK_RID_DEVICE_SN`——**两者必须不同**（account 即 client_id）。运行期可用 `DOCK_DEVICE_SN` / `DOCK_RID_SN` 环境变量覆盖 |
| `broker_ip` | `const char *` | 非空时跳过发现，直接连接此地址 |
| `broker_port` | `int` | Broker 端口（默认 `1883`） |
| `use_discovery` | `bool` | 先监听 UDP `18830`（默认 `true`） |
| `discovery_timeout_ms` | `int` | 回退前的最大发现等待 |
| `fallback_ip` | `const char *` | 发现失败时使用的地址 |
| `plaintext_access` | `bool` | 默认 `true`：单次明文 MQTT，用户名 `"<app_id>_esdk_<sn>"`。`false` → runtime 两阶段 TLS+HMAC 安全接入（需 CA 与安全 broker） |
| `username_suffix` | `const char *` | 明文用户名后缀；RID 设备用 `"@rid"` 以提高单包上限，由 `dock_session_default_opts_rid()` 代为设置，dt7 无需手工填。可用 `DOCK_USERNAME_SUFFIX` 环境变量覆盖。`NULL`/`""` = 无 |
| `transport_vtable` | `const void *` | 可选的自定义传输（如 CI loopback mock）。`NULL` = 默认 mosquitto |

---

## 6. 错误处理

`dock_session_connect()` 在任何失败（凭据错误、broker 不可达、鉴权被拒）时返回 `NULL`。使用句柄前先判 `NULL`。

`auth_report_capability_*` 系列返回 thing-model 返回码：

| 返回码 | 值 | 含义 |
|---|---|---|
| `THING_MODEL_RC_OK` | `0` | 成功（`_sync`：已确认且 `result:0`） |
| `THING_MODEL_RC_TIMEOUT` | `-3` | `_sync` 在超时内未收到 `state_reply` |
| `THING_MODEL_RC_TRANSPORT_ERROR` | `-4` | 传输层发送失败 |
| `THING_MODEL_RC_AUTH_FAILED` | `-5` | 鉴权被拒 |
| 其它 `< 0` | — | 收到回复但 `result != 0`，或其它 runtime 错误 |

机库侧 `state_reply` 里值得认识的 result：

| `data.result` | `error_reason` | 含义 |
|---|---|---|
| `0` | — | 设备已上线，下游能力域可用 |
| `201` | `Device type slot is full, max limit reached for this type` | 该 `device_type` 已绑定设备、槽位已占满。必须先解绑已有的同类型设备，否则换什么 SN 都会被拒 |

能力上报的结果走 `state_reply` 主题，所以上面这种拒绝由回复给出，而不是由发送调用给出：
`dock_session_connect()` 和发完即返回的 `auth_report_capability_dt5()/dt7()` 在消息发出后即返回成功。
需要拿到网关判定结果时，请用 `_sync` 变体（或自己挂 `state_reply` 监听）——这也是生产环境推荐
`auth_report_capability_dt5_sync()` 的原因。

**注意事项**
- 声明正确的 `device_type`：`dt5` 用于第三方机库，`dt7` 用于 RID 设备。`dt7` 上报是监视能力的硬前置
  ——上报 `dt5` 不会启用它。
- 能力上报要在连接成功**之后**、使用其它能力**之前**进行；建议用 `_sync` 变体，确认设备上线后再动作。
- 发现与连接是两个独立的失败点——`NULL` 会话最常见的原因是：既没发现到 broker，`fallback_ip` 也不可达。
