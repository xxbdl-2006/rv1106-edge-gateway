# RV1106 Edge Gateway Handoff

> ## ⚠️ 状态：本文档是历史快照，**现状以 `docs/agent-handoff.md` 为准**
>
> 本文写于 **RTSP 阶段**（RTSP 刚跑通、OSD 与 MPU6050 尚未动工），此后项目又走完了
> Frame RingBuffer/采集线程、开机自启、8 小时长稳、Sensor 数据面、OSD 上板、
> 真实 MPU6500 进生产配置。**§3 / §5 / §7 / §13 / §14 / §16 / §19 已按 2026-09-29 的
> 实际状态重写**，其余章节（尤其 §11 的 MPP 调用链、§12 的早期根因）保留为历史记录。
>
> - 想知道**现在能跑到什么程度** → `docs/agent-handoff.md`（唯一权威）
> - 想知道**实测到了什么数据** → `docs/status.md`
> - 想知道**当年为什么这么设计** → 本文档
>
> 判断依据：若本节与文档其余内容冲突，以本节与 `agent-handoff.md` 为准。

## 1. 文档目的

本文档用于把当前项目完整交接给下一位开发者或 AI。内容覆盖：

- 项目目标和当前完成度。
- 硬件、系统和开发环境。
- 当前代码架构、文件职责和运行流程。
- 已验证的构建、推送和运行命令。
- 已有问题的根因和规避方法。
- 下一阶段的工作顺序和验收标准。

本文档描述的是当前可用基线，不是最终架构。

## 2. 项目目标

在 Luckfox Pico Pro/Max（Rockchip RV1106G）上构建一套工业边缘音视频与多传感器网关。

核心技术目标：

- 摆脱官方 `rkipc` 黑盒应用。
- 使用标准 Linux V4L2 框架采集 SC3336 视频。
- 使用 Rockchip MPP 进行硬件 H.264 编码。
- 构建低延迟媒体流水线。
- 接入 I2C 传感器和 OSD/告警。
- 提供标准 RTSP 视频输出。

目标媒体链路：

```text
SC3336 -> MIPI CSI -> ISP/CIF -> V4L2 -> NV12 -> MPP H.264
       -> Packet Queue -> RTSP -> VLC/NVR
```

目标传感器链路（**已实现，总线形式与原设想不同**）：

```text
MPU6050 -> bit-bang I²C (gpio70/71) -> Sensor RingBuffer -> OSD
```

> 原设想走 `/dev/i2c-4`，但运行时 device-tree overlay 在这块板上是坏的，
> 该节点不存在；改为用户态位翻转 GPIO。见 §5 与 `docs/mpu6050-wiring.md`。
> **告警（Alarm）部分未做**：当前只有 TILT / MAG 两个阈值标记烧进叠加层，没有独立告警通道。

## 3. 当前完成度

| 阶段 | 状态 | 说明 |
| --- | --- | --- |
| 系统启动 | 已完成 | Buildroot Linux 5.10.160 正常启动 |
| ADB/RNDIS | 已完成 | Windows ADB 和 RNDIS 网络可用 |
| SD/分区 | 已完成 | `/dev/mmcblk1` 多分区映射已确认 |
| SPI0 | 已完成 | `/dev/spidev0.0` 已验证 |
| SC3336 | 已完成 | MIPI CSI、ISP、V4L2 节点正常 |
| 3A | 已完成 | `rkaiq_3A_server --silent` 可稳定曝光和白平衡 |
| V4L2 采集 | 已完成 | 1280x720 NV12，300 帧约 30 FPS |
| MPP 编码 | 已完成 | 硬件 H.264 编码 |
| 文件播放 | 已完成 | 生成的 H.264 可由 VLC 正常播放 |
| Packet Sink/Queue | 已完成 | File Sink、Queue Sink、有界丢帧队列 |
| RTSP over TCP | 已完成 | 多客户端 fan-out，已上板验证 |
| Frame RingBuffer | 已完成 | `--threads` 采集线程独立，**已上板验证**（8 小时长稳走的即此路径） |
| 开机自启 | 已完成 | 冷启动实测：**上电到出流约 19 秒**（2026-09-29 修复竞态后；旧记录的「11 秒」已作废，见 §13） |
| 长稳测试 | 已完成 | **8 小时干净收尾**：864,001 帧 / 30.00fps / 零丢帧零泄漏（2026-09-28） |
| Sensor 数据面 | 已完成 | `sensor_source` 接口 + 有界样本环 + 姿态解算 + Mock 源（`test-sensor`） |
| MPU6050 驱动 | 已完成 | 软件 bit-bang I²C 读通真硅片：`WHO_AM_I=0x70`(MPU6500)、0 io error、静止 `\|a\|≈0.99g` |
| OSD 叠加 | 已完成 | 1bpp 画布 + 5x7 点阵，已上板验证 PASS（300/300 帧合成，**帧率零开销**） |
| **真实 IMU 源** | 已完成 | `mpu6050_source` 实现 `sensor_source`，`--osd-source mpu6050` 可选（2026-09-29） |
| **生产配置单一来源** | 已完成 | 生产命令行收敛到仓库 `scripts/gateway.env`，安装脚本推它、长稳脚本 source 它 |

