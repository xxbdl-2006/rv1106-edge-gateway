# RV1106 Edge Gateway 状态与验收记录

本文档记录每一项已完成的验收、实测数据和踩过的坑。`docs/handoff.md` 描述架构和
交接要求，本文档描述**实际测到了什么**。两者冲突时以本文档的实测数据为准。

最后更新：2026-09-28

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
| 长期稳定性 | 8 小时 | **8 小时干净收尾（线程化，864,001 帧 / 0 丢帧）** | 见 §1 长稳数据 |

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

无。8 小时干净收尾已于 2026-09-28 达成（见下）。

### 2 小时长稳：干净收尾已达成（2026-09-27，同步流水线）

第一次拿到带退出码的完整记录，补上了上一节缺的东西。

客户端侧：

```text
总时长       02:00:00.033
帧数         216,001
平均帧率     30 fps（speed=1x）
码率         838.7 kbits/s
输出         719.9 MB
drop_frames  0        全程
dup_frames   0        全程
progress=end
exit=0
```

服务端侧：

```text
RTSP: session 00EF0F77 ended because the client closed it
RTSP: session 00EF0F77 closed after 216004 frames
失步计数（grep 'RTSP: RTSP/1.0'）   0
gateway.log 大小                     14,845 字节
```

216,004（服务端）× 216,001（文件）差 3 帧，是解码器 fifo 在 EOF 后冲刷的正常产物。

资源趋势（120 个采样点，每 60 秒）：

| 指标 | 起点 | 中点 | 终点 | 峰值 | 结论 |
| --- | --- | --- | --- | --- | --- |
| RSS | 10,472 KB | 11,952 KB | 11,980 KB | 11,980 KB | 后 60 分钟完全平坦 |
| fds | 25 | 25 | 24 | 25 | 无泄漏 |
| threads | 6 | 6 | 5 | 6 | 无泄漏 |
| CPU | — | — | — | 16% | 均值 15.6% |
| 温度 | 50.5°C | 53.5°C | 53.5°C | 55.3°C | 稳定 |
| 可用内存 | 155,912 KB | — | 154,472 KB | 最低 154,384 KB | 稳定 |

**RSS 的形状值得记住**：前 60 分钟爬升约 1.5MB，之后 60 分钟只出现 3 个不同取值，
最后 30 分钟钉死在 11980 KB。这是分配器热身而不是泄漏 ——
判断泄漏要看**尾部是否仍在爬**，不要只看首尾差值。

### 8 小时长稳：干净收尾 + 零泄漏（2026-09-28，线程化流水线）

**这是本项目第一次跑满 8 小时，也是第一次在线程化路径上做长稳。**

客户端侧：

```text
总时长       08:00:00.033
帧数         864,001          （理论值 30 × 28800 = 864,000）
平均帧率     30.00 fps（speed=1x，elapsed=8:00:00.40）
码率         602.0 kbits/s
输出         2,167,276,930 字节（2.02 GB）
drop_frames  0        全程
dup_frames   0        全程
progress=end
exit=0
```

服务端侧：

```text
RTSP: session 00EF0F36 ended because the connection hung up
RTSP: session 00EF0F36 closed after 864004 frames
失步计数（grep 'RTSP: RTSP/1.0'）   0
客户端错误响应次数                   0
错误日志（error|fail|panic|desync|mismatch）  0 条
gateway.log 大小                     51,789 字节（约 6.5 KB/小时）
```

864,004（服务端）× 864,001（文件）差 3 帧 —— 与 2 小时测试**同样的 3 帧**，
再次印证这是解码器 fifo 在 EOF 后冲刷的固定产物，不是丢帧。

资源趋势（**480 个采样点**，每 60 秒，覆盖整整 8 小时）：

