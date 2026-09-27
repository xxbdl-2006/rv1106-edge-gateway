# RV1106 Embedded Linux Edge Gateway

基于 Luckfox Pico Pro/Max（Rockchip RV1106G）的嵌入式 Linux 音视频与多传感器边缘网关。

项目主线是不依赖官方 `rkipc` 黑盒程序，自行实现：

```text
SC3336 -> MIPI CSI -> V4L2 -> NV12 -> MPP H.264 -> Packet Queue -> RTSP
```

后续继续接入 MPU6050、OSD 和告警。

## 当前状态

已完成并验证：

- SD 卡固件启动、ADB 与 RNDIS 网络。
- SC3336、MIPI D-PHY、ISP/CIF 和 V4L2 节点初始化。
- `/dev/video11` 上 1280x720 NV12 连续采集，约 30 FPS。
- `rkaiq_3A_server` 启动和稳定帧采集。
- NV12 数据送入 Rockit MPI 硬件编码 H.264，文件可由 VLC 正常播放。
- Packet Sink 抽象 + File Sink + 有界 Packet Queue（满时丢最旧整个 GOP）。
- **RTSP over TCP 多客户端**，ffplay / VLC / ffmpeg 均可播放，已上板验证。
- **开机自启**：重启后约 11 秒自动出流，无需人工操作。
- **Frame RingBuffer + 采集线程独立**（`--threads`），已主机自测。

```text
rtsp://172.32.0.93:8554/live/0
```

当前程序的输出后端由 `--sink` 选择：

```text
--sink file   encoder -> 文件，与基线逐字节一致
--sink queue  encoder -> 有界队列 -> 落盘线程，用于回归验证队列
--sink rtsp   encoder -> 有界队列 -> RTSP over TCP 多客户端
```

采集与编码默认仍是同步流水线；加 `--threads` 后采集线程独立、经 Frame RingBuffer 交给编码。
尚未实现 OSD 和 MPU6050。

详细的验收数据、已修复缺陷根因和构建自测说明见 `docs/status.md`。

## 硬件与软件环境

| 项目 | 配置 |
| --- | --- |
| 主控 | Rockchip RV1106G，Cortex-A7 + RISC-V + 0.5T NPU |
| 开发板 | Luckfox Pico Pro / Max |
| 摄像头 | SC3336，MIPI CSI，2304x1296 |
| 传感器 | MPU6050，规划接入 `/dev/i2c-4`（尚未接入） |
| 板端系统 | Buildroot Linux 5.10.160，`armv7l` |
| 启动介质 | TF/SD 卡，`/dev/mmcblk1` |
| 宿主机 | Windows 11 + VMware Ubuntu 22.04 |
| 交叉工具链 | `arm-rockchip830-linux-uclibcgnueabihf-gcc` |
| 板端网络 | RNDIS，开发板约 `172.32.0.93` |

## 架构

当前已实现的媒体链路：

```text
+--------+    MIPI CSI    +--------+    V4L2 mmap    +------------------+
| SC3336 | -------------> | RKISP | --------------> | v4l2_capture     |
+--------+                +--------+                 +------------------+
                                                              |
                                                              | NV12
                                                              v
                                                    +------------------+
                                                    | Rockit H.264 VENC|
                                                    +------------------+
                                                              |
                                                              | packet
                                                              v
                                                    +------------------+
                                                    | Packet Sink      |
                                                    | file / queue     |
                                                    | / rtsp           |
                                                    +------------------+
```

RTSP 服务端为三层线程模型（`src/rtsp_server.c`）：

```text
listener  accept + 回收已结束会话
reader    Packet Queue 的唯一消费者，每帧 packetize 一次后扇出给所有客户端
client    每连接一个线程，只做请求/应答，绝不碰队列
```

RTP 的 seq/timestamp 是**流的属性而不是连接的属性**，所以必须"一次打包、多方扇出"。

目标架构：

```text
[v4l2_capture] --NV12--> Frame RingBuffer --NV12--> [encoder thread]
                                                          |
                                                          v
                                                    Packet Queue
                                                          |
                                                          v
                                                    [rtsp / sink]

MPU6050 / Mock Sensor -> Sensor RingBuffer -> OSD / Alarm
```