详细的验收数据见 `docs/status.md`，最新的完成清单与证据见 `docs/agent-handoff.md`。

### 3.1 RTSP 阶段的实际交付

```text
--sink file   encoder -> file，与基线逐字节一致
--sink queue  encoder -> 有界队列 -> 落盘线程，用于回归验证队列
--sink rtsp   encoder -> 有界队列 -> RTSP over TCP 多客户端
```

线程模型（`src/rtsp_server.c`）分三层，**不要改回每客户端各自取队列**：

```text
listener  accept + 回收已结束会话
reader    队列的唯一消费者，每帧 packetize 一次后扇出给所有客户端
client    每连接一个线程，只做请求/应答，绝不碰队列
```

原因是 RTP 的 seq/timestamp 是**流的属性而不是连接的属性**。若每个客户端各自
`acquire()`，帧会被随机瓜分，所有观众同时花屏。同理，PLAY 时不重置 RTP 时间戳，
因为那会打断正在观看的其他人。


## 4. Git 仓库

仓库：

```text
https://github.com/xxbdl-2006/rv1106-edge-gateway
```

默认分支：

```text
main
```

初始可用基线提交：

```text
c699ac0 Initial RV1106 edge gateway baseline
```

仓库当前为私有状态。

## 5. 硬件与系统

| 项目 | 当前配置 |
| --- | --- |
| 主控 | RV1106G |
| CPU | Cortex-A7 + RISC-V + 0.5T NPU |
| 板卡 | Luckfox Pico Pro / Max |
| 摄像头 | SC3336 |
| 摄像头接口 | MIPI CSI |
| 摄像头分辨率 | 2304x1296 |
| 传感器 | MPU6050（实际料为 **MPU6500**，`WHO_AM_I=0x70`；寄存器布局同 MPU6050），**已接入** |
| 传感器总线 | **软件 bit-bang I²C**：GPIO **pin 14 (SDA) / pin 24 (SCL)** |
| 启动介质 | TF/SD 卡 |
| 根文件系统 | Buildroot Linux，ext4，只读 rooffs |
| 内核版本 | 5.10.160 |
| 用户态架构 | `armv7l GNU/Linux` |

分区映射：

```text
env      -> /dev/mmcblk1p1
idblock  -> /dev/mmcblk1p2
uboot    -> /dev/mmcblk1p3
boot     -> /dev/mmcblk1p4
userdata -> /dev/mmcblk1p5
rootfs   -> /dev/mmcblk1p6
```

> **为什么不用 `/dev/i2c-4`：** 这块板上运行时 device-tree overlay 是**坏的** ——
> 写一个合法的 dtbo 和写一段 GARBAGE 都返回 `rc=0`，写入路径根本没有生效。
> 因此 I²C 控制器始终没有出现在设备树里，`/dev/i2c-4` 不存在。改为在用户态用
> sysfs GPIO **位翻转（bit-bang）** 直接抖 pin 14/pin 24。
> 验证手法值得记住：**验证一条写入路径是否生效，就喂给它必然非法的输入，看它报不报错。**
> 细节见 `docs/mpu6050-wiring.md`。

