# RV1106 Edge Gateway Handoff

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

目标传感器链路：

```text
MPU6050 -> /dev/i2c-4 -> Sensor RingBuffer -> OSD/Alarm
```

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
| MPP 编码 | 已完成 | 硬件 H.264 编码到文件 |
| 文件播放 | 已完成 | 生成的 H.264 可由 VLC 正常播放 |
| RTSP | 未完成 | 下一阶段主要任务 |
| RingBuffer/多线程 | 未完成 | RTSP 后实施 |
| MPU6050 | 未完成 | 当前硬件尚未接入 |

当前代码是单进程、单线程、文件输出版本。它是有意保留的稳定基线。

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
| 传感器 | MPU6050，待接入 |
| 传感器总线 | `/dev/i2c-4` |
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

```text
Video Capture Thread (当前与主循环合并)
        |
        v
V4L2 MPLANE mmap
        |
        v
NV12 1280x720
        |
        v
MPP H.264 Encoder
        |
        v
H.264 Annex-B File
```

代码职责：

```text
v4l2_capture.c
  独立 V4L2 采集诊断程序

v4l2_mpp_encode.c
  V4L2 到 MPP 的主循环
  通过 V4L2_CAPTURE_NO_MAIN 复用 v4l2_capture.c

mpp_encoder.c/.h
  MPP 初始化、编码配置、packet 获取和文件写入
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

MPP：

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

## 13. 当前限制

- 采集、编码在主线程中同步执行。
- H.264 只能写文件。
- 没有 Packet Queue。
- 没有 RTSP。
- 没有客户端连接和重连处理。
- 没有 Frame RingBuffer。
- 没有 MPU6050。
- 3A 依赖人工启动。
- 没有开机服务、看门狗和自动重启。
- 没有长时间网络压力测试。

## 14. 下一阶段任务

优先级顺序：

1. 保留当前文件编码版本为 Baseline。
2. 将 `FILE *` 输出改为 Packet Sink。
3. 实现 File Sink 和 Packet Queue。
4. 实现 RTSP over TCP 单客户端。
5. 实现 H.264 RTP 打包。
6. 增加断线重连和多客户端。
7. 增加 Frame RingBuffer 和编码线程。
8. 增加 Mock Sensor 和 OSD 数据面。
9. 接入真实 MPU6050。
10. 做开机服务和 8 小时稳定性测试。

详细路线见：

```text
docs/roadmap.md
```

## 15. RTSP 验收标准

```text
URL: rtsp://172.32.0.93:8554/live/0
Codec: H.264
Resolution: 1280x720
Frame Rate: 30 FPS
Latency: less than 1 second
Client disconnect must not block encoder
Client reconnect must recover
30-minute playback without crash
```

## 16. 传感器验收标准

```text
Device: /dev/i2c-4
Address: 0x68 or 0x69
WHO_AM_I: 0x68
Accelerometer norm: approximately 1g at rest
Gyroscope bias: near zero after calibration
Sample rate: 100 Hz
Run time: 10 minutes without I2C errors
```

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

## 19. 最终交付目标

```text
可重复构建
可重复烧写
V4L2 采集稳定
MPP H.264 编码稳定
RTSP 标准输出
多线程 RingBuffer
传感器和 OSD
异常恢复
长时间运行测试
完整 README、架构图、测试报告和演示视频
```
