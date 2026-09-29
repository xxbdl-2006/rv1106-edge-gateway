# RV1106 Embedded Linux Edge Gateway

基于 Luckfox Pico Pro/Max（Rockchip RV1106G）的嵌入式 Linux 音视频与多传感器边缘网关。

项目主线是不依赖官方 `rkipc` 黑盒程序，自行实现：

```text
SC3336 -> MIPI CSI -> V4L2 -> NV12 -> MPP H.264 -> Packet Queue -> RTSP
```

**MPU6050、OSD 均已接入并进生产配置**（见下），剩余工作见 `docs/agent-handoff.md`。

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
- **Frame RingBuffer + 采集线程独立**（`--threads`），已上板验证并跑满 8 小时。
- **Sensor 数据面**：`sensor_source` 接口 + 有界环形缓冲 + 姿态解算 + Mock 数据源。
- **MPU6050 驱动**：位翻转 I2C 已上真硅片读通（`WHO_AM_I=0x70`，0 io error）；
  overlay 在这块板上是坏的，因此**不走 `/dev/i2c-4`**，改用 GPIO pin 14/24 位翻转。
- **真实 IMU 源**：`mpu6050_source` 实现 `sensor_source`，`--osd-source mpu6050` 可选。
- **OSD 叠加**：已接进编码流水线并上板验证 PASS（300/300 帧合成成功，**帧率零开销**）。
- **生产配置已开 OSD + 真实 IMU**：`/userdata/gateway.env` 含 `--osd --osd-source mpu6050`，
  30 分钟 54000 帧 30.000 fps 长稳通过，真拉流抽帧确认数值在动态更新。

> 全局最新的完成/未完成清单见 **`docs/agent-handoff.md`**；本文件的这部分不再维护。

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
加 `--osd` 后传感器遥测会被合成进每一帧；`--osd-source` 支持 `mock`（假数据）与
`mpu6050`（**生产配置用的真实源**）。轮询间隔由 `--osd-imu-interval-ms` 控制，默认 100 ms
（这个值不是随便取的：一次 bit-bang 突发 ≈28 ms，按 10 ms 读会把帧率从 30 打到 21.4，
详见 `docs/status.md` §2.6）。

详细的验收数据、已修复缺陷根因和构建自测说明见 `docs/status.md`。

## 硬件与软件环境

| 项目 | 配置 |
| --- | --- |
| 主控 | Rockchip RV1106G，Cortex-A7 + RISC-V + 0.5T NPU |
| 开发板 | Luckfox Pico Pro / Max |
| 摄像头 | SC3336，MIPI CSI，2304x1296 |
| 传感器 | MPU6050（料为 MPU6500，`WHO_AM_I=0x70`），**已接入**，走 GPIO pin 14/24 位翻转 I²C |
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

完整架构（**两条都已实现并上板**）：

```text
[v4l2_capture] --NV12--> Frame RingBuffer --NV12--> [encoder thread]
                                                          |
                                                          v
                                                    Packet Queue
                                                          |
                                                          v
                                                    [rtsp / sink]

MPU6050(bit-bang I²C) / Mock Sensor -> Sensor RingBuffer -> Sensor Attitude
                                    -> OSD (1bpp canvas) -> 合成进编码器输入的私有副本
```

`--threads` 实现左侧的采集线程 + Frame RingBuffer 部分；`--osd --osd-source mpu6050`
实现下方支线。两条线的接缝只有一处：`SENSOR_SRC` **不进** `MEDIA_SRC`。

> **告警（Alarm）未实现**：当前只有 TILT / MAG 两个阈值标记烧进叠加层，没有独立告警通道。

## 仓库结构

