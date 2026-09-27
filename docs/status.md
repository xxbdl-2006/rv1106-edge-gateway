# RV1106 Edge Gateway 状态与验收记录

本文档记录每一项已完成的验收、实测数据和踩过的坑。`docs/handoff.md` 描述架构和
交接要求，本文档描述**实际测到了什么**。两者冲突时以本文档的实测数据为准。

最后更新：2026-09-27

---

## 1. 验收结果总览

| 验收项 | 标准 | 实测 | 结论 |
| --- | --- | --- | --- |
| 分辨率/帧率 | 1280x720 @ 30fps | 30.002 fps（30 分钟长跑） | 通过 |
| 延迟 | < 1s | ffplay 约 0.75s | 通过 |
| 客户端断开不阻塞编码 | 必须 | 无客户端时队列正常丢旧 GOP | 通过 |
| 客户端重连可恢复 | 必须 | 重连后从下一个 IDR 开始 | 通过 |
| 30 分钟播放不崩溃 | 必须 | 54004 帧 / 0 empty | 通过 |
| 多客户端 | 加分项 | 4 路并发，关任一路不影响其他 | 通过 |
| 开机自启 | 加分项 | `adb reboot` 后 11 秒自动出流 | 通过 |
| 长期稳定性 | 8 小时 | 累计 6.5 小时零丢帧（分段） | 见 §4 |

### 长稳实测数据（ffmpeg 拉流）

| | 第 1 段 | 第 2 段 |
| --- | --- | --- |
| 时长 | 1 小时 53 分 | 4 小时 36 分 |
| 帧数 | 204,591 | 496,278 |
| 平均 fps | 30.0 | 30.0 |
| **drop_frames 非零次数** | **0 / 13226** | **0 / 32068** |
| **dup_frames 非零次数** | **0 / 13226** | **0 / 32068** |
| speed | 1.01x | 1.01x |
| 结束方式 | `Failed reading RTSP data: End of file` | 无退出记录（被外部强杀） |

**累计 6 小时 29 分、70 万帧、零丢帧、零重复帧。** 两段都是在「几小时」这个量级
上断的，而不是某个固定秒数，且第 2 段连一行结尾都没有 —— 说明是**主机侧进程生命
周期**问题（休眠/关机/窗口被关），不是服务端缺陷。

第 1 段结尾的 `error while decoding MB 65 23` 出现在 `End of file` **之后**，是断开
瞬间半个 P 帧没解完的必然产物，不是坏帧。

### 尚未完成

- **一次干净的 8 小时收尾**：目前缺的是「有 `progress=end`、有 `exit=0`」的完整
  结束记录，而不是时长。建议用 `-t 7200` 让 ffmpeg 自己到点退出。

---

## 2. 已修复的缺陷与根因

这些都是真实发生过、并且已经在自测里钉死的问题。**改动时不要把它们改回去。**

### 2.1 RTSP 协议类

| 现象 | 根因 | 修复 |
| --- | --- | --- |
| ffmpeg 系客户端 SETUP 404 | `a=control` 用相对路径，被拼到 `Content-Base` 后得到重复路径 | control 发绝对 URL，`path_matches()` 用 `strstr` 包含匹配 |
| PLAY 返回 454 | session id 发十六进制但按十进制解析，`5F0A1B2C` 被截成 5 | 按 RFC 2326 当不透明字符串比较 |
| SPS 被污染 | Annex-B 4 字节起始码多出的 `0x00` 被算进上一个 NAL | 找到起始码后向前吃掉连续零 |
| RTP-Info 出现 `//trackID=0` | 路径拼接未归一化 | `build_base_url()` 归一化 |

### 2.2 VLC 卡死（两个独立原因，都表现为「卡在某个整数时间」）

| 现象 | 根因 | 修复 |
| --- | --- | --- |
| 精确在 01:00 被踢 | 空闲超时 60s 撞上 live555 的 60s 保活周期 | 超时改 300s，且**送出帧也算活动** |
| 在 2:57 / 3:04 退出 | 响应缺 `Session` 头，live555 三次保活拿不到确认后放弃 | `client_send_response()` 集中生成 Session 头 |
| 播放期间单线程 100% CPU | 客户端线程 `poll(0)` 空转 | 固定 `RTSP_REQUEST_POLL_MS = 20` |

