# RV1106 Edge Gateway 状态与验收记录

本文档记录每一项已完成的验收、实测数据和踩过的坑。`docs/handoff.md` 描述架构和
交接要求，本文档描述**实际测到了什么**。两者冲突时以本文档的实测数据为准。

最后更新：**2026-09-29**

> 本次（2026-09-29）新增：
> - **§1.3 OSD 上板验收** —— 300/300 帧合成成功，**帧率零开销**。
> - **§1.4 真实 MPU6500 进生产配置** —— 30 分钟 54000 帧 30.000 fps，真拉流抽帧确认数值在动。
> - **§2.6 bit-bang 总线成本**（新根因）—— 一次 14 字节突发 **28 ms** 而非「几毫秒」，
>   以及由此选出的 100 ms 轮询间隔。
> - **§2.7 配置写在多处 = 副本**（新根因）—— 重装会静默抹掉板上改动。
> - **§2.8 冷启动从未成功过**（**最严重**）—— 接管逻辑与厂商启动链的竞态，
>   每次上电都不出流；已修复并复验通过。**同时作废「开机 11 秒出流」这个数字。**
> - §4 测试套件表、§5 板端操作、§6 下一步同步更新。

---

## 1. 验收结果总览

| 验收项 | 标准 | 实测 | 结论 |
| --- | --- | --- | --- |
| 分辨率/帧率 | 1280x720 @ 30fps | 30.002 fps（30 分钟长跑） | 通过 |
| 延迟 | < 1s | ffplay 约 0.75s（**仅粗测，无文档化方法**） | 通过（弱） |
| 客户端断开不阻塞编码 | 必须 | 无客户端时队列正常丢旧 GOP | 通过 |
| 客户端重连可恢复 | 必须 | 重连后从下一个 IDR 开始 | 通过 |
| 30 分钟播放不崩溃 | 必须 | 54004 帧 / 0 empty | 通过 |
| 多客户端 | 加分项 | 4 路并发，关任一路不影响其他 | 通过 |
| 开机自启 | 加分项 | 冷启动实测：**上电到出流约 19 秒**（修复竞态后，见 §2.8） | 通过 |
| **冷启动（上电 → 3A 首次收敛）** | 必须 | 修复前**每次上电都不出流**；修复后 PASS（见 §2.8） | 通过（已修） |
| 长期稳定性 | 8 小时 | **8 小时干净收尾（线程化，864,001 帧 / 0 丢帧）** | 见 §1 长稳数据 |
| **OSD 叠加** | 合成不拖慢编码 | 300/300 帧合成，**30.003 fps vs 无 OSD 30.000 fps** | 通过（零开销） |
| **真实 IMU 数据面** | 进生产配置 | 30 分钟 54000 帧 30.000 fps，**抽帧数值在变** | 见 §1.4 |
| **传感器总线错误率** | 可接受 | 15057 次采样 1 次错误（≈7e-5） | 通过（不加重试） |
| 网络断开/恢复 | — | **未测** | 未覆盖 |

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

**功能项无：8 小时干净收尾已于 2026-09-28 达成**（见下），随后 2026-09-29 又把真实 IMU
与 OSD 接进生产配置并跑完 30 分钟长稳（见 §1.3 / §1.4）；同日发现并修复了**冷启动竞态**
（§2.8）—— 它此前**每次上电都不出流**。

剩余的**全部是工程化收尾，不是功能**：

```text
[x] 冷启动（上电 → 3A 首次收敛）完整验证 —— 已做，并因此发现+修复一个严重缺陷（§2.8）
[ ] 网络断开/恢复专项测试（roadmap 列了，从未跑过）
[ ] 端到端延迟的正式测量（只有 ffplay ~0.75s 粗测，无文档化方法）
[ ] 架构图、演示视频
[ ] restart 端到端回归（本轮被 USB 掉链打断，只做了新判据的静态验证，见 §2.8 末）
```

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

### 1.3 OSD 上板验收：PASS，零可测开销（2026-09-29）

`--osd` 跑 300 帧：

```text
annotated=300   passed_through=0   composite_refused=0
sensor          polls=300  samples=300
Average FPS     30.003     （无 OSD 的基线 30.000 —— 差值在噪声内）
```