## 6. 开发环境

Windows 11：

- ADB
- RNDIS
- VLC
- VMware 共享目录 `F:\luckfox_share`

Ubuntu 22.04 虚拟机：

- SDK：`/home/aaazhx/luckfox-pico`
- 工作目录：`/mnt/hgfs/luckfox_share/rv1103`
- 交叉工具链：`arm-rockchip830-linux-uclibcgnueabihf-gcc`

MPP SDK：

```text
MPP_ROOT=/home/aaazhx/luckfox-pico/media/mpp/release_mpp_rv1106_arm-rockchip830-linux-uclibcgnueabihf
```

头文件：

```text
$(MPP_ROOT)/include
$(MPP_ROOT)/include/rockchip
```

库：

```text
$(MPP_ROOT)/lib/librockchip_mpp.so
```

## 7. 当前架构

### 7.1 两条流水线

```text
视频主线
  /dev/video11 --V4L2 mmap--> 紧致化 NV12 1280x720
       |
       |  [--threads] 采集线程 --> frame_ring --> 主线程
       v
  Rockit MPI VENC (H.264 Baseline, CBR 2Mbps)
       |
       v
  Packet Sink:  file | queue | rtsp
                                    |
                                    v
                          RTSP over TCP 多客户端 (上限 4)

传感器支线（与视频流水线刻意零交集）
  MPU6050(bit-bang I²C, gpio70/71) --10Hz--> sensor_source
       --> sensor_ring --> sensor_attitude(pitch/roll)
       --> osd_feed (50Hz 采样)
       --> osd_overlay (1bpp 画布 + NV12 合成)
       --> 合成进编码器输入的私有副本
```

> 两条线的接缝只有一处：`SENSOR_SRC` **不进** `MEDIA_SRC`，传感器代码不碰视频缓冲。

### 7.2 三层网络线程（`src/rtsp_server.c`）

```text
listener  accept + 回收已结束会话
reader    Packet Queue 的唯一消费者，每帧 packetize 一次后扇出给所有客户端
client    每连接一个线程，只做请求/应答，绝不碰队列
```

原因是 RTP 的 seq/timestamp 是**流的属性而不是连接的属性**。

### 7.3 代码职责

```text
v4l2_capture.c         独立 V4L2 采集诊断程序（也以 V4L2_CAPTURE_NO_MAIN 被主程序复用）
v4l2_mpp_encode.c      主程序：参数解析、主循环、OSD 接缝、sink 选择
capture_thread.c/.h    采集线程，经回调把 NV12 交给 frame_ring
frame_ring.c/.h        有界 NV12 帧环，创建期一次性分配，push 永不阻塞
mpp_encoder.c/.h       Rockit MPI VENC 封装（编码配置、取码流）
packet_queue.c/.h      有界 packet 队列（满时丢最旧整个 GOP）
packet_sink.h          Sink 抽象接口（file / queue / rtsp）
rtsp_proto.c/.h        RTSP 解析 / SDP（纯缓冲，可主机自测）
rtp_h264.c/.h          H.264 RTP 打包（纯缓冲，可主机自测）
rtsp_server.c/.h       RTSP 服务端（**唯一持有 socket 的文件**）
sensor_source.h        Sensor 抽象接口
mpu6050_source.c/.h    真实 MPU6050 源（convert 是纯算术，host 可测；open/read/close 在 __linux__ 内）
mpu6050.c/.h           驱动层：寄存器编解码、标定、量程换算
mpu6050_i2c.c          bit-bang I²C 时序（gpio_sysfs.c 之上）
sensor_ring.c/.h       有界样本环（head/tail/count 模型）
sensor_attitude.c/.h   pitch/roll 解算
mock_sensor.c          假数据源（故障注入、wave 模式）
osd_font / osd_overlay / osd_format / osd_telemetry / osd_feed / osd_annotate
                       OSD 各层（点阵字体、1bpp 画布合成、定点格式化、遥测行、采样、私有副本合成）
```