| 指标 | 起点 | 1/4 | 1/2 | 3/4 | 终点 | 结论 |
| --- | --- | --- | --- | --- | --- | --- |
| RSS | 16,036 KB | 16,272 | 16,280 | 16,284 | 16,284 | 见下 |
| fds | 24 | — | — | — | **24** | 完全无变化 |
| threads | 6 | — | — | — | **6** | 完全无变化 |
| CPU | — | — | — | — | — | 均值 22.0%，峰值 22.0% |
| 温度 | 51.1°C | — | — | — | — | 51.1 ~ 55.3°C |
| 可用内存 | 150,652 KB | — | — | — | 150,304 KB | 仅降 348 KB |
| alive 标志异常次数 | — | — | — | — | — | **0 / 480** |

**RSS 稳定性的量化证据**（这是判断泄漏最有力的方式）：

| 区间 | 不同取值个数 |
| --- | --- |
| 前 1 小时 | **9 个**（分配器热身） |
| 最后 2 小时 | **1 个**（完全钉死） |
| 最后 1 小时 | **1 个** |

最后 2 小时 RSS 增长 **0 KB**，最后 4 小时仅增长 **4 KB**。

**结论：零泄漏。** 曲线形状与 2 小时测试完全同构（前期热身 → 中期收敛 → 后期钉死），
只是时间轴拉长了 4 倍，说明这是稳定的分配器行为而非偶然。


复现命令：

```bash
bash scripts/start-2h-soak.sh            # 同步路径
bash scripts/start-2h-soak.sh --threads  # 线程化路径（需先编好）
bash scripts/start-2h-soak.sh --hours 8  # 推到 8 小时
```

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

1. **✅ 上板验证 `--threads` 已完成**（2026-09-27/28）：8 小时长稳零丢帧、零泄漏、
   干净收尾（见 §1）。线程化路径与同步路径的对比：
   | | 同步（2h） | 线程化（8h） |
   | --- | --- | --- |
   | fps | 30 | 30.00 |
   | drop/dup | 0 | 0 |
   | 失步 | 0 | 0 |
   | RSS 终值 | 11,980 KB | 16,284 KB |
   | CPU 均值 | 15.6% | 22.0% |
   | 线程数 | 5 | **6** |
   
   线程化路径 CPU 高约 6 个百分点、RSS 多约 4MB —— 这是**采集线程 + 3 槽 frame ring
   （1280×720×1.5 字节/槽 ≈ 4.15MB）的固定开销**，与时长无关（8 小时后仍在 16,284 KB
   不动），属预期成本而非泄漏。**收益是架构性的**：采集不再被编码/落盘拖累，
   为 OSD 与磁盘写入留出了头寸。
2. **✅ MPU6050 C 版已上板读通**（2026-09-28，见 `docs/mpu6050-wiring.md` §6/§7）：
   `0x68: present` + `WHO_AM_I=0x70 (MPU6500)` + **`0 io error(s)`** +
   `|a| raw 1.056 / calibrated 0.996`，与 Python 黄金对照逐项一致。
   过程中修掉了两个只有真设备才暴露的 bug：缺 `-Isrc`（主机检查标志与真实构建不一致）
   与 **sysfs GPIO 写序**（`direction=in` 时写 `value` 返回 EPERM → 225 个 io error）。
   下一步：Mock Sensor → OSD。
3. **✅ `S99gateway restart` 白等 20 秒已修复**（见 §7），并顺带修掉了
   `LD_LIBRARY_PATH` 重复追加（`S99gateway` 与 `gateway-supervise.sh` **两处都有**）、
   以及 Windows 检出导致的 CRLF shebang 问题。

---

## 7. S99gateway restart 白等 20 秒：根因与修复（2026-09-28）

### 现象与实测

`/tmp/gateway-boot.log` 里两个场景的对比是决定性的：

```
开机     12:00:05 start requested
         12:00:05 /userdata is ready
         12:00:09 rkipc is streaming on /dev/video11, taking the camera over   ← 4 秒
         12:00:16 gateway is up

restart  12:01:32 start requested
         12:01:32 /userdata is ready
         12:01:55 rkipc never appeared in 20s, continuing anyway               ← 白等 23 秒
         12:01:59 gateway is up
```

### 根因

旧代码用 `wait_for_process rkipc` 等 rkipc **进程**出现（200 × 0.1s = 20 秒上限）。
但 rkipc 只由 `S21appinit` → `/oem/usr/bin/RkLunch.sh`（其 `post_chk()` 的最后一行
`rkipc -a /oem/usr/share/iqfiles &`）在**开机时启动一次**。restart 场景下：