**像素级判定**（不是靠肉眼，也不是靠 MAD）：

| 区域 | 判定 | 实测 |
| --- | --- | --- |
| overlay 区 | 强像素占比（>40 灰阶变化） | **21.0%** |
| 对照带（同帧无人为改动区） | 同上 | **0.0%** |

> **为什么用「强像素占比」而不是 MAD**：overlay 是**尖锐/局部/高对比**的，
> 而曝光漂移是**平滑/全局**的。真机上 MAD 比值只有 1.68x，强像素比有 7.8x ——
> MAD 会把曝光漂移平均进去，把信号淹掉。**先想清楚「它和干扰在形状上差在哪」，
> 再去统计那个形状。**

一键验证脚本：`scripts/verify-osd.sh`（Windows 入口 `scripts/verify-osd.cmd`），约 40 秒。

### 1.4 真实 MPU6500 进生产配置（2026-09-29）

生产命令行（**唯一来源 = 仓库 `scripts/gateway.env`**）已加 `--osd --osd-source mpu6050`：

```text
GATEWAY_ARGS="-d /dev/video11 -w 1280 -H 720 --warmup 30 --sink rtsp --rtsp-port 8554
              --threads --ring-slots 4 --quiet --osd --osd-source mpu6050"
```

**30 分钟长稳**（`bash scripts/imu-soak.sh`，`--sink rtsp`）：

```text
Captured    54000 frames        Average FPS : 30.000
Ring        pushed=54001  dropped_busy=0  dropped_oldest=0  peak_depth=3
OSD         annotated=54000  passed_through=0  composite_refused=0
Sensor      polls=53672  samples=15057  no_sample=38614  errors=1
gpio70/71   无残留
```

| 指标 | 起点 | 结束 | 判读 |
| --- | --- | --- | --- |
| fds | 28 | 28（30 次采样**全部** 28） | 无泄漏。28 = 基线 24 + 2 引脚×2，与 `gpio_sysfs.c` 预开 fd 的设计吻合 |
| threads | 6 | 6 | 无线程泄漏 |
| RSS | 17444 KB | 17960 KB | 前 11 min +440 KB，后 11 min +68 KB，末尾连停 3 点 → **热身非泄漏** |
| CPU | 32% | 32% | 基线 14~17% → **约翻倍**（28 ms × 10 次/s ≈ 一个核的 28%） |
| 温度 | 55.9°C | 55.9°C | 基线 52.5°C，+3.4°C |

**真拉流抽帧确认叠加层到了客户端**（从 Windows 侧，这一条不能省）：

```powershell
ffmpeg -rtsp_transport tcp -i rtsp://172.32.0.93:8554/live/0 -t 8 -frames:v 2 out-%02d.png
```

两帧对比，**数值在变**（是活数据不是静态渲染）：

| | 帧 1 | 帧 2 |
| --- | --- | --- |
| FRAME | 2438 | 2451 |
| TEMP | 49.3 | 49.1 |
| ROLL | +0.2 | +0.5 |
| ACC | 1.00 | 0.99 |

面板内容：`PITCH/ROLL`、`ACC/TEMP`、`STATUS FRAME/FPS`、`IMU MPU6050 OK E=0`。

> ⚠️ **`annotated=N` 只证明编码器做了合成，不证明客户端收到了。**
> `--sink rtsp` 在没有客户端连接时 RTP 根本不发包。**生产配置上线后必须真拉一次流抽帧看。**

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

### 2.6 bit-bang 总线一次突发是 28 ms，不是「几毫秒」（2026-09-29，最贵的一个）

**现象**：把真实 IMU 接进数据面后，帧率从 30 塌到 **21.359 fps**、`dropped_busy=122`。

**根因**：软件 bit-bang I²C 的**一次 14 字节突发实测 ≈ 28 ms**（总线约 5kHz）。
这个代价全部花在**喂编码器的那个线程**上。代码注释里原先写的是 "a few milliseconds"
—— **错了十倍**，而帧率替这个错误买了单。

**量化手法（值得复用）**：让采样器按 10 ms 周期跑 20 s，只拿到 **713** 个样本
（理论 2000），反推每次循环 ≈ 28 ms。一次 `mpu6050_read()` 就是**单次 14 字节突发**
（没有多余事务），所以 28 ms 就是总线本身的代价，不是软件开销。