## 8. 构建

在 Ubuntu 虚拟机中执行：

```bash
cd /mnt/hgfs/luckfox_share/rv1103

make clean
make CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
```

输出：

```text
v4l2_capture
v4l2_mpp_encode
```

如果 SDK 路径不同，修改 `Makefile` 中的 `MPP_ROOT`。

## 9. 板端运行

推送：

```powershell
adb push F:\luckfox_share\rv1103\v4l2_capture /userdata/
adb push F:\luckfox_share\rv1103\v4l2_mpp_encode /userdata/
adb push F:\luckfox_share\rv1103\scripts\start_rkaiq.sh /userdata/

adb shell "chmod +x /userdata/v4l2_capture /userdata/v4l2_mpp_encode /userdata/start_rkaiq.sh"
```

停止官方 rkipc：

```powershell
adb shell "killall -9 rkipc || true"
adb shell "pidof rkipc"
```

启动 3A：

```powershell
adb shell "sh /userdata/start_rkaiq.sh"
adb shell "sleep 2"
```

采集一帧：

```powershell
adb shell "/userdata/v4l2_capture -d /dev/video11 -w 1280 -H 720 -f NV12 -n 1 --warmup 30 -o /userdata/frame.nv12"
```

编码 300 帧：

```powershell
adb shell "/userdata/v4l2_mpp_encode -d /dev/video11 -w 1280 -H 720 -n 300 --warmup 30 -o /userdata/live.h264"
adb pull /userdata/live.h264 F:\luckfox_share\live.h264
```

## 10. 已验证结果

V4L2 参数：

```text
Video Device : /dev/video11
Driver       : rkisp_v7
Entity       : rkisp_mainpath
Format       : NV12
Resolution   : 1280x720
Planes       : 1
Stride       : 1280
Size Image   : 1382400
```

连续采集：

```text
Captured     : 300 frames
Average FPS  : approximately 30
```

H.264：

```text
文件由 MPP 硬件编码器生成
文件可以由 VLC 正常播放
未观察到明显花屏或解码失败
```

## 11. 关键调用链

> ⚠️ 本节「MPP」小节记录的是**早期 MPP 版本**的调用链，**已不是当前实际使用的 API**。
> 现在实际用的是 **Rockit MPI VENC**：
> `RK_MPI_VENC_CreateChn` / `RK_MPI_VENC_SendFrame` / `RK_MPI_VENC_GetStream`
> （`VENC_PACK_S.stPackInfo[]` + `MB_POOL`）。**以 `src/mpp_encoder.c` 源码为准。**
> V4L2 小节仍然准确。

V4L2：

```text
open("/dev/video11")
VIDIOC_QUERYCAP
VIDIOC_S_FMT
VIDIOC_REQBUFS
VIDIOC_QUERYBUF
mmap
VIDIOC_QBUF
VIDIOC_STREAMON
VIDIOC_DQBUF
VIDIOC_QBUF
VIDIOC_STREAMOFF
```

MPP（**历史，见上方警告**）：

```text
mpp_buffer_group_get_internal
mpp_buffer_get
mpp_create
mpp_init_ext
MPP_SET_OUTPUT_TIMEOUT
mpp_enc_cfg_init
MPP_ENC_GET_CFG
MPP_ENC_SET_CFG
mpp_frame_init
encode_put_frame
mpp_frame_deinit
packet descriptor
encode_get_packet
write H.264
```

## 12. 已知问题和根因

### 12.1 停止 rkipc 后画面偏绿

原因：

- 只停止了 `rkipc`，没有运行独立的 3A 服务。
- 或者编码了 3A 收敛前保存的旧 NV12 帧。

解决：

- 启动 `/userdata/start_rkaiq.sh`。
- 丢弃前 30 帧。
- 不要复用旧的 `seq.nv12`。

### 12.2 V4L2 stride 为 2304

原因：

- `G_FMT` 继承了旧的 `bytesperline`。
- 修改分辨率时没有清零 `bytesperline` 和 `sizeimage`。

解决：

