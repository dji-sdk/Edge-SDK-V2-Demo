# edge-sdk 开发者文档

面向 O4 地面站的第三方机库接入 SDK。文档按功能组织，每页都有英文版（去掉 `.zh` 后缀即是）。

## 上手

| | |
|---|---|
| [快速上手](guide/01-quickstart.zh.md) | 五步跑通 |
| [构建与部署](guide/02-build-and-deploy.zh.md) | host / aarch64、凭据、单设备裁剪 |
| [核心概念](guide/03-concepts.zh.md) | 会话 · dt5/dt7 · 上下行 · service/reply/event · bid/tid |

## 能力

| 能力 | 设备 | 简介 |
|---|---|---|
| [设备接入与信息](features/00-device-access-and-info.zh.md) | 通用 | 连接、鉴权、上报能力 |
| [飞行控制](features/01-flight-control.zh.md) | dt5 | 起飞 / 降落 / 异常恢复，进度推送 |
| [充电与供电](features/02-charging-and-power.zh.md) | dt5 | 飞机开关机、充电开关 |
| [物模型参数](features/03-thing-model-parameters.zh.md) | dt5 | 读写机库物模型属性 |
| [机库配置](features/04-dock-configuration.zh.md) | dt5 | 网络、SDR 固件、云平台、组织绑定 |
| [监视上报（RID/ADS-B）](features/05-surveillance-rid.zh.md) | dt7 | 上报探测到的飞机 |

## 建议阅读路径

1. [核心概念](guide/03-concepts.zh.md) —— 整套 API 立足的五个概念。
2. [快速上手](guide/01-quickstart.zh.md) —— 端到端串一遍。
3. 你要做的那个[功能页](features/)。