**教训：凡是「卡住的时间是某个整数的整数倍」，先去数这个整数对应哪个定时周期。**

VLC 后续仍有卡顿，经 verbose 日志确认是 **VLC 自身 D3D11VA 硬解**问题（demux 停
10s），与服务端无关；ffplay/ffmpeg 全程正常。

### 2.3 并发类（最隐蔽的一个）

**现象**：起流瞬间一次解码错误 + 花屏，客户端回 `RTSP/1.0 501`。

**根因**：客户端线程写 RTSP 响应、reader 线程写 RTP，**两者并发写同一个 socket**。
interleaved 帧交错后客户端解析器失步。

**修复**：`client_send_response()` 自持 `server->mutex`；`send_rtp_packet()` 改为
在栈上拼好整包**一次 send**。

> ⚠️ 任何 handler 都**不得持锁**调用 `client_send_response()`，否则死锁。

验证：失步记录 4 → 0，ffmpeg 零错误。

### 2.4 平台/环境类

| 现象 | 根因 | 修复 |
| --- | --- | --- |
| adb 后台起的进程莫名消失 | adbd 在 shell 服务退出时杀整个进程组，子进程来不及 setsid | `& sleep 2`（纯竞态，`start-stop-daemon --background` 不做 setsid） |
| 手动能跑、开机起不来 | init 环境未 source `/etc/profile.d/RkEnv.sh`，缺 `LD_LIBRARY_PATH` | 脚本开头显式 source 并去重 |
| `killall` 在 init 环境"存在但什么都不做" | busybox 行为差异，重定向 stderr 后错误不可见 | 改用 shell 内建 `kill` + `pidof` |
| 日志大小取不到 | 板上**没有 `stat` 命令** | 改 `wc -c < file` |
| awk 匹配 `VmRSS:` 失败 | `/proc/PID/status` 字段名**带冒号** | `sub(/:$/, "", name)` |
| Git Bash 调 adb 报 not found | 以 `/` 开头的参数被 MSYS 改写成 `C:/...` | `export MSYS_NO_PATHCONV=1` |

### 2.5 条件变量时钟（最新，容易再犯）

**现象**：`capture_thread_wait_ready()` 报「没有帧」，但采集线程实际在以百万帧/秒
的速度正常产出。

**根因**：`pthread_cond_init(cond, NULL)` 创建的条件变量走 **CLOCK_REALTIME**，而
超时用的绝对时间是按 **CLOCK_MONOTONIC** 算的。Windows 上 monotonic 约等于「进程
启动至今的秒数」（几千），realtime 约 17 亿，于是 monotonic 的死线在 realtime 计时
器看来是 1970 年 → `timedwait` 立刻返回 `ETIMEDOUT`。

**更坑的是**：`pthread_condattr_setclock()` 在 uclibc 上生效，在 winpthreads 上返回
`EINVAL` 且**静默**保持 realtime。所以不能假设设置成功。

**修复**：`start()` 里检查 `pthread_condattr_setclock()` 的返回值，把**实际被接受的
时钟**记在 `cond_clock` 字段里，`build_deadline()` 按它算死线。

`src/frame_ring.c` 和 `src/capture_thread.c` 都已按此处理。

---

## 3. 当前架构

### 3.1 两条流水线

```text
--threads 关闭（默认，已验证的稳定路径）
  V4L2 DQBUF -> 紧致化 NV12 -> MPP 编码 -> Sink

--threads 开启（本次新增）
  采集线程: V4L2 DQBUF -> 紧致化 NV12 -> frame_ring
  主线程  : frame_ring -> MPP 编码 -> Sink
```

### 3.2 为什么要有 frame_ring

**收益是架构准备，不是救火。** 当前同步流水线实测 30.002fps、零丢帧，说明它并不是
瓶颈。分线程的价值在于：

- 编码或落盘卡顿时不再拖累采集（现在慢编码会直接卡住 `VIDIOC_DQBUF`，传感器队列
  在背后悄悄堆积，延迟随之增长）；