`--threads` 已经实现左侧的采集线程 + Frame RingBuffer 部分。

## 仓库结构

```text
.
├── Makefile
├── README.md
├── docs/
│   ├── adb-flash.md
│   ├── handoff.md
│   ├── roadmap.md
│   └── status.md
├── scripts/
│   ├── S99gateway
│   ├── diagnose_v4l2.sh
│   ├── flash_image.ps1
│   ├── flash_partition.sh
│   ├── gateway-supervise.sh
│   ├── install_autostart.ps1
│   ├── soak-monitor.sh
│   ├── start_gateway.ps1
│   └── start_rkaiq.sh
├── src/
│   ├── capture_signal.h      共享的 g_stop 声明
│   ├── capture_thread.c/.h   采集线程 + Frame RingBuffer 生产者
│   ├── frame_ring.c/.h       有界帧环形缓冲（覆盖最旧帧）
│   ├── h264_util.c/.h        Annex-B / NAL 工具
│   ├── mpp_encoder.c/.h      Rockit VENC 编码封装
│   ├── packet_queue.c/.h     有界 packet 队列（丢最旧整个 GOP）
│   ├── packet_sink.h         Sink 抽象接口
│   ├── rtp_h264.c/.h         H.264 RTP 打包（纯缓冲，可主机自测）
│   ├── rtsp_proto.c/.h       RTSP 解析 / SDP（纯缓冲，可主机自测）
│   ├── rtsp_server.c/.h      RTSP 服务端（唯一持有 socket 的文件）
│   ├── sink_file.c/.h        文件 Sink
│   ├── sink_queue.c/.h       队列 Sink
│   ├── v4l2_capture.c        V4L2 采集（同步基线 + 诊断工具）
│   └── v4l2_mpp_encode.c     主程序
├── tests/
│   ├── host-stubs/           MinGW 缺 POSIX 头文件时的语法检查桩
│   ├── test_capture_thread.c
│   ├── test_frame_ring.c
│   ├── test_packet_queue.c
│   └── test_rtp_rtsp.c
└── tools/
    └── nv12_to_png.py
```

分层原则：**socket 只出现在 `src/rtsp_server.c` 一个文件里**。协议解析、RTP 打包、
Frame RingBuffer、Packet Queue 都是纯缓冲、无系统依赖，因此全部可以在主机上跑单测。
新增网络功能请沿用这个划分。

## SDK 路径

主程序同时依赖 Rockit MPI 和 MPP。默认路径在 `Makefile` 中配置为：

```makefile
SDK_ROOT ?= /home/aaazhx/luckfox-pico
MPP_ROOT ?= $(SDK_ROOT)/media/mpp/release_mpp_rv1106_arm-rockchip830-linux-uclibcgnueabihf

ROCKIT_INCLUDE_DIR       ?= $(SDK_ROOT)/media/rockit/rockit/mpi/sdk/include
ROCKIT_LIBRARY_DIR       ?= $(SDK_ROOT)/media/out/lib
ROCKIT_ROOT_LIBRARY_DIR  ?= $(SDK_ROOT)/media/out/root/usr/lib
```

包含头文件：

```text
$(MPP_ROOT)/include
$(MPP_ROOT)/include/rockchip
$(ROCKIT_INCLUDE_DIR)
```

链接库：

```text
-lrockit -lrga -lrockchip_mpp -lstdc++ -lpthread -lrt -ldl -lm
```

可执行文件带 `-Wl,-rpath,/oem/usr/lib`，因为板端 Rockchip 的库放在 `/oem/usr/lib`。

如果 SDK 目录不同，只需要修改 `SDK_ROOT`。**注意 Windows 侧没有 SDK，也编不了板端程序**，
只能做主机自测。

## 编译

在 Ubuntu 22.04 虚拟机中执行：

```bash
cd /mnt/hgfs/luckfox_share/rv1103

make clean
make CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
```

生成两个程序：