**模型**（`单次突发 28 ms` + `编码 ≈12 ms` vs `每帧预算 1000/采集帧率`）：

```text
采集 30 fps（33.3 ms 预算）：28 + 12 = 40 > 33.3  → 崩到 21.4 fps   ✓ 与实测吻合
采集 25 fps（40.0 ms 预算）：40 = 40              → 勉强，掉 7 帧     ✓ 与实测吻合
改成 100 ms 间隔 @30fps    ：每帧只摊 9.3 ms      → 21.3 < 33.3，有余量
```

**同场次三档对照**（`SWEEP_MS=10,50,100 bash scripts/fps-osd-compare.sh 300`）：

| 采样间隔 | fps | dropped_busy | captured |
| --- | --- | --- | --- |
| none（基线） | **30.001** | 0 | 301 |
| 10 ms | **21.409** | **122** | 426 |
| 50 ms | 26.612 | 38 | 340 |
| **100 ms（默认）** | **29.998** | **0** | 302 |

10 ms 那档**逐字复现**了最初的故障（122 / 426 / 21.409，首次是 122 / 426 / 21.359）
—— 它是**对照组里的「已知坏」档**，证明这台测量装置确实测得出效应。否则
「100 ms 与基线持平」就分不清是**改好了**还是**根本没测到**。

**修复**：

- `MPU6050_SOURCE_DEFAULT_MIN_INTERVAL_US` 10000 → **100000**（10 Hz）。不是 100 Hz：
  瓶颈在**总线**不在传感器。
- 新增 `--osd-imu-interval-ms N`（0~10000，0=用默认）。**这个 flag 必须存在**：正确值
  是那条总线的属性，只能实测找，而找的过程不能每次都回 VM 重编译一趟。
- 日志改成 `part at 100 Hz, bus read every 100 ms` —— 把**芯片自采样率**和**总线读取率**
  分开写。**把两者混在一起，正是这个成本被读错的原因。**

> ⚠️ **跨场次比 fps 是不可信的**：同一批实验里基线出现过 25 fps，因为脚本刚新起
> `rkaiq_3A_server`、**曝光还没收敛** → 采集只给 25 fps；收敛后才回到 30 fps。
> **必须同场次对照**，这就是 `fps-osd-compare.sh` 存在的理由。
> 报 fps 要连 `dropped_busy` 一起给：`0 + 低 fps` = 采集上限；`>0` = 编码真落后。

### 2.7 同一份配置写在多处 → 重装会静默回退（2026-09-29）

**现象**：把叠加层加进板上的 `/userdata/gateway.env` 之后，**任何一次重装或长稳都会
把它抹掉**，而且全程没有任何报错。表征是「OSD 某天开始不见了」，排查方向会指向
OSD 代码，而原因在一个**安装脚本**里。

**根因**：生产的 `GATEWAY_ARGS` 曾同时存在于**四个地方** —— `install_autostart.ps1`
（在内部拼字符串）、`start-2h-soak.sh`、`imu-soak.sh`、以及板上的 `/userdata/gateway.env`。

**修复**：唯一来源收敛到仓库 `scripts/gateway.env` —— 安装脚本**推**它，两个长稳脚本
**source** 它。安装脚本里唯一还自己算的是 `--quiet` 的能力检测（老二进制给了这个 flag
会拒绝启动），且明确只改 `GATEWAY_ARGS=` 那一行。

> **判据：如果一件事「改了 A 之后 B 会把它改回去」，那 A 不是配置，是副本。**
> **另一条实践**：改配置改**仓库那份**再重装，**不要直接在板上改** —— 板上那份是副本，
> 下一次重装会静默覆盖它。装完可用 `wc -c` + `diff` 核对两份是否逐字节一致。

### 2.8 冷启动从来就没成功过：接管逻辑与厂商启动链的竞态（2026-09-29）

**这是本项目最严重的一个缺陷：每次上电，板子都不出流。**

**现象**（`adb reboot` 后抓的现场）：

```text
gateway: supervisor not running
gateway: encoder not running
gateway: 3A server not running
gateway: warning, rkipc is running      <-- 只剩厂商的 rkipc
8554:    (没有监听)
```