```text
.
├── Makefile
├── README.md
├── docs/
│   ├── agent-handoff.md      ★ 最新交接文档（现状以此为准）
│   ├── status.md             实测数据与根因分析
│   ├── handoff.md            历史快照（写于 RTSP 阶段，已标注）
│   ├── roadmap.md            阶段规划
│   ├── mpu6050-wiring.md     接线、overlay 证伪、bit-bang、上板步骤
│   ├── luckfox-pico-max-pinout.md
│   ├── next-tests.md
│   └── adb-flash.md
├── scripts/
│   ├── gateway.env           ★ 生产命令行的唯一来源
│   ├── S99gateway            开机自启（接管 rkipc、启 3A、拉 supervisor）
│   ├── gateway-supervise.sh  看护进程
│   ├── install_autostart.ps1 安装三件套到 /userdata
│   ├── verify-osd.sh / .cmd  OSD 上板一键验证
│   ├── verify-imu.sh         真实 IMU 一键验证
│   ├── verify-mpu6050.sh     MPU6050 驱动验证
│   ├── imu-soak.sh           带真实 IMU 的长稳（不依赖 imu-sample）
│   ├── start-2h-soak.sh      长稳（2h/8h）
│   ├── fps-osd-compare.sh    同场次帧率对照/扫描
│   ├── soak-monitor.sh       资源采样到 /userdata/soak.csv
│   ├── gen_osd_font.py       点阵字体生成
│   ├── adb-helper.sh         adb_sh 封装（避免并行杀 daemon）
│   ├── diagnose_v4l2.sh
│   ├── start_gateway.ps1 / start_rkaiq.sh
│   ├── flash_image.ps1 / flash_partition.sh
│   └── probe-i2c3-*.sh       早期 I²C 引脚探测（历史）
├── src/
│   ├── v4l2_capture.c        V4L2 采集（同步基线 + 诊断工具）
│   ├── v4l2_mpp_encode.c     主程序
│   ├── mpp_encoder.c/.h      Rockit VENC 编码封装
│   ├── capture_thread.c/.h   采集线程 + Frame RingBuffer 生产者
│   ├── capture_signal.h      共享的 g_stop 声明
│   ├── frame_ring.c/.h       有界帧环形缓冲（覆盖最旧帧）
│   ├── packet_queue.c/.h     有界 packet 队列（丢最旧整个 GOP）
│   ├── packet_sink.h         Sink 抽象接口
│   ├── sink_file.c/.h        文件 Sink
│   ├── sink_queue.c/.h       队列 Sink
│   ├── rtsp_server.c/.h      RTSP 服务端（唯一持有 socket 的文件）
│   ├── rtsp_proto.c/.h       RTSP 解析 / SDP（纯缓冲，可主机自测）
│   ├── rtp_h264.c/.h         H.264 RTP 打包（纯缓冲，可主机自测）
│   ├── h264_util.c/.h        Annex-B / NAL 工具
│   ├── sensor_source.h       Sensor 抽象接口
│   ├── mock_sensor.c/.h      Mock 数据源
│   ├── mpu6050_source.c/.h   真实 IMU 源（convert 纯算术，host 可测）
│   ├── mpu6050.c/.h          寄存器编解码、标定、量程换算
│   ├── mpu6050_i2c.c         bit-bang I²C 驱动层
│   ├── mpu6050_gpio.h        引脚配置
│   ├── i2c_bitbang.c/.h      bit-bang 时序
│   ├── gpio_sysfs.c/.h       sysfs GPIO 封装
│   ├── sensor_ring.c/.h      有界样本环
│   ├── sensor_attitude.c/.h  pitch/roll 解算
│   ├── sensor_math.h         无 libm 的三角/开方
│   ├── osd_font.c/.h         5x7 点阵字体（生成物）
│   ├── osd_overlay.c/.h      1bpp 画布 + NV12 合成
│   ├── osd_format.c/.h       定点格式化
│   ├── osd_telemetry.c/.h    遥测行
│   ├── osd_feed.c/.h         50Hz 采样
│   └── osd_annotate.c/.h     私有副本里合成
├── tests/
│   ├── host-stubs/           MinGW 缺 POSIX 头文件时的语法检查桩
│   ├── test_packet_queue.c   test_rtp_rtsp.c
│   ├── test_frame_ring.c     test_capture_thread.c
│   ├── test_mpu6050.c        test_i2c_bitbang.c
│   ├── test_mpu6050_source.c test_sensor.c
│   └── test_osd.c            test_osd_pipeline.c
└── tools/
    ├── imu_sample.c          板端取数（走 sensor_source 接口）
    ├── mpu6050-probe.c       板端驱动探测
    ├── i2c-bitbang.py        ★ 黄金对照（Python 版，同板同芯片同引脚）
    └── nv12_to_png.py
```

分层原则：**socket 只出现在 `src/rtsp_server.c` 一个文件里**。协议解析、RTP 打包、
Frame RingBuffer、Packet Queue、传感器数学、OSD 各层都是纯缓冲、无系统依赖，
因此全部可以在主机上跑单测。新增网络功能请沿用这个划分。

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

会构建并依次运行**十个测试套件**（外加 `board-flags` 与 `host-syntax-can-fail` 两项标志检查）：

```text
test-packet-queue      43 checks
test-rtp-rtsp          86 checks
test-frame-ring        64 checks
test-capture-thread    13 checks
test-mpu6050          105 checks
test-i2c-bitbang       50 checks
test-mpu6050-source    28 checks
test-sensor           622~697 checks（每次运行浮动，见下）
test-osd              143 checks
test-osd-pipeline      39 checks
--------------------------------------
                     1205 checks, 0 failures（本轮实测）
```