- `S_FMT` 前将 `bytesperline` 和 `sizeimage` 清零。
- 目标是 `stride=1280, size=1382400`。

### 12.3 MPP 初始化触发内核崩溃

错误日志：

```text
MppCtxType error 0
```

原因：

- 在 `mpp_init_ext` 之前调用了 MPP control。
- 使用了旧版 `mpp_init`。

解决：

- 使用该 SDK 提供的 `mpp_init_ext` 和 `vcodec_attr`。
- `mpp_create -> mpp_init_ext -> configure/control`。

当前板端 MPP 版本：

```text
2024-02-20
16e796a4
```

### 12.4 H.264 输出文件为 0 字节

原因：

- 当前 SDK 的 packet 获取接口与新版 MPP 不同。
- packet 描述符必须由调用者提供并交给 `encode_get_packet`。

当前代码已针对此版本实现兼容，不要直接替换为其他 MPP 官方示例。

## 13. 当前限制（2026-09-29 重写）

> 本节原先写的「没有 Packet Queue / 没有 RTSP / 没有 Frame RingBuffer / 没有 MPU6050 /
> 3A 依赖人工启动 / 没有开机服务」**全部已实现**，是最误导人的一处。下面是**真正**剩下的限制。

**功能性缺口**

- ✅ **冷启动已补完验证，并发现另一半是坏的**：2026-09-29 补做「上电 → 3A 首次收敛」，
  结果**每次上电都不出流** —— 接管逻辑与 `RkLunch.sh` 后台启动 rkipc 的竞态。
  已用 `/proc/uptime` 做开机/重启判别修复并复验 PASS。详见 `docs/status.md` §2.8。
  **「开机 11 秒出流」这个旧数字随之作废，正确值是上电到出流约 19 秒。**
- ✅ **网络断开/恢复专项测试已做**（2026-09-29，见 `docs/status.md` §2.9）：
  把 RNDIS 网口捅掉 20 秒 —— 网关不崩、不泄漏、编码不停、死会话被回收、恢复后新客户端可连。
  ⚠️ 代价：**原客户端扛不过去**（被服务端判为 stalled 后重置），必须重连。
- ✅ **端到端延迟已正式测量**（2026-09-29，见 `docs/status.md` §2.10）：
  网关侧稳态 **≲50ms**（两个队列 `peak_depth=1`、无漂移），
  新观众首帧等待 **1.12~2.12s**（IDR 门控 0~1s + 客户端地板 ~1.1s）。
  **旧记录的「ffplay 约 0.75s」作废** —— 它两个都不是。
- ✅ **架构图已产出**（2026-09-29）：`docs/architecture.html`，自包含 HTML，
  按代码实际结构画（含一处纠正：`sensor_ring.c` **不在生产路径上**）。
- **演示视频未产出**。

**设计上刻意保留的边界**

- **传感器量程不可配置**：偏置是**原始计数**，只在 ±2g / ±250dps 下有意义；
  `mpu6050_apply_calibration()` 会显式拒绝其他量程（静默减错量程会得到 0.98g，
  落在所有容差内而且是错的）。
- **姿态是相对「标定时的安装角」**，不是相对世界水平：单点标定分不清倾斜和零偏，
  X/Y 的偏置里本来就含那 9.3° 的模块倾斜。要真正分开得做六面翻转。
- **总线错误不加重试**：30 分钟 15057 次采样里 1 次（≈7e-5），无法在板上确定性触发，
  一段没被验证过的错误恢复代码本身就是负债。程序行为是标记 stale + 沿用上一姿态。

**性能代价（已实测，非缺陷）**

- 一次 bit-bang 14 字节突发 **≈28 ms**（总线约 5kHz），因此轮询间隔默认 **100 ms** 而不是每帧。
- 加了 IMU 叠加后 **CPU 32%**（基线 14~17%）、**温度 55.9°C**（基线 52.5°C，+3.4°C）。
  要省 CPU 可把间隔调到 200 ms。
- VLC 播放仍有卡顿，经 verbose 日志确认是 **VLC 自身 D3D11VA 硬解**问题，与服务端无关；
  ffplay / ffmpeg 全程正常。