1. rkipc 早已被我们杀掉；
2. `RkLunch.sh` 不会再跑（开机才跑一次）；
3. 于是 rkipc 永远不会出现 → **必然**吃满 20 秒超时。

顺带纠正一个此前记录中的错误假设：rkipc **不是**被改名成 K 开头禁用的，它是由
`S21appinit` 正常拉起的。restart 时它不存在，纯粹是因为已经被杀且启动链不再触发。

### 关键实测：rkipc 与摄像头就绪无关

板端实测（uptime 13h39m、rkipc 完全不存在）下：
- 网关持续推流正常，`Threads: 6`，CPU 21.4%（与长稳基线一致）；
- `/dev/video11` 存在且**唯一占用者就是 `v4l2_mpp_encode` 自己**；
- `rkaiq_3A_server` 独立工作，不依赖 rkipc。

结论：**判据应该是"摄像头节点是否就绪"，而不是"rkipc 进程是否在"**。

### 修法

| 旧 | 新 |
| --- | --- |
| 等 rkipc 进程出现 | 先等 `/dev/video11` 节点出现（150 × 0.2s = 30s） |
| 再等它 streaming | **仅当 rkipc 存在**时才等它 streaming；不在就直接进行 |
| 无条件 `stop_by_name rkipc` | 先查 `capture_holder`，**占用者不是 rkipc 就拒绝接管**并 `return 1` |

新增的三个函数：`capture_holder()`（查谁占着采集节点）、`wait_for_capture_node()`
（等节点）、`camera_takeover_wait()`（组合判据）。第三个保护的意义在于：如果采集节点
被一个**非 rkipc** 的进程占着，那多半是绕过 pid 文件检查的另一个网关实例，
此时杀掉它比失败更糟，所以宁可大声失败。

### 实测结果

**restart 场景**（rkipc 不在，即原 bug 的触发条件）：

| | 修复前 | 修复后 |
| --- | --- | --- |
| `restart` 命令返回 | ~25s | **5s** |
| takeover 全程 | 23s | **5s**（`01:43:36 → 01:43:41`） |
| boot log 关键字 | `rkipc never appeared in 20s` | `no rkipc, nothing is initialising the camera, proceeding` |
| 重启后推流 | — | **241 帧 / 8.03s / 30fps / 无误** |

**开机场景**（rkipc **正在推流**，2026-09-28 上板复测）：

```
14:07:40 start requested
14:07:40 takeover started, LD_LIBRARY_PATH=[/oem/usr/lib:/oem/lib]
14:07:40 /userdata is ready
14:07:40 rkipc is present, waiting for it to open /dev/video11
14:07:40 rkipc is streaming, taking the camera over     ← 读到 rkipc 已在推流，立刻接管
14:07:41 rkipc pids before: [845]
14:07:43 rkipc is gone
14:07:46 3A server running as 3771
14:07:48 gateway is up, supervisor pid 3785             ← 全程 8 秒
```

这条分支是**首次实测**（此前只验证过 rkipc 不存在的路径），确认「rkipc 存在且已在推流」
时不会多等：判据立刻成立并接管。两条分支自此都上板跑通。

复测推流：**301 帧 / 10.03s / 30fps / 无误**，`Threads: 6`，
`/dev/video11` 唯一占用者是网关自己。

### 顺带修掉的三个问题

**（1）`LD_LIBRARY_PATH` 每次重启重复追加。**
`RkEnv.sh` 内容是 `export LD_LIBRARY_PATH=$HOME/usr/lib:$HOME/lib:$LD_LIBRARY_PATH`
（**前置**追加），而 `S99gateway` 每次调用都会 source 它一次（restart 一次 + 它
re-exec 出来的 takeover worker 再一次）。原代码用一个 `GATEWAY_ENV_LOADED` 环境变量
做守卫，但**跨进程无效**：每次重启是新进程，标志丢了而变量被继承下来，于是每重启一次
就多两份。实测已涨到 6 份：