> `test-sensor` 的检查项数量**每次运行都不固定**，全部 PASS。原因是有个真线程的
> 生产者/消费者用例，每消费一个样本计一次 CHECK，消费多少取决于调度。
> **这不是失败，别去「修」它；也别把某个固定数字写进文档。**
> 统计检查项数也不要 `grep "checks="`（长行会截断），宜单独跑二进制。

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
| `--osd` | 关 | 把传感器遥测合成进每一帧 |
| `--osd-source src` | `mock` | `mock`（假数据）/ `mpu6050`（**生产用的真实源**） |
| `--osd-imu-interval-ms N` | 100 | IMU 总线读取间隔（0~10000，0=用源默认）。**日常不用写** |

### OSD 叠加

```powershell
# 真实 IMU 叠加（生产配置用的就是这条）
adb shell "/userdata/v4l2_mpp_encode -d /dev/video11 -w 1280 -H 720 --warmup 30 \
  --sink rtsp --rtsp-port 8554 --threads --ring-slots 4 --osd --osd-source mpu6050"
```

面板内容：`PITCH/ROLL`、`ACC/TEMP`、`STATUS FRAME/FPS`、`IMU MPU6050 OK E=0`。

> 🔴 `--osd-imu-interval-ms` 默认 **100 ms 不是随便取的**：一次 bit-bang I²C 突发
> 实测 ≈ **28 ms**，按 10 ms 读会把帧率从 30 打到 21.4。三档对照见 `docs/status.md` §2.6。
>
> ⚠️ 编码器打印 `annotated=N` **只证明它做了合成，不证明客户端收到了**
> （`--sink rtsp` 没客户端时 RTP 根本不发包）。**上生产配置后必须真拉一次流抽帧看**：
> `ffmpeg -rtsp_transport tcp -i rtsp://172.32.0.93:8554/live/0 -t 8 -frames:v 2 out-%02d.png`
> 且**两帧数值要在变**，否则分不清活数据和静态渲染。

### 开机自启

`scripts/install_autostart.ps1` 一键安装；重启后无需人工干预，约 11 秒自动出流。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install_autostart.ps1 -Start
```

> 🔴 **生产命令行的唯一来源是仓库里的 `scripts/gateway.env`**，由安装脚本推成板上的
> `/userdata/gateway.env`。**改配置改仓库那份再重装，不要在板上直接改** ——
> 板上那份是副本，下一次重装会静默覆盖它，而且没有任何报错。
> `imu-soak.sh` / `start-2h-soak.sh` 也从同一个文件 source 参数，避免多处各写一份。

诊断入口：

```text
/tmp/gateway-boot.log              启动全过程（含环境变量与原始报错）
/etc/init.d/S99gateway status      监督进程状态
/userdata/gateway.log              网关程序日志
```

叠加层是活的还是降级了：

```powershell
adb shell "grep -E 'IMU attached|IMU unavailable' /userdata/gateway.log | tail -2"
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

项目级交接说明：

```text
docs/agent-handoff.md   ★ 最新，现状以此为准
docs/handoff.md         历史快照（写于 RTSP 阶段，已在文首标注）
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

长稳实测：**8 小时干净收尾（864,001 帧 / 30.00fps / 零丢帧 / 零泄漏）**，
另有带宽真实 IMU 的 30 分钟长稳（54000 帧 / 30.000 fps）。

接下来：

```text
[x] Packet Sink / File Sink / Packet Queue
[x] RTSP over TCP 多客户端
[x] Frame RingBuffer + 采集线程独立（8 小时长稳通过）
[x] 开机服务
[x] 8 小时长稳正式验收（864,001 帧零丢帧零泄漏）
[x] Mock Sensor -> OSD 数据面
[x] OSD 接进编码流水线并上板验证
[x] 真实 MPU6050 接入（src/mpu6050_source.c）
[x] OSD + 真实 IMU 并入默认生产配置（gateway.env，30 分钟长稳）
[~] 冷启动（上电 → 3A 首次收敛）完整验证 —— fail-soft 已就位，只差自然断电重启实测
[ ] 网络断开/恢复专项测试
[ ] 端到端延迟的正式测量（目前只有 ffplay ~0.75s 粗测）
[ ] 架构图与演示视频

更新版本的清单（含每项证据）见 `docs/agent-handoff.md`。
```

## 安全与资源注意事项

- 不要同时运行 `rkipc` 和自定义 MPP 编码程序。
- 不要把损坏或尚未验证的镜像写入正在挂载的分区。
- 不要无上限写 `/userdata`。
- 不要在主循环中执行无限增长的动态分配（Frame RingBuffer 与 Packet Queue 均创建时一次性分配）。
- 不要改回"每个 RTSP 客户端各自取队列"的实现，否则多客户端会互相抢帧导致全员花屏。
- 任何 RTSP handler 都不得持锁调用 `client_send_response()`，否则死锁。
- 旧版 MPP 内核对错误的 context 初始化非常敏感，出错后建议重启开发板清除状态。