```text
v4l2_capture
v4l2_mpp_encode
```

`v4l2_capture` 用于摄像头节点诊断和原始 NV12 采集。

`v4l2_mpp_encode` 是主程序，支持 `--sink file|queue|rtsp`。

### 主机自测

不需要开发板，也不需要 SDK。在 Windows（MinGW gcc）或 Linux 上执行：

```bash
make test
```

会构建并依次运行四套测试：

```text
test-packet-queue     43 checks
test-rtp-rtsp         86 checks
test-frame-ring       64 checks
test-capture-thread   13 checks
-----------------------------
                     206 checks, 0 failures
```

只做语法检查（不链接，用于含 socket / V4L2 的文件）：

```bash
make host-syntax
```

注意：MinGW 没有 `linux/videodev2.h`、`sys/mman.h` 和 POSIX socket，
因此 `v4l2_mpp_encode.c` 和 `v4l2_capture.c` 只能在 Linux 侧做语法检查。
详见 `tests/host-stubs/README.md`。

## 板端运行

先推送程序：

```powershell
adb push F:\luckfox_share\rv1103\v4l2_capture /userdata/
adb push F:\luckfox_share\rv1103\v4l2_mpp_encode /userdata/
adb push F:\luckfox_share\rv1103\scripts\start_rkaiq.sh /userdata/

adb shell "chmod +x /userdata/v4l2_capture /userdata/v4l2_mpp_encode /userdata/start_rkaiq.sh"
```

停止冲突程序并启动 3A：

```powershell
adb shell "killall -9 rkipc || true"
adb shell "pidof rkipc"
adb shell "sh /userdata/start_rkaiq.sh"
adb shell "sleep 2"
```

采集一帧：

```powershell
adb shell "/userdata/v4l2_capture -d /dev/video11 -w 1280 -H 720 -f NV12 -n 1 --warmup 30 -o /userdata/frame.nv12"
```

### 三种 Sink

编码到文件（基线，用于回归比对）：

```powershell
adb shell "/userdata/v4l2_mpp_encode -d /dev/video11 -w 1280 -H 720 -n 300 --warmup 30 --sink file -o /userdata/live.h264"
adb pull /userdata/live.h264 F:\luckfox_share\live.h264
```

编码 -> 有界队列 -> 落盘线程（用于验证队列丢帧语义）：

```powershell
adb shell "/userdata/v4l2_mpp_encode -d /dev/video11 -n 300 --sink queue -o /userdata/live.h264"
```

RTSP over TCP 推流（默认端口 8554）：

```powershell
adb shell "setsid nohup /userdata/v4l2_mpp_encode -d /dev/video11 --sink rtsp --rtsp-port 8554 > /userdata/rtsp.log 2>&1 < /dev/null & sleep 2; echo launched"
```

播放（Windows 侧）：

```powershell
ffplay -rtsp_transport tcp rtsp://172.32.0.93:8554/live/0
```

用 `--threads` 把采集搬到独立线程、经 Frame RingBuffer 交给编码：

```powershell
adb shell "/userdata/v4l2_mpp_encode -d /dev/video11 --sink rtsp --threads --ring-slots 4"
```

默认仍是同步流水线；`--threads` 为显式开关。相关参数：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--threads` | 关 | 启用采集线程 + Frame Ring Buffer |
| `--ring-slots N` | 4 | Frame Ring 槽位数（2~64） |
| `--rtsp-port N` | 8554 | RTSP 监听端口 |
| `--sink mode` | `file` | `file` / `queue` / `rtsp` |

### 开机自启

`scripts/install_autostart.ps1` 一键安装；重启后无需人工干预，约 11 秒自动出流。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install_autostart.ps1 -Start
```

诊断入口：

```text
/tmp/gateway-boot.log              启动全过程（含环境变量与原始报错）
/etc/init.d/S99gateway status      监督进程状态
/userdata/gateway.log              网关程序日志
```

## 已知可用条件

