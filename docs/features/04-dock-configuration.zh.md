# 机库配置 (Dock Configuration)

> 能力：配置机库的网络、SDR（图传）固件类型、云平台接入与组织绑定。四个由第三方推送给机库的独立设置。

| | |
|---|---|
| **设备** | dt5（第三方机库） |
| **方向** | 上行——第三方调用机库（`requests` / `requests_reply`） |
| **头文件** | `edge_dock/config.h` |

---

## 1. 功能概述

四组独立配置，都挂在共享的 `DockConfig_t` 对象上：

| 组 | 读 | 写 |
|---|---|---|
| 网络 | `dock_config_get_network_params()` | `dock_config_set_network_params()`（DHCP） / `..._static[_ex]()` |
| SDR 固件 | `dock_config_get_sdr_firmware()` | `dock_config_set_sdr_firmware[_ex]()` |
| 云平台 | — | `dock_config_set_cloud_params[_ex]()` |
| 组织绑定 | — | `dock_config_organization_bind[_ex]()` |

每个 `set` 都是一次 `requests` → `requests_reply` 往返，返回 `result`。

---

## 2. 快速示例

```c
#include "edge_dock/edge_dock.h"

DockConfig_t *cf = dock_config_create(s);

/* 网络：静态 IP */
dock_config_set_network_params_static_ex(cf,
        "192.168.1.100", "255.255.255.0", "192.168.1.1", "8.8.8.8", "8.8.4.4");

/* SDR 固件：100 = M4TD，103 = M400 */
dock_config_set_sdr_firmware_ex(cf, 100);

/* 上云：cloud_type 决定哪些字段有效，不适用的字段会被丢弃不发。
 * 0 = 司空 FlightHub 2（组织id + 绑定码），1 = 三方云（全部可填），
 * 2 = FlightHub 私有化（地址 + 组织id + 绑定码）。
 * 内部先发 set_cloud_params，再发 airport_organization_bind。*/
dock_config_cloud_onboard(cf, 2, "mqtt://host:1883", NULL, NULL, "org-id", "device-binding-code");

/* 也可以自己分别调这两个方法 */
dock_config_set_cloud_params_ex(cf, 1, "mqtt://host:1883", "account", "password");
dock_config_organization_bind_ex(cf, "org-id", "device-binding-code");

dock_config_destroy(cf);
```

---

## 3. API 参考

### 网络
- `int dock_config_get_network_params(DockConfig_t *c)` —— 读当前网络配置。
- `int dock_config_set_network_params(DockConfig_t *c)` —— 设为动态 IP（DHCP）。
- `int dock_config_set_network_params_static(DockConfig_t *c)` —— 用 demo 默认值设静态 IP。
- `int dock_config_set_network_params_static_ex(DockConfig_t *c, const char *static_ip, const char *netmask, const char *gateway, const char *pri_dns, const char *sec_dns)`
  —— 显式字段设静态 IP。

### SDR 固件
- `int dock_config_get_sdr_firmware(DockConfig_t *c)` —— 读当前图传固件类型。
- `int dock_config_set_sdr_firmware(DockConfig_t *c)` —— 用 demo 默认值设置。
- `int dock_config_set_sdr_firmware_ex(DockConfig_t *c, int drone_type)` —— 显式设置；`drone_type` `100` = M4TD，`103` = M400。

### 云平台
`cloud_type` 是固定枚举，且决定机库认哪些上云字段——地址与 MQTT 账号密码只有在 broker 归运营方所有时才有意义：

| `cloud_type` | 地址（`mqtt_url`） | 账号 / 密码 | `organization_id` 组织id | `device_binding_code` 绑定码 |
|---|---|---|---|---|
| `0` 司空 FlightHub 2 | — | — | ✓ | ✓ |
| `1` 三方云 | ✓ | ✓ | ✓ | ✓ |
| `2` FlightHub 私有化 | ✓ | — | ✓ | ✓ |

- `bool dock_config_cloud_field_allowed(int cloud_type, E_DockCloudField field)` —— 上表的谓词形式；
  发送前用它决定该向操作者采集哪些字段（dock_cli 就靠它决定只问哪几项）。
- `const char *dock_config_cloud_type_str(int cloud_type)` / `dock_config_cloud_field_name(E_DockCloudField)`
  —— 给日志与交互提示用的可读名称。
- `int dock_config_cloud_onboard(DockConfig_t *c, int cloud_type, const char *mqtt_url, const char *mqtt_account, const char *mqtt_password, const char *organization_id, const char *device_binding_code)`
  —— 一次性上云。不适用于该 `cloud_type` 的字段、以及传 `NULL`/`""` 的字段，都不会出现在请求里；
  先发 `set_cloud_params`，若给了组织id/绑定码再发 `airport_organization_bind`（`set_cloud_params` 失败则跳过绑定）。
- `int dock_config_set_cloud_params(DockConfig_t *c)` —— 用 demo 默认值设置。
- `int dock_config_set_cloud_params_ex(DockConfig_t *c, int cloud_type, const char *mqtt_url, const char *mqtt_account, const char *mqtt_password)`
  —— 裸方法，不套用上述策略；字段传 `NULL` 即不上线。

### 组织绑定
- `int dock_config_organization_bind(DockConfig_t *c)` —— 用 demo 默认值绑定。
- `int dock_config_organization_bind_ex(DockConfig_t *c, const char *organization_id, const char *device_binding_code)`
  —— 用显式组织 id + 绑定码绑定。

以上确认成功均返回 `0`，否则 `<0`。无 `_ex` 版会用 demo 默认值调用对应 `_ex` 版。

---

## 4. 交互时序

```
  你的应用                                    edge-sdk           O4 机库
    │ dock_config_set_sdr_firmware_ex(cf,100) ─▶│ request: set_sdr_firmware ─▶│
    │                                           │◀── requests_reply{result} ──│
    │◀── 0 (或 <0) ─────────────────────────────│                   │
```

---

## 5. 数据结构

`DockConfig_t` 不透明。各组的线级参数：
- **网络（静态）：** `static_ip`、`netmask`、`gateway`、`pri_dns`、`sec_dns`；或 `dynamic_ip=true` 走 DHCP。
- **SDR 固件：** `drone_type`（`100` M4TD / `103` M400）。
- **云平台：** `cloud_type`（`0`/`1`/`2`）、`mqtt_url`、`mqtt_account`、`mqtt_password`——哪些字段
  生效取决于 `cloud_type`（见 §3 表格），不适用的字段不会发出。
- **组织：** `organization_id`、`device_binding_code`。

---

## 6. 错误处理

- `set_cloud_params` 与 `organization_bind` 只回 `result`（无 `output`）；返回 `<0` 表示操作被拒或传输失败。
- 发送前校验网络字段——不可达的网关/DNS 在线级会被接受，却可能让机库失联；请保留恢复路径（如 DHCP 回退）。
