# Development Roadmap

> **完成度核对（2026-09-29）**：本文的每个阶段**除「工程化与压力测试」外均已实现并上板验证**。
> 各阶段标题已加 ✅ / ⚠️ 标记；正文保留原计划（含当时的参数设想），作为设计意图的记录。
> 现状清单与每项证据见 `docs/agent-handoff.md`，实测数据见 `docs/status.md`。
>
> | 阶段 | 状态 |
> | --- | --- |
> | 已完成（系统/链路基线） | ✅ |
> | RTSP 输出 | ✅ 多客户端 fan-out，8 小时长稳 |
> | 多线程与 RingBuffer | ✅ `--threads` + frame_ring |
> | 传感器阶段 | ✅ Mock + 真实 MPU6500（走 bit-bang，非 `/dev/i2c-4`） |
> | OSD 与告警阶段 | ⚠️ OSD ✅（未用 RGA/MPP OSD，走 1bpp 画布自合成）；**告警通道未实现** |
> | 系统服务阶段 | ✅ 上电到出流约 19 秒（旧记录「11 秒」已作废，见 status.md §2.8） |
> | 工程化与压力测试 | ⚠️ 8h/多客户端/泄漏/CPU 温度 ✅；**网络断线恢复、端到端延迟未测** |
> | 最终交付物 | ⚠️ 代码与脚本 ✅；**架构图、演示视频未产出** |

## 已完成

- RV1106 Buildroot 系统启动。
- ADB 和 RNDIS 开发链路。
- SPI0 和 `/dev/spidev0.0` 设备树验证。
- SC3336 MIPI CSI 链路。
- V4L2 NV12 1280x720 连续采集。
- `rkaiq_3A_server` 稳定曝光和白平衡。
- V4L2 到 MPP H.264 文件编码。
- 300 帧连续采集和 VLC 文件播放验证。

## ✅ 阶段：RTSP 输出（已完成）

目标：

```text
MPP H.264 Packet -> Packet Queue -> RTSP over TCP
```

任务：

1. 将编码器的 `FILE *` 输出抽象为 Packet Sink。
2. 实现 File Sink，保留当前功能。
3. 实现有界 Packet Queue。
4. 实现 RTSP Server。
5. 实现 H.264 RTP 打包。
6. 支持 SPS、PPS、IDR、Single NAL 和 FU-A。
7. 支持客户端断开和重新连接。

验收：

```text
rtsp://172.32.0.93:8554/live/0
1280x720 30 FPS
延迟小于 1 秒
连续播放 30 分钟
客户端断开不阻塞编码
```

## ✅ 阶段：多线程与 RingBuffer（已完成）

线程模型：

```text
Video Capture Thread
    -> Frame RingBuffer
    -> Encoder Thread
    -> Packet RingBuffer
    -> RTSP Thread
```

任务：

1. 建立固定容量的 Frame Pool。
2. 建立单生产者单消费者 RingBuffer。
3. 明确背压和丢帧策略。
4. 采集线程复制 NV12 后立即归还 V4L2 buffer。
5. 编码线程独立于网络线程。
6. 统计各线程 FPS、延迟、丢帧和队列深度。

建议参数：

```text
V4L2 Buffer：4
Frame Pool：6
Frame Queue：4
Packet Queue：32 packets 或 4 MB
```

## ✅ 阶段：传感器（已完成）

> **实现与原计划的偏差**：原计划走 `/dev/i2c-4`，但运行时 device-tree overlay
> 在这块板上是坏的（写合法 dtbo 和写 GARBAGE 都 rc=0），该节点不存在 →
> 改为用户态 **bit-bang GPIO（pin 14 SDA / pin 24 SCL）**。
> 另外 `WHO_AM_I` 实测是 **0x70**（料为 MPU6500，寄存器布局同 MPU6050），不是 0x68。
> 采样率也不是 100 Hz —— 见下方「需要更正的两处」。

没有 MPU6050 时，先实现 Mock Sensor：

```text
sensor_source
    -> mock_sensor
    -> mpu6050_source
```

统一数据：

```c
struct sensor_sample {
    uint64_t timestamp_ns;
    float accel_x;
    float accel_y;
    float accel_z;
    float gyro_x;
    float gyro_y;
    float gyro_z;
    float pitch;
    float roll;
};
```

后续真实 MPU6050 任务：