`/tmp/gateway-boot.log` 全文就是失败过程：

```text
14:11:20 start requested
14:11:20 /userdata is ready
14:11:20 no rkipc, nothing is initialising the camera, proceeding   <-- 判断错了
14:11:21 rkipc pids before: []                                      <-- 杀了个空列表
14:11:23 rkipc survived as [709], the capture node stays busy       <-- 放弃
```

`rkipc(709)` 的 `starttime` = **746 ticks = 开机后 7.46 秒**，两个 fd（44、52）指向
`/dev/video11`。而 S99gateway 在**开机约 6 秒**就跑了 —— **它比 rkipc 早约 1.5 秒**。

**根因：`RkLunch.sh` 把 rkipc 放在一个后台函数里启动。**

```text
/etc/init.d/S21appinit  最后一行:  sh /oem/usr/bin/RkLunch.sh

RkLunch.sh:
  rcS()      跑 /oem/usr/etc/init.d/S??*
  post_chk() 等 /userdata 挂载 / insmod / network_init & / 多次 lsmod|grep
             / cp rkipc.ini / cp image.bmp / rk_mpi_ao_test（放测试 WAV）
             / luckfox-config / **最后一行才是 `rkipc -a ... &`**
  末尾:      post_chk &     <-- 后台执行，所以 S21appinit 立刻返回
```

于是"现在没有 rkipc"和"rkipc 正要来"是两件事，而代码把它们当成了一件：

| 场景 | S99gateway 看到的 | 真实情况 | 结果 |
| --- | --- | --- | --- |
| **冷启动** | 没有 rkipc | 1.5 秒后就来 | ❌ **放弃接管 → 开机无流** |
| **restart** | 没有 rkipc | 永远不会有（RkLunch.sh 只在开机跑一次） | ✅ 正确跳过等待 |

那句判断是上一轮修「restart 白等 20 秒」时加的 —— **那次修复本身没错**，
只是**缺一个"这次是开机还是重启"的信息**。

**曾试过但不可用的判据**：用「RkLunch.sh 进程还在不在」判断厂商链是否还在跑。
**不可行**：该进程长期残留（实测 uptime 224 秒仍在，ppid=1、状态 S、
`wchan=do_wait`、子进程列表为空）。**它的"不在"有意义，"在"没有意义。**

**修复**（`scripts/S99gateway`）

1. 新增 `uptime_seconds()`：读 `/proc/uptime`（板上没有 `stat`，这是唯一可用的单调时钟）。
2. 新增 `wait_for_late_rkipc()`：rkipc 缺席时**不立刻下结论**，等它，但有**三条退出路径**：

   | 退出条件 | 对应场景 |
   | --- | --- |
   | rkipc 出现了 | 冷启动的正常结局 |
   | 厂商链进程消失 | 链跑完了也没起 rkipc，确实没得等 |
   | **uptime 超过 30 秒** | 太晚不可能是开机 → 一定是 restart，没人会来 |

   第三条是关键：**uptime 正是上一轮缺的那个信息。**
3. `sleep 2` 盲等改成**有界轮询**，等 rkipc 进程和 `/dev/video11` 都真正空闲；
   并新增一条断言——rkipc 退出后如果还有**别的**进程占着节点，大声失败（而不是带着
   "device busy" 硬起）。

**复验：冷启动 PASS**（同一条路径，跑修好的脚本）

```text
14:20:28 start requested
14:20:32 rkipc appeared late (uptime 9s), it was on its way after all   <-- 等到了
14:20:35 rkipc pids before: [873]
14:20:37 rkipc is gone, /dev/video11 is free                            <-- 新断言
14:20:40 3A server running as 1850
14:20:42 gateway is up, supervisor pid 1864
```

| 时刻 | 事件 |
| --- | --- |
| boot+0 | 上电 |
| boot+5 | S99gateway `start requested`（与旧代码**同一时刻**，说明时序没变，变的是逻辑） |
| boot+9 | 迟到的 rkipc 被等到 |
| boot+14 | rkipc 已杀、节点空闲 |
| boot+19 | **gateway up** |

