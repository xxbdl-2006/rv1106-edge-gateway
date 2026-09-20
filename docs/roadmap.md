# Development Roadmap

## 已完成

- RV1106 Buildroot 系统启动。
- ADB 和 RNDIS 开发链路。
- SPI0 和 `/dev/spidev0.0` 设备树验证。
- SC3336 MIPI CSI 链路。
- V4L2 NV12 1280x720 连续采集。
- `rkaiq_3A_server` 稳定曝光和白平衡。
- V4L2 到 MPP H.264 文件编码。
- 300 帧连续采集和 VLC 文件播放验证。

## 当前阶段：RTSP 输出

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

## 下一阶段：多线程与 RingBuffer

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

## 传感器阶段

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

## OSD 与告警阶段

任务：

1. 将传感器数据送入独立 RingBuffer。
2. 生成文字或图标位图。
3. 使用 RGA 或 MPP OSD 叠加视频。
4. 增加姿态和加速度阈值告警。
5. 避免 OSD 阻塞视频编码。

## 系统服务阶段

任务：

1. 禁用或停止自动启动的 `rkipc`。
2. 开机启动 `rkaiq_3A_server`。
3. 开机启动网关程序。
4. 实现 SIGINT 和 SIGTERM 优雅退出。
5. 实现异常重启。
6. 增加日志和运行统计。

## 工程化与压力测试

- 8 小时连续 RTSP 播放。
- 网络断开和恢复。
- 多客户端连接。
- 内存和文件描述符泄漏检查。
- CPU 和温度记录。
- 编码延迟和端到端延迟。
- V4L2、MPP、RTSP 错误恢复。

## 最终交付物

- 可重复的构建脚本。
- 设备树补丁。
- V4L2 采集程序。
- MPP 编码模块。
- RingBuffer 和多线程流水线。
- RTSP Server。
- 传感器模块和 OSD。
- 测试报告。
- 架构图和演示视频。
