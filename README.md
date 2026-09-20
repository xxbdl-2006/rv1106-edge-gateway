# RV1106 Embedded Linux Edge Gateway

基于 Luckfox Pico Pro/Max（Rockchip RV1106G）的嵌入式 Linux 音视频与多传感器边缘网关。

项目主线是不依赖官方 `rkipc` 黑盒程序，自行实现：

```text
SC3336 -> MIPI CSI -> V4L2 -> NV12 -> MPP H.264 -> 文件/后续 RTSP
```

后续继续接入 MPU6050、RingBuffer、多线程流水线和 OSD。

## 当前状态

已完成并验证：

- SD 卡固件启动、ADB 与 RNDIS 网络。
- SC3336、MIPI D-PHY、ISP/CIF 和 V4L2 节点初始化。
- `/dev/video11` 上 1280x720 NV12 连续采集。
- `rkaiq_3A_server` 启动和稳定帧采集。
- V4L2 采集 300 帧，平均约 30 FPS。
- NV12 数据送入 MPP，硬件编码 H.264。
- 生成的 H.264 文件可以由 VLC 正常播放。

当前程序仍属于第一阶段实时链路验证：

```text
单进程
单线程
采集与编码同步执行
H.264 输出到文件
尚未实现 RTSP、RingBuffer 和 MPU6050
```

## 硬件与软件环境

| 项目 | 配置 |
| --- | --- |
| 主控 | Rockchip RV1106G，Cortex-A7 + RISC-V + 0.5T NPU |
| 开发板 | Luckfox Pico Pro / Max |
| 摄像头 | SC3336，MIPI CSI，2304x1296 |
| 传感器 | MPU6050，规划接入 `/dev/i2c-4` |
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
                                                    | MPP H.264 Encoder|
                                                    +------------------+
                                                              |
                                                              v
                                                    +------------------+
                                                    | H.264 file       |
                                                    +------------------+
```

目标架构：

```text
Video Capture Thread -> Frame RingBuffer -> Encoder Thread
                                                  |
                                                  v
                                           Packet RingBuffer
                                                  |
                                                  v
                                             RTSP Thread

MPU6050 / Mock Sensor -> Sensor RingBuffer -> OSD / Alarm
```

## 仓库结构

```text
.
├── Makefile
├── README.md
├── docs/
│   ├── adb-flash.md
│   ├── handoff.md
│   └── roadmap.md
├── scripts/
│   ├── diagnose_v4l2.sh
│   ├── flash_image.ps1
│   ├── flash_partition.sh
│   └── start_rkaiq.sh
├── src/
│   ├── mpp_encoder.c
│   ├── mpp_encoder.h
│   ├── v4l2_capture.c
│   └── v4l2_mpp_encode.c
└── tools/
    └── nv12_to_png.py
```

## MPP SDK 路径

默认路径在 `Makefile` 中配置为：

```makefile
MPP_ROOT ?= /home/aaazhx/luckfox-pico/media/mpp/release_mpp_rv1106_arm-rockchip830-linux-uclibcgnueabihf
```

包含头文件：

```text
$(MPP_ROOT)/include
$(MPP_ROOT)/include/rockchip
```

链接库：

```text
$(MPP_ROOT)/lib/librockchip_mpp.so
```

如果 SDK 目录不同，只需要修改 `MPP_ROOT`。

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

`v4l2_mpp_encode` 用于 V4L2 采集加 MPP H.264 编码。

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

编码 300 帧：

```powershell
adb shell "/userdata/v4l2_mpp_encode -d /dev/video11 -w 1280 -H 720 -n 300 --warmup 30 -o /userdata/live.h264"
adb pull /userdata/live.h264 F:\luckfox_share\live.h264
```

当前程序使用文件输出。后续增加 RTSP 时，编码线程会改用有界 Packet Queue，而不是直接写文件。

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

## MPP 版本说明

当前板端 MPP 版本为：

```text
2024-02-20
16e796a4
```

该版本使用：

```text
mpp_init_ext
vcodec_attr
encode_put_frame
encode_get_packet
```

当前 `mpp_encoder.c` 已经针对此版本的 packet 描述符生命周期做兼容处理。不要直接改用其他 MPP 版本的初始化代码，除非同时确认头文件、动态库和内核模块一致。

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

下一阶段优先实现：

```text
MPP Packet -> Packet Queue -> RTSP over TCP
```

验收目标：

```text
VLC 可以打开 rtsp://172.32.0.93:8554/live/0
1280x720 30 FPS 连续播放
单客户端断开和重连不影响编码
延迟小于 1 秒
```

之后再进入：

```text
Frame RingBuffer
多线程采集和编码
Mock Sensor
OSD 与告警
MPU6050
开机服务
长时间稳定性测试
```

## 安全与资源注意事项

- 不要同时运行 `rkipc` 和自定义 MPP 编码程序。
- 不要把损坏或尚未验证的镜像写入正在挂载的分区。
- 不要无上限写 `/userdata`。
- 不要在主循环中执行无限增长的动态分配。
- 旧版 MPP 内核对错误的 context 初始化非常敏感，出错后建议重启开发板清除状态。