1. `/dev/i2c-4` 设备扫描。
2. 检查地址 `0x68` 或 `0x69`。
3. 读取 `WHO_AM_I(0x75)`。
4. 退出睡眠模式。
5. 配置采样率、DLPF、量程。
6. 连续读取 14 字节数据。
7. 进行零偏校准。
8. 计算 pitch 和 roll。

验收：

```text
WHO_AM_I = 0x68
静止时加速度模长约 1g
校准后陀螺仪零偏接近 0
100 Hz 连续运行 10 分钟无 I2C 错误
```

**需要更正的两处（实测结果）**：

| 原计划 | 实测 | 说明 |
| --- | --- | --- |
| `WHO_AM_I = 0x68` | **0x70** | 料是 MPU6500，寄存器布局同 MPU6050 |
| **采样率 100 Hz** | **芯片 100 Hz，总线读取 10 Hz（100 ms）** | bit-bang 一次 14 字节突发 ≈ **28 ms**，10 Hz 是总线极限而非传感器极限。按 100 Hz 读会把帧率从 30 打到 21.4 |

其余验收项通过：静止 `\|a\| = 0.990~0.995 g`；30 分钟 15057 次采样 1 次总线错误（≈7e-5）。
另注意**姿态是相对「标定时的安装角」**，不是相对世界水平（单点标定分不清倾斜和零偏）。

## ✅ 阶段：OSD（已完成）；⚠️ 告警未实现

任务：

1. 将传感器数据送入独立 RingBuffer。
2. 生成文字或图标位图。
3. 使用 RGA 或 MPP OSD 叠加视频。
4. 增加姿态和加速度阈值告警。
5. 避免 OSD 阻塞视频编码。

**实现与原计划的偏差**：**没有用 RGA / MPP OSD**，而是用 1bpp 画布 + 内嵌 5x7 点阵字体
**在私有 NV12 副本上自合成**（避开逐帧分配和 libc 浮点格式化，也避开了拿不到
freetype/fontconfig 的精简 rootfs）。结果见 `docs/status.md` §1.3：300/300 帧合成成功，
**帧率零开销**（30.003 vs 30.000 fps）。

**第 4 项（告警）未实现**：只有 TILT / MAG 两个阈值**标记**烧进叠加层，没有独立告警通道。

## ✅ 阶段：系统服务（已完成）

任务：

1. 禁用或停止自动启动的 `rkipc`。
2. 开机启动 `rkaiq_3A_server`。
3. 开机启动网关程序。
4. 实现 SIGINT 和 SIGTERM 优雅退出。
5. 实现异常重启。
6. 增加日志和运行统计。

**实测**：**冷启动上电到出流约 19 秒**。接管判据是**等 `/dev/video11` 节点出现**
（不是等 rkipc 进程 —— rkipc 只在开机启动一次，restart 时永远不会出现，旧写法白等 20 秒）。
三件套：`S99gateway` + `/userdata/gateway-supervise.sh` + `/userdata/gateway.env`。

## ⚠️ 阶段：工程化与压力测试（部分完成）

| 项 | 状态 |
| --- | --- |
| 8 小时连续 RTSP 播放 | ✅ 864,001 帧 / 30.00fps / 零丢帧零泄漏 |
| 多客户端连接 | ✅ 4 路并发，关任一路不影响其他 |
| 内存和文件描述符泄漏检查 | ✅ fds 24→24、threads 6→6，末 2 小时 RSS 增长 0 KB |
| CPU 和温度记录 | ✅ 基线 14~17% / 52.5°C；带 IMU 后 32% / 55.9°C |
| V4L2、MPP、RTSP 错误恢复 | ⚠️ fail-soft（缺 IMU 不掉流）已就位；**网络断开/恢复专项测试未跑** |
| **网络断开和恢复** | ❌ **未测** |
| **编码延迟和端到端延迟** | ❌ **未正式测**（只有 ffplay ~0.75s 粗测） |

## 最终交付物（部分完成）

- ✅ 可重复的构建脚本。
- ✅ 设备树补丁（`dts/`）。
- ✅ V4L2 采集程序。
- ✅ MPP(Rockit) 编码模块。
- ✅ RingBuffer 和多线程流水线。
- ✅ RTSP Server。
- ✅ 传感器模块和 OSD。
- ✅ 测试报告（`docs/status.md`）。
- ❌ **架构图**。
- ❌ **演示视频**。