- `rkipc` 必须停止。
- `rkaiq_3A_server` 必须在采集前启动。
- 推荐先丢弃 30 帧再保存或编码。
- 当前默认 V4L2 节点为 `/dev/video11`。
- 当前要求驱动返回单 plane NV12。
- 当前主要验证参数为 `1280x720 @ 30 FPS`。

如果采集画面偏绿或过暗，优先检查：

```text
/dev/video11 是否是 rkisp_mainpath
rkaiq_3A_server 是否正在运行
是否在启动 3A 前采集了旧帧
rkipc 是否仍然占用 ISP 或 MPP
```

## 编码 API 说明

当前板端媒体库版本为：

```text
2024-02-20
16e796a4
```

**实际使用的编码 API 是 Rockit MPI**，即：

```text
RK_MPI_VENC_CreateChn
RK_MPI_VENC_SendFrame
RK_MPI_VENC_GetStream
VENC_PACK_S.stPackInfo[]
MB_POOL
```

不是旧版 MPP 的 `mpp_init_ext` / `encode_put_frame`。`docs/handoff.md` 早期章节里
描述旧版 MPP API 的部分已经过时，**以 `src/mpp_encoder.c` 源码为准**。

两个必须注意的点：

- 一帧编码输出可能是**多段**（`u32DataNum` + `stPackInfo[]`），必须遍历所有段，
  否则会丢数据、花屏。
- 旧版 MPP 内核对错误的 context 初始化非常敏感。不要混用不同版本的头文件、
  动态库和内核模块。

## 图像格式约定

- V4L2：NV12，单 plane。
- 1280x720：V4L2 stride 1280，size 1382400。
- MPP 输入：NV12，`MPP_FMT_YUV420SP`。
- H.264 输出：Annex-B 码流。

如果 V4L2 返回 stride 2304，当前采集工具会将可见行重排为紧凑 NV12。最终 V4L2 节点仍应优先协商为 stride 1280。

## NV12 调试工具

Windows 侧可以使用：

```powershell
python .\tools\nv12_to_png.py frame.nv12 frame.png --width 1280 --height 720
```

该工具支持指定 UV 偏移、UV stride 和 UV 顺序，用于排查 NV12/NV21 或 stride 问题。

## ADB 刷写

`boot`、`uboot`、`idblock`、`env` 等未挂载分区可以通过 ADB 和 `dd` 刷写。脚本不会允许在线写入 `rootfs` 或 `userdata`。

详细说明见：

```text
docs/adb-flash.md
```

## 后续路线

完整阶段说明见：

```text
docs/roadmap.md
```

项目级交接说明见：

```text
docs/handoff.md
```

当前验收数据、已修复缺陷根因、构建与自测说明见：

```text
docs/status.md
```

已完成的 RTSP 验收目标：

```text
VLC / ffplay 可以打开 rtsp://172.32.0.93:8554/live/0
1280x720 30 FPS 连续播放
任一客户端断开和重连不影响其他客户端与编码
多客户端（最多 4 路）同拉互不干扰
```

长稳实测：两段共 6 小时 29 分、约 70 万帧、零丢帧。仍缺一次干净收尾的 8 小时正式验收。

接下来：

```text
[x] Packet Sink / File Sink / Packet Queue
[x] RTSP over TCP 多客户端
[x] Frame RingBuffer + 采集线程独立（待上板验证）
[x] 开机服务
[ ] 8 小时长稳正式验收
[ ] Mock Sensor -> OSD 数据面
[ ] 真实 MPU6050 接入
```

## 安全与资源注意事项

- 不要同时运行 `rkipc` 和自定义 MPP 编码程序。
- 不要把损坏或尚未验证的镜像写入正在挂载的分区。
- 不要无上限写 `/userdata`。
- 不要在主循环中执行无限增长的动态分配（Frame RingBuffer 与 Packet Queue 均创建时一次性分配）。
- 不要改回"每个 RTSP 客户端各自取队列"的实现，否则多客户端会互相抢帧导致全员花屏。
- 任何 RTSP handler 都不得持锁调用 `client_send_response()`，否则死锁。
- 旧版 MPP 内核对错误的 context 初始化非常敏感，出错后建议重启开发板清除状态。