## 14. 下一阶段任务（2026-09-29 重写）

1–10 全部已完成。原先的第 7 项标「待上板验证」、第 8/9 项为待办、第 10 项标「进行中」，
**现均已上板验证并进入生产配置**：

| # | 任务 | 现状 |
| --- | --- | --- |
| 1–6 | Baseline / Packet Sink / File+Queue Sink / RTSP 单客户端 / RTP 打包 / 多客户端重连 | ✅ 已完成 |
| 7 | Frame RingBuffer + 编码线程 | ✅ **已上板验证**（8 小时长稳走此路径） |
| 8 | Mock Sensor + OSD 数据面 | ✅ 已完成，`test-sensor` / `test-osd` / `test-osd-pipeline` 覆盖 |
| 9 | 接入真实 MPU6050 | ✅ **已进生产配置**（`--osd-source mpu6050`，真实数据已从 RTSP 抽帧确认在变） |
| 10 | 开机服务 + 8 小时稳定性测试 | ✅ 上电到出流约 19 秒（竞态已修，见 §13）；**8 小时干净收尾**（864,001 帧 / 零丢帧 / 零泄漏） |

**剩下的是 P2 工程化收尾**（都是文档与素材，不是功能）：

```text
[x] README.md / docs/handoff.md 过时表述（2026-09-29 已全量对齐现状）
[x] docs/status.md 补 OSD 上板与真实 IMU 章节（另补 §2.6/§2.7/§2.8 三条根因）
[x] 冷启动（上电 → 3A 首次收敛）完整验证 —— 已做（含物理断电重启），并发现+修复严重缺陷（§2.8）
[x] restart 端到端回归 —— PASS，接管 4 秒，零白等
[x] 网络断开/恢复专项测试 —— PASS（网口捅掉 20s），见 `docs/status.md` §2.9
[x] 端到端延迟的正式测量 —— PASS（网关侧 ≲50ms；首帧等待 1.12~2.12s），见 `docs/status.md` §2.10
[x] 架构图 —— `docs/architecture.html`
[ ] 演示视频
```

已完成项的实测数据和根因分析见：

```text
docs/status.md
```

**最新、最全的完成清单与每项证据**见：

```text
docs/agent-handoff.md
```

详细路线见：

```text
docs/roadmap.md
```

## 15. RTSP 验收标准（2026-09-29 全部通过）

| 项 | 标准 | 实测 | 结论 |
| --- | --- | --- | --- |
| URL | `rtsp://172.32.0.93:8554/live/0` | 可用，VLC / ffplay / ffmpeg 均可播 | ✅ |
| Codec / 分辨率 | H.264 / 1280x720 | H.264 Baseline，CBR 2Mbps | ✅ |
| 帧率 | 30 FPS | **30.000 fps**（8 小时长稳保持） | ✅ |
| 延迟（已在途的帧） | < 1s | **网关增加值 ≲50ms**（33ms 排队 + 19ms 网络），无漂移 | ✅ |
| 首帧等待（新观众） | < 1s | **1.12~2.12s**（IDR 门控 0~1s + 客户端地板 ~1.1s） | ⚠️ 见 status.md §2.10 |
| 客户端断开不阻塞编码 | 必须 | 无客户端时队列正常丢旧 GOP | ✅ |
| 客户端重连可恢复 | 必须 | 重连后从下一个 IDR 开始 | ✅ |
| 30 分钟播放不崩溃 | 必须 | 54004 帧 / 0 empty（另有 8 小时长稳） | ✅ |
| 多客户端 | 加分项 | 4 路并发，关任一路不影响其他 | ✅ |

## 16. 传感器验收标准（2026-09-29 实测重写）