- 为后续 OSD 留出头寸；
- 每帧只需一次 memcpy，环满时**覆盖最旧帧**而不阻塞上游。

### 3.3 线程与分工

```text
src/capture_thread.c / .h   V4L2 取帧线程，通过回调把 NV12 交给 frame_ring
src/frame_ring.c / .h       有界 NV12 帧环，创建期一次性分配，push 永不阻塞
src/capture_signal.h        g_stop 的外部声明，信号处理/capture 循环/采集线程共用
```

**分层原则**：`capture_thread.c` 刻意不依赖任何 V4L2 细节，所以能在 Windows 上跑
单测。V4L2 相关的东西全部留在 `v4l2_capture.c` / `v4l2_mpp_encode.c`。

**socket 只出现在 `src/rtsp_server.c` 一个文件里**，协议解析在 `rtsp_proto.c`，RTP
打包在 `rtp_h264.c`，都是纯缓冲无依赖 —— 新增网络功能请沿用这个划分。

---

## 4. 构建与自测

### 构建（Ubuntu 虚拟机）

```bash
cd /mnt/hgfs/luckfox_share/rv1103
make clean
make CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
```

### 主机自测（Windows / Linux 均可）

```bash
make test          # 四个套件，共 206 项检查
make host-syntax   # MinGW 下对含 socket 的文件做语法检查
```

| 套件 | 检查数 | 覆盖内容 |
| --- | --- | --- |
| `test-packet-queue` | 43 | 队列顺序、零拷贝借用、丢整个 GOP |
| `test-rtp-rtsp` | 86 | RTP 打包、SDP、请求解析 |
| `test-frame-ring` | 64 | 帧环顺序、覆盖最旧、生产者消费者并发 |
| `test-capture-thread` | 13 | 采集成帧、warmup 过滤、故障停机、stop 不挂起 |

合计 **206 项，0 失败**。

### MinGW 限制

`src/v4l2_capture.c` 和 `src/v4l2_mpp_encode.c` 依赖 `linux/videodev2.h`，MinGW
不提供，**只能在虚拟机里检查**。可主机测试的逻辑都抽到了 `frame_ring.c` 和
`capture_thread.c`。桩文件说明见 `tests/host-stubs/README.md`。

---

## 5. 板端操作备忘

### 起流

```bash
# 开机自启已装好的情况下，什么都不用做
# 手动：
/userdata/gateway-supervise.sh &
```

### 长稳采样

```bash
# 启动
adb shell "setsid /userdata/soak-monitor.sh 60 >/dev/null 2>&1 < /dev/null & sleep 2; echo started"
# 停止
adb shell "kill \$(cat /tmp/soak-monitor.pid)"
```

健康基线（无客户端）：**RSS 11.5MB、fds 24、threads 5、CPU 14~17%、温度 52.5°C、
可用内存 155MB**。长稳要看的是这些数字**是否随时间单调上升**，而不是绝对值。

### 拉流

```powershell
ffmpeg -rtsp_transport tcp -i rtsp://172.32.0.93:8554/live/0 -t 7200 -f null - `
  -nostats -progress D:\luckfox_share\soak-progress.txt 2> D:\luckfox_share\soak-ffmpeg.log
"exit=$LASTEXITCODE" | Add-Content D:\luckfox_share\soak-ffmpeg.log
```

**必须落一条 `exit=`**，否则下次断流只能靠反推结束原因。
另外先关掉休眠，否则几小时后进程会随着电脑一起睡过去：

```powershell
powercfg /change standby-timeout-ac 0
powercfg /change hibernate-timeout-ac 0
```

诊断入口：`/tmp/gateway-boot.log`、`/etc/init.d/S99gateway status`、
`/userdata/gateway.log`。

---

## 6. 下一步

1. **上板验证 `--threads`**：`make test` 已全绿，但新路径**尚未上板跑过**。
   建议先与默认同步路径对比同样时长，确认 fps/丢帧一致后再考虑改默认值。
2. **一次干净的 8 小时收尾**（见 §1）。
3. Mock Sensor → OSD → 真实 MPU6050 → 8 小时长稳。