**产出确认**：8554 在听、gateway 1873 / 3A 1850 在跑、rkipc 已杀、
`IMU attached WHO_AM_I=0x70`；真拉流抽帧两帧成功，面板数值正常
（`PITCH -0.4 ROLL +3.3 / ACC 0.99g TEMP 46.3C / FRAME 1231 30.0 FPS / IMU MPU6050 OK E=0`）。

**restart 零回归**（板上静态验证新判据）：

```text
uptime_seconds       = 529
vendor_chain_running -> RUNNING（RkLunch.sh 长期残留，符合预期）
wait_for_late_rkipc  -> 返回 1，529s -> 530s      <-- 立即返回，没有白等 30 秒
```

> 🔴 **"开机 11 秒出流"这个数字要作废。** 它测的是一个**不可靠的路径**（S99gateway
> 恰好晚于 rkipc 时）。现在的诚实数字是 **上电到出流约 19 秒**（接管本身 14 秒，
> 其中包含等厂商链的 ~4 秒——这是必须付的代价），而旧代码在冷启动下是**永不**出流。
>
> 顺带纠正一条相关记录：`install_autostart.ps1` 里"禁用 rkipc init 脚本"那一步
> 在这块板上是**空操作**（安装日志原文：`no rkipc init script found (it may be
> started elsewhere)`）——因为 rkipc 根本不是 init.d 起的，是 `RkLunch.sh` 起的。

**教训**

- **"现在没有 X" 和 "X 不会来" 是两个判断。** 代码里凡是把两者合并的地方，都值得问
  一句"凭什么"。这里凭的是"重启时 rkipc 不会来"，而那句话只在重启时成立。
- **加一个"这次是开机还是重启"的信息，比猜一个超时值可靠得多。** uptime 是免费的。
- **修复新场景不能把旧场景改回去。** 上一轮修"白等 20 秒"是对的，所以这一轮的退出
  条件里必须有"uptime 超过阈值"，而不是简单地在缺 rkipc 时无条件多等 30 秒。
- **"只验证了一半"的地方，就是 bug 藏身的地方。** 交接文档里那句
  「冷启动只验证了一半（那次 3A 已在跑）」，指的正是这条路径。
- **判活不必依赖 adb。** 本次验证过程中 USB 掉链，RTSP 走 TCP/IP，
  `ping` + `/dev/tcp/172.32.0.93/8554` 就能判断板子和网关是否还活着。

---

## 3. 当前架构

### 3.1 两条流水线

```text
视频主线
  --threads 关闭（同步路径）
    V4L2 DQBUF -> 紧致化 NV12 -> Rockit VENC 编码 -> Sink

  --threads 开启（生产配置走的路径）
    采集线程: V4L2 DQBUF -> 紧致化 NV12 -> frame_ring
    主线程  : frame_ring -> Rockit VENC 编码 -> Sink

传感器支线（与视频流水线刻意零交集）
  MPU6050(bit-bang I²C) -> sensor_source -> sensor_ring -> sensor_attitude
                        -> osd_feed(50Hz) -> osd_overlay -> 合成进编码器输入的私有副本

Sink:  file | queue | rtsp (RTSP over TCP, 多客户端上限 4)
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

src/sensor_source.h         Sensor 抽象接口（mock 与真实 IMU 共用）
src/mpu6050_source.c / .h   真实 MPU6050 源；convert 是纯算术（host 可测），
                            open/read/close 在 __linux__ 内（要调驱动）
src/mpu6050.c / .h          寄存器编解码、标定、量程换算
src/mpu6050_i2c.c           bit-bang I²C 时序（建在 gpio_sysfs.c 之上）
src/sensor_ring.c / .h      有界样本环（head/tail/count 模型）
src/sensor_attitude.c / .h  pitch/roll 解算
src/mock_sensor.c           假数据源（故障注入、wave 模式）
src/osd_*.c / .h            OSD 各层：font / overlay / format / telemetry / feed / annotate
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
make test          # 十个测试二进制 + 两项标志检查
make host-syntax   # MinGW 下对含 socket / V4L2 的文件做语法检查
make board-flags   # 同上，但用板端的 CPPFLAGS/CFLAGS
```