| 项 | 原定标准 | 实测结果 | 结论 |
| --- | --- | --- | --- |
| 设备 | `/dev/i2c-4` | **不存在**（overlay 坏，改用 sysfs GPIO bit-bang pin14/24） | 已改设计 |
| 地址 | `0x68` 或 `0x69` | `0x68: present` | 通过 |
| `WHO_AM_I` | `0x68` | **`0x70`**（料是 MPU6500，寄存器布局同 MPU6050） | 通过（值需更正） |
| 静止加速度模长 | 约 1g | 校准后 `\|a\| = 0.990~0.995 g`（host 0.9999） | 通过 |
| 陀螺仪零偏 | 校准后近 0 | 偏置已固化进 `src/mpu6050.h` | 通过 |
| 采样率 | 100 Hz | **芯片自采样 100 Hz，总线读取 10 Hz（100 ms）** | 已改设计（见下） |
| 连续运行 | 10 分钟无 I²C 错误 | **30 分钟 15057 次采样 1 次错误**（≈7e-5），无泄漏无崩溃 | 通过 |

**「采样率」这条必须更正**：原标准假设总线能跟上 100 Hz，实测**不能** ——
一次 bit-bang 14 字节突发 ≈ **28 ms**（总线约 5kHz），100 Hz 需要 10 ms 周期。
按 100 Hz 读会把帧率从 30 打到 21.4；100 ms 间隔下每帧只摊 9.3 ms，帧率**零损失**。
所以是「芯片 100 Hz、总线 100 ms」，两件事分开看。

`--osd-imu-interval-ms N` 是留给换板子的旋钮，日常不用写（默认 100 ms）。

## 17. 调试入口

查看 V4L2 拓扑：

```powershell
adb shell "v4l2-ctl --list-devices"
adb shell "media-ctl -p -d /dev/media0"
adb shell "media-ctl -p -d /dev/media1"
```

查看视频格式：

```powershell
adb shell "v4l2-ctl -d /dev/video11 --all"
```

检查 3A：

```powershell
adb shell "pidof rkaiq_3A_server"
```

检查 rkipc：

```powershell
adb shell "pidof rkipc"
```

查看内核 MPP 错误：

```powershell
adb shell "dmesg | tail -n 100"
```

把 NV12 转为 PNG：

```powershell
python .\tools\nv12_to_png.py frame.nv12 frame.png --width 1280 --height 720
```

## 18. 开发注意事项

- 不要在未验证的情况下修改当前可用的 MPP packet 生命周期。
- 不要同时运行 `rkipc` 和自定义 MPP 编码程序。
- 不要在启动 3A 前采集需要验收的帧。
- 不要无限写 `/userdata`。
- 不要把正在挂载的 `rootfs` 或 `userdata` 在线 `dd`。
- 不要在主循环中无限分配内存。
- 修改 DTS 后必须重新执行 `./build.sh firmware`。
- DTS 是否生效应检查实际设备和 `/proc/device-tree/`，不要依赖 `uname -a` 时间。

## 19. 最终交付目标（2026-09-29 核对）

```text
[x] 可重复构建                  make CROSS_COMPILE=... （VM 内）
[x] 可重复烧写                  scripts/flash_image.ps1 / flash_partition.sh
[x] V4L2 采集稳定               1280x720 NV12，30.00 fps
[x] MPP(Rockit) H.264 编码稳定  VLC/ffplay/ffmpeg 均可播
[x] RTSP 标准输出               rtsp://172.32.0.93:8554/live/0，多客户端 fan-out
[x] 多线程 RingBuffer           --threads + frame_ring，8 小时长稳零丢帧
[x] 传感器和 OSD                真实 MPU6500 → 姿态 → OSD 叠加，已进生产配置
[x] 冷启动可靠出流              上电→出流约 19 秒（2026-09-29 修的竞态；旧记录「11 秒」已作废）
[x] 异常恢复                    fail-soft（缺 IMU 不掉流）已就位；
                                网络断开/恢复专项测试已跑（网口捅掉 20s，见 status.md §2.9）
[x] 长时间运行测试              8 小时 864,001 帧；30 分钟带 IMU 长稳
[x] 完整 README                 README 已全量对齐现状（2026-09-29）
[x] 架构图                      `docs/architecture.html`（自包含，按代码实际结构）
[x] 测试报告                    docs/status.md 已补 OSD/IMU/根因章节
[ ] 演示视频                    未产出
```

**结论：功能全部闭环；剩下的是文档与素材。** 最新清单见 `docs/agent-handoff.md`。
