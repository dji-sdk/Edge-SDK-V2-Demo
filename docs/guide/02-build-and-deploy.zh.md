# 构建与部署

这篇讲怎么把 edge-sdk 编到你的目标平台、把凭据接进去、以及只做一种设备时怎么把 SDK 裁小。
发布包是自包含的——不需要代码生成器、不需要 Python、不碰任何网络。

## 两种编法

### `build.sh`（省事的封装）

```bash
./build.sh              # host 编译（x86_64）           → build/
./build.sh --cross      # aarch64 交叉编译              → build-aarch64/
./build.sh -c           # 清掉对应 build 目录重新 configure
./build.sh -j 8         # 设并发
```
host 和 cross 用**各自独立**的 build 目录，缓存互不干扰，你可以来回切不用手动清。

### 直接 CMake

```bash
cmake -B build && cmake --build build -j
```
顶层 `CMakeLists.txt` 会自动认出 release 模式（包里带着 `sdk/` 快照），直接纯静态编译，一点代码生成都不用。

## 会编出什么

| 产物 | 是什么 |
|---|---|
| `dock_quickstart` | 最小示例——连接 + 上报能力 + 一次充电 |
| `dock_cli` | 交互菜单，覆盖所有能力 |

host 产物在 `build/`，cross 产物在 `build-aarch64/`。

## 凭据

把你的开发者凭据当 CMake 变量传进去，`dock_app_info.h` 会在 configure 时自动生成（**别把真凭据提交进仓库**）：

```bash
cmake -B build \
  -DDOCK_APP_ID=xxx -DDOCK_APP_KEY=xxx -DDOCK_APP_LICENSE=xxx \
  -DDOCK_APP_NAME=xxx -DDOCK_DEVICE_SN=你的机库SN \
  -DDOCK_BROKER_FALLBACK_IP=192.168.50.10 -DDOCK_BROKER_PORT=1883
```

如果同时还要出 RID 设备，它必须有**自己的** SN——账号 `"<app_id>_esdk_<sn>"` 就是 MQTT client_id，
dt5 和 dt7 共用一个时，后连的一方会被拒（`CONNACK rc=5 not authorised`）。
`DOCK_RID_SN` 留空时默认派生 `<DOCK_DEVICE_SN>_rid`，要完全独立的值就显式传：

```bash
cmake -B build ... -DDOCK_RID_SN=你的RID设备SN
```

**只出 RID 设备**（`-DEDGE_DOCK_ENABLE_DOCK=OFF`）时没有 dt5 可冲突，`DOCK_DEVICE_SN` 会被原样使用，
不追加 `_rid` 后缀。configure 会打印最终解析出的 SN（`[edge-sdk] device SN: ...`），拿不准就看这行。

## 只做一种设备就裁掉另一种

如果你只出一种设备，把另一种整个关掉——它的源码和生成代码都不会进编译，包体也更小：

```bash
cmake -B build -DEDGE_DOCK_ENABLE_DOCK=ON  -DEDGE_DOCK_ENABLE_RID=OFF   # 只做 dt5 机库
cmake -B build -DEDGE_DOCK_ENABLE_DOCK=OFF -DEDGE_DOCK_ENABLE_RID=ON    # 只做 dt7 RID
```

| 选项 | 设备 | 能力 |
|---|---|---|
| `EDGE_DOCK_ENABLE_DOCK` | dt5（机库） | 飞行、充电、配置 |
| `EDGE_DOCK_ENABLE_RID` | dt7（RID） | 监视 |

两个至少得开一个。公共核心（会话 / 发现 / id / auth）永远都编。

## 交叉编译到 aarch64

`./build.sh --cross` 用的是仓库里自带的 `third_party/aarch64-linux-gnu/` 迷你 sysroot（头文件 + 预编译好的
OpenSSL/mosquitto），所以交叉编译**完全不碰**宿主机系统库。它会自动选 `cmake/toolchain-aarch64.cmake`，
你不用管。

## 依赖

- **host：** 编译时找系统的 OpenSSL 和 libmosquitto（cJSON 是源码内置的）。
- **交叉 / vendored：** 靠自带 sysroot 做到自包含，目标机上不需要任何系统库。
- 可选的 `libuuid` 能让 `bid`/`tid` 生成更规范；没有也行，SDK 会退回 `SN + 计数器`。