| 套件 | 检查数 | 覆盖内容 |
| --- | --- | --- |
| `test-packet-queue` | 43 | 队列顺序、零拷贝借用、丢整个 GOP |
| `test-rtp-rtsp` | 86 | RTP 打包、SDP、请求解析 |
| `test-frame-ring` | 64 | 帧环顺序、覆盖最旧、生产者消费者并发 |
| `test-capture-thread` | 13 | 采集成帧、warmup 过滤、故障停机、stop 不挂起 |
| `test-mpu6050` | 105 | 寄存器编解码、标定、量程换算 |
| `test-i2c-bitbang` | 50 | 时序边沿序列（录制式 GPIO 假后端） |
| `test-mpu6050-source` | 28 | 真实 IMU 源：解码→校准→姿态、静止 `\|a\|=1.000`、量程不符必须拒绝 |
| `test-sensor` | **622 ~ 697（浮动）** | 数学、姿态、mock、sample ring、接口贯通 |
| `test-osd` | 143 | 点阵字体、定点格式化、1bpp 画布、遥测行 |
| `test-osd-pipeline` | 39 | 采样节奏、陈旧样本、速率窗口、annotate 只写副本 |

本轮实测合计 **1205 项，0 失败**（总数随 `test-sensor` 浮动，说明见下方）；
`board-flags` 与 `host-syntax-can-fail` 另计。

> **`test-sensor` 的检查项数量每次运行都不固定**（实测 622 ~ 697，**全部 PASS**）。
> 原因：`tests/test_sensor.c` 里 `test_ring_producer_consumer()` 有一个真线程的
> 生产者/消费者用例，每消费一个样本计一次 CHECK，消费多少取决于调度。
> **这不是失败，别去「修」它；也别把某个固定数字写进文档。**

### MinGW 限制

`src/v4l2_capture.c` 与 `src/v4l2_mpp_encode.c` 依赖 `linux/videodev2.h`，MinGW
**整个 include 树里都没有任何 Linux 内核头**。过去这两个文件只能在虚拟机里检查，
因此编码器（所有集成的落点）的每次改动在主机侧都是「盲改」。

现在 `tests/host-stubs/linux/videodev2.h` 按其**实际用到的符号**补齐了声明，
配合 `-D__linux__ -include extra.h`，`make host-syntax` / `make board-flags`
可以对它做零告警语法检查。该桩**只声明用到的名字**（猜全量会接受真实头文件会拒绝的
代码）、**不建模 ABI**、绝不用于链接或运行。

`make host-syntax-can-fail` 是这两个目标的**体检**：复制一份编码器、把一个结构体
成员写错，断言编译器必须拒绝。桩文件若哪天与源码脱节，前两个目标会一边打印成功一边
什么都不查——一个不会变红的绿灯比没有检查更糟，因为它会被信任。该目标一旦失败，
在修好桩文件之前不要相信另外两个。

---

## 5. 板端操作备忘

### 起流

```bash
# 开机自启已装好的情况下，什么都不用做
# 手动：
/userdata/gateway-supervise.sh &
```

### 生产命令行：唯一来源是仓库，不是板子

```bash
# 安装（只装，不碰正在跑的进程）
powershell -File scripts/install_autostart.ps1
# 装完立刻起
powershell -File scripts/install_autostart.ps1 -Start
# 让新的 GATEWAY_ARGS 生效
adb shell "/etc/init.d/S99gateway restart"

# 核对板上副本与仓库源文件是否逐字节一致
export MSYS_NO_PATHCONV=1
adb shell "wc -c < /userdata/gateway.env"      # 应与本地一致（当前 2522）
diff <(tr -d '\r' < scripts/gateway.env) <(adb shell "cat /userdata/gateway.env" | tr -d '\r')

# 回滚（备份是本轮之前的无 OSD 配置，仍有效）
adb shell "cp /userdata/gateway.env.bak /userdata/gateway.env"
adb shell "/etc/init.d/S99gateway restart"
```

> 🔴 **改配置改 `scripts/gateway.env`（仓库）再重装，不要在板上直接改** ——
> 板上那份是它的副本，下一次重装会静默覆盖，而且没有任何报错。

### 叠加层是活的还是降级了

```bash
adb shell "grep -E 'IMU attached|IMU unavailable' /userdata/gateway.log | tail -2"
```