```
LD_LIBRARY_PATH=/oem/usr/lib:/oem/lib:/oem/usr/lib:/oem/lib:/oem/usr/lib:/oem/lib:
```

修法不是跳过 source（这些路径确实要用），而是 source 之后**去重**：保留每项首次出现
的顺序，丢掉重复项。实测连续 restart 三次，值稳定在 `/oem/usr/lib:/oem/lib` 不再增长。

**同一个 bug 在 `gateway-supervise.sh` 里还有一份**（2026-09-28 补修）。
上板复查时发现一个反常现象：**supervisor 的环境是干净的，但它拉起的 encoder 是双份的**。原因是
链路上有三层都会 source `RkEnv.sh`：

| 层 | 继承到的值 | 自己 source 后 |
| --- | --- | --- |
| `adb shell`（登录 profile） | — | 已经是双份 |
| `S99gateway` | 双份 | 去重 → **单份** |
| `gateway-supervise.sh` | 单份 | 又前置一份 → **双份** |
| `v4l2_mpp_encode` | 双份 | — |

所以只修 `S99gateway` 不够：它把值擦干净了，下一层立刻又加上一份。
`gateway-supervise.sh` 里是**一模一样的 `GATEWAY_ENV_LOADED` 守卫**，同样是跨进程无效。
现在两处都用同一个 `dedup_path()`，实测 supervisor 与 encoder 的值**都是干净的
`/oem/usr/lib:/oem/lib`**，逐层归位。

> 顺带一个观察：`adb shell` 自己进出的 `LD_LIBRARY_PATH` **本来就是双份的**（adbd 的登录
> 环境会 source 两遍）。所以以后看到双份不要立刻当成 bug —— 但要确认**它有没有继续往上涨**，
> 那才是问题所在。

### 另一个教训：`adb push` 被掉线打断会留下 0 字节文件

USB 掉线时正在进行的那次 `adb push` 会**写一半就断**，在板端留下一个 **0 字节**的
`/etc/init.d/S99gateway`。它有两层危害：

1. 下次开机 `S99gateway` 是个空文件 → **什么都没启动**（本次实测：板子重启后
   `v4l2_mpp_encode`/`gateway-supervise`/`rkaiq_3A_server` 全无，只剩 rkipc）；
2. 文件**存在于 `ls` 里**，所以排查时容易先怀疑「脚本逻辑错」而不是「文件是空的」。

**根文件系统是 ext4（可写）**，所以 `/etc` 的改动**会跨重启保留** —— 这一点也意味着
一次被截断的推送不会自己恢复。推送后核对字节数是必须的：
`wc -c < /etc/init.d/S99gateway` 应与本地一致。

**（2）Windows 检出导致 CRLF shebang，脚本"not found"。**
`scripts/S99gateway` 在工作区被写成 CRLF，`adb push` 原样送到板端后，内核把 shebang
读成 `#!/bin/sh\r`，去执行一个名为 `/bin/sh` + CR 的解释器，于是报
`/bin/sh: /etc/init.d/S99gateway: not found` —— 报的是"文件找不到"，而文件明明在，
极具误导性。

> ⚠️ 判断行尾**不能用 Git Bash 的 `grep -c $'\r'`，它会漏报**（shell 层就把 CR 吃掉了，
> 回报 0）。必须用 `od -c file | grep -o '\\r' | wc -l`。我一开始正是用 grep 判定
> 「文件是 LF」，结论完全错误。

注意这个坑**本仓库已经埋好了防线但没覆盖到它**：`.gitattributes` 里有 `*.sh text eol=lf`，
而 `S99gateway` **没有 `.sh` 后缀**，所以规则没管到它。已补上显式规则：

```gitattributes
scripts/S99gateway text eol=lf
scripts/gateway-supervise.sh text eol=lf
scripts/soak-monitor.sh text eol=lf
```

另外该文件在 git 里是 `100644`（非可执行位），而在板端必须是 `755`，所以部署后需要
`chmod 755`。推送后建议核对 `head -c 12 | od -c` 确认是 `\n` 而非 `\r\n`。