判活必须**从另一端真拉流抽帧**（`annotated=` 计数器只证明编码器做了合成）：

```powershell
ffmpeg -rtsp_transport tcp -i rtsp://172.32.0.93:8554/live/0 -t 8 -frames:v 2 out-%02d.png
```

### 真实 IMU 的一键验证 / 长稳

```bash
bash scripts/imu-soak.sh                  # 完整长稳，**不依赖 imu-sample**
FRAMES=300 bash scripts/imu-soak.sh       # 快速回归版
bash scripts/verify-imu.sh                # 需要板端先编好 imu-sample（见下）
```

> ⚠️ `make clean` 之后 `imu-sample` **常被漏编**，而 `verify-imu.sh` 缺它会直接 die。
> 快速回归请用上面那条 `FRAMES=300`。

### 长稳采样

```bash
# 启动
adb shell "setsid /userdata/soak-monitor.sh 60 >/dev/null 2>&1 < /dev/null & sleep 2; echo started"
# 停止
adb shell "kill \$(cat /tmp/soak-monitor.pid)"
```

健康基线（无客户端、无 OSD）：**RSS 11.5MB、fds 24、threads 5、CPU 14~17%、
温度 52.5°C、可用内存 155MB**。长稳要看的是这些数字**是否随时间单调上升**，而不是绝对值。

**开了真实 IMU 叠加后**（生产配置，2026-09-29）：**fds 28**（= 24 + 2 引脚×2）、
**threads 6**、**CPU 32%**、**温度 55.9°C**、RSS 17.4~18.0MB。这些是 IMU 的固定代价，
不是泄漏 —— 但判据不变：看尾部是否仍在爬。`--quiet` 下**线程数是唯一判据**
（日志被抑制了）。

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
   （当时写的"下一步：Mock Sensor → OSD"后来都已闭环，见下面第 4/5 项。）
3. **✅ `S99gateway restart` 白等 20 秒已修复**（见 §7），并顺带修掉了
   `LD_LIBRARY_PATH` 重复追加（`S99gateway` 与 `gateway-supervise.sh` **两处都有**）、
   以及 Windows 检出导致的 CRLF shebang 问题。
4. **✅ Mock Sensor → OSD 数据面已完成**（2026-09-29，见 §1.3）：`sensor_source` 接口、
   有界样本环、姿态解算、1bpp 画布 OSD 全部实现；`--osd` 上板 300/300 帧合成成功，
   **帧率零开销**。
5. **✅ 真实 MPU6500 接进数据面并进入生产配置**（2026-09-29，见 §1.4 与 §2.6）：
   `mpu6050_source` 实现 `sensor_source`，`--osd-source mpu6050` 已进 `gateway.env`；
   30 分钟 54000 帧 30.000 fps；解决了 bit-bang 总线 28 ms 导致的帧率塌陷
   （根因与三档对照见 §2.6）。
6. **✅ 生产配置收敛为单一来源**（2026-09-29，见 §2.7）：`scripts/gateway.env`。
7. **✅ 冷启动竞态已发现并修复**（2026-09-29，见 §2.8）：这是唯一一次"按计划去验证
   一个已知的未验证路径，结果是坏的"。**每次上电都不出流**，根因是接管逻辑与
   `RkLunch.sh` 后台启动 rkipc 的竞态；用 `/proc/uptime` 做开机/重启判别后修复，
   冷启动复验 PASS（上电到出流约 19 秒），restart 路径经判据静态验证无回归。

**剩下的是 P2 工程化收尾，都不是功能**：

```text
[x] README.md / docs/handoff.md / roadmap.md 过时表述（2026-09-29 已对齐现状）
[x] 冷启动（上电 → 3A 首次收敛）完整验证 —— 已做，并发现+修复严重缺陷（§2.8）
[ ] 网络断开/恢复专项测试（roadmap 列了，从未跑过）
[ ] 端到端延迟的正式测量（只有 ffplay ~0.75s 粗测）
[ ] 架构图、演示视频
[ ] restart 端到端回归（本轮被 USB 掉链打断）
```

> 最新、最全的完成清单与每项证据见 **`docs/agent-handoff.md`**。

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
