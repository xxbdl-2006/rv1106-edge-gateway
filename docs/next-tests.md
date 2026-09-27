# 下一轮板端测试指令

本文档给出两组可直接复制的指令。板子到手后按顺序执行即可。

前置：**必须在 Ubuntu VM 里重编**，用 `mingw32-make test` 编出来的 x86 版本在板上跑不了
（`--help` 会报 `/bin/sh: not found`）。

```bash
cd /mnt/hgfs/luckfox_share/rv1103
make clean
make CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
```

---

## 一、下一步测试：验证 `--threads`（线程化采集路径）

目标：确认「采集线程 + Frame Ring Buffer」这条新路径在真硬件上不出问题，并且
**与默认同步路径的产物一致**。

判据只有三条：

1. 线程化路径能正常出流、ffplay 可播；
2. `Ring dropped_oldest=0`（说明编码没跟不上采集，环没溢出）；
3. 两个路径的**帧数和平均 FPS 基本一致**。

> 注意：`dropped_oldest=0` 不等于"路径没在工作"。真正说明线程化生效的是启动横幅里的
> `Capture mode : thread + frame ring`，以及统计行里 `popped` 与 `captured` 同步增长。

### 1.1 准备板端环境

```powershell
# Git Bash 里跑 adb 前必须加这句，否则 /userdata/... 会被改写成 Windows 路径
$env:MSYS_NO_PATHCONV=1
```

推二进制（重编后必做）：

```powershell
adb push F:\luckfox_share\rv1103\v4l2_mpp_encode /userdata/
adb shell "chmod +x /userdata/v4l2_mpp_encode"
adb shell "pidof v4l2_mpp_encode; pidof rkipc"
```

停网关、杀 rkipc、起 3A（**顺序不能反**，否则采集节点被占、3A 脚本静默退出）：

```powershell
# 1. 确认网关已停，避免和下面的手动测试抢采集节点
adb shell "/etc/init.d/S99gateway stop"

# 2. 杀 rkipc（必须确认无输出），起 3A
adb shell "killall -9 rkipc; sleep 2; pidof rkipc"
adb shell "sh /userdata/start_rkaiq.sh"
adb shell "pidof rkaiq_3A_server"                          # 必须有输出
```

### 1.2 冒烟：先各跑 30 秒

先确认能起来，再跑长测。

```powershell
# A. 同步路径（基线）
adb shell "/userdata/v4l2_mpp_encode -d /dev/video11 -w 1280 -H 720 -n 900 --warmup 30 --sink file -o /userdata/sync.h264" 2>&1 | Select-String "Capture mode|Captured|Average FPS"
```

```powershell
# B. 线程化路径
adb shell "/userdata/v4l2_mpp_encode -d /dev/video11 -w 1280 -H 720 -n 900 --warmup 30 --sink file --threads -o /userdata/thread.h264" 2>&1 | Select-String "Capture mode|Ring|Capture captured|Captured|Average FPS"
```

期望看到（B 比 A 多两行统计）：

```text
Capture mode : synchronous          /  Capture mode : thread + frame ring
Ring pushed=... popped=... dropped_oldest=0 dropped_busy=0 peak_depth=...
Capture captured=900 skipped=30 errors=0
Captured     : 900 frames
Average FPS  : 30.0xx
```

**这一步的硬判据**：B 里 `dropped_oldest=0`、`errors=0`、`Average FPS` 与 A 相差不超过 1%。

### 1.3 拉 RTSP 看画面

```powershell
adb shell "setsid nohup /userdata/v4l2_mpp_encode -d /dev/video11 -w 1280 -H 720 --warmup 30 --sink rtsp --rtsp-port 8554 --threads > /userdata/thread-test.log 2>&1 < /dev/null & sleep 2; echo launched"
adb shell "pidof v4l2_mpp_encode"
adb shell "head -n 12 /userdata/thread-test.log"
```

Windows 侧播放，**至少看 5 分钟**：

```powershell
ffplay -rtsp_transport tcp -fflags nobuffer rtsp://172.32.0.93:8554/live/0
```

播放期间抽查这两项：

```powershell
adb shell "grep -a -c 'RTSP: RTSP/1.0' /userdata/thread-test.log"   # 必须为 0（未失步）
adb shell "wc -c < /userdata/thread-test.log"                        # 应只有几 KB
```

停止：

```powershell
adb shell "kill $(adb shell pidof v4l2_mpp_encode)"
adb shell "tail -n 6 /userdata/thread-test.log"                      # 看 Ring / Capture 统计
```

### 1.4 与默认路径做产物比对（可选，但很有说服力）

同一段场景各录 300 帧，比较码率与帧数；两者**不应逐字节相同**（线程化会让编码时序略有差异），
但帧数和码率应在同一量级：

```powershell
adb pull /userdata/sync.h264   F:\luckfox_share\sync.h264
adb pull /userdata/thread.h264 F:\luckfox_share\thread.h264
```

```powershell
ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames,bit_rate -of default=nw=1 F:\luckfox_share\sync.h264
ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames,bit_rate -of default=nw=1 F:\luckfox_share\thread.h264
```

### 1.5 通过标准

| 项 | 通过条件 |
| --- | --- |
| 启动横幅 | 确认为 `thread + frame ring` |
| `dropped_oldest` / `dropped_busy` | 均为 0 |
| `Capture errors` | 0 |
| Average FPS | ≥ 29.9，与同步路径差 < 1% |
| ffplay 5 分钟 | 无花屏、无卡顿、无报错 |
| `grep -c 'RTSP: RTSP/1.0'` | 0（未失步） |

任何一项不过：**先回到不带 `--threads` 的路径**确认它是好的，再单独排查线程化路径。

---

## 二、2 小时测试（同时作为 8 小时长稳的第一段）

目标：拿到**一次干净收尾**的长跑记录 —— 有 `progress=end`、有 `exit=0`。
之前两段（1h53m + 4h36m，共 70 万帧零丢帧）都是被主机侧打断的，缺的正是这个收尾。

### 2.1 启动网关 + 长稳采样器

```powershell
$env:MSYS_NO_PATHCONV=1

# 1. 先写好运行参数。gateway-supervise.sh 从 /userdata/gateway.env 读，
#    改参数不用碰脚本。下面这份是线程化路径；要测默认路径就把 --threads 去掉。
adb shell "echo 'GATEWAY_ARGS=\"-d /dev/video11 -w 1280 -H 720 --warmup 30 --sink rtsp --rtsp-port 8554 --quiet --threads --ring-slots 4\"' > /userdata/gateway.env"

# 2. 停掉任何在跑的实例，清空旧日志
adb shell "/etc/init.d/S99gateway stop"
adb shell ": > /userdata/gateway.log"
adb shell "rm -f /userdata/soak.csv"

# 3. 起网关。必须走 S99gateway start —— 它会先等摄像头真正就绪再接管，
#    然后由它去拉 supervisor。直接跑 gateway-supervise.sh 会跳过这段等待。
adb shell "/etc/init.d/S99gateway start"
adb shell "sleep 12; /etc/init.d/S99gateway status"

# 4. 起采样器：每 60 秒一行
adb shell "setsid /userdata/soak-monitor.sh 60 >/dev/null 2>&1 < /dev/null & sleep 2; echo started"
adb shell "pidof v4l2_mpp_encode"
```

确认横幅里是本轮要测的路径：

```powershell
adb shell "head -n 12 /userdata/gateway.log"    # 看 Capture mode 那行
```

### 2.2 用 `-t` 让客户端到点自退（关键）

**不要靠键盘 Ctrl+C 停 ffmpeg** —— 那样拿不到干净的退出码。用 `-t 7200`（2 小时）让它自己结束：

```powershell
$log  = "D:\luckfox_share\soak-2h-$(Get-Date -Format yyyyMMdd-HHmmss).log"
$prog = "D:\luckfox_share\soak-2h-progress.txt"

ffmpeg -rtsp_transport tcp -i rtsp://172.32.0.93:8554/live/0 `
       -c copy -t 7200 -nostats -progress $prog -y D:\luckfox_share\soak-2h.h264 2> $log

# 必须落退出码，这是"干净收尾"的唯一凭据
"exit=$LASTEXITCODE" | Tee-Object -FilePath $log -Append
```

跑完检查（三条都要对）：

```powershell
Get-Content $prog | Select-String "progress=" | Select-Object -Last 3   # 末行应为 progress=end
Get-Content $log  | Select-String "exit="                               # 应为 exit=0
Get-Content $prog | Select-String "drop_frames|dup_frames" | Where-Object { $_ -notmatch "=0$" }   # 应无输出
```

### 2.3 中途（建议第 1 小时）抽查一次

```powershell
adb shell "tail -n 5 /userdata/soak.csv"
adb shell "grep -a -c 'RTSP: RTSP/1.0' /userdata/gateway.log"   # 必须为 0
adb shell "pidof v4l2_mpp_encode"
```

CSV 列含义（顺序固定）：

```text
time, uptime_s, pid, rss_kb, fds, threads, cpu_pct, temp_mc,
encoder_alive, supervisor_alive, three_a_alive, free_mem_kb, gw_log_bytes
```

**健康基线（无客户端）**：RSS 11.5MB、fds 24、threads 5、CPU 14~17%、温度 52.5°C、可用内存 155MB。

要看的不是绝对值，而是这四项**是否随时间单调上升**：

| 列 | 危险信号 |
| --- | --- |
| `rss_kb` | 持续爬升 → 泄漏 |
| `fds` | 持续爬升 → socket / 文件没关 |
| `threads` | 持续爬升 → 线程泄漏（多次连断后才暴露） |
| `temp_mc` | 持续上升 → 会触发降频、静默丢帧 |

注意 `temp_mc` 单位是**毫摄氏度**，52500 = 52.5°C。

### 2.4 收尾

```powershell
# 停采样器
adb shell "kill $(adb shell cat /tmp/soak-monitor.pid)"

# 取回数据
adb pull /userdata/soak.csv D:\luckfox_share\soak-2h.csv
adb pull /userdata/gateway.log D:\luckfox_share\gateway-2h.log
```

### 2.5 2 小时的交付物（缺一不可）

| 文件 | 证明什么 |
| --- | --- |
| `soak-2h-progress.txt`，末行 `progress=end` | 客户端是正常结束的，不是被打断 |
| 日志里的 `exit=0` | 同上，且是最硬的凭据 |
| `drop_frames` / `dup_frames` 全程为 0 | 端到端零丢帧 |
| `soak-2h.csv` | 无内存/fd/线程/温度泄漏趋势 |
| `gateway.log`（几 KB，无失步计数） | 服务端侧干净 |

### 2.6 2 小时通过后就能顺推到 8 小时

同样流程，只改 `-t 28800`。2 小时先跑一遍的价值是：**把"能干净收尾"这件事先证明掉**，
8 小时只是同一件事跑久一点，风险面不会变大。

---

## 三、常见故障对照

| 现象 | 原因 |
| --- | --- |
| `VIDIOC_S_FMT: Device or resource busy` | rkipc 没杀干净，或 3A 没起 |
| `can't load library 'librkaiq.so'` | 没 `. /etc/profile.d/RkEnv.sh` |
| `--help` 报 `/bin/sh: not found` | 推了 x86 版本，需在 VM 里重编 |
| adb push 后以 `/` 开头的参数变成 `C:/...` | 没 `export MSYS_NO_PATHCONV=1` |
| 后台进程起不来、日志文件都没创建 | `setsid ... &` 后面缺 `sleep 2`，adbd 杀了进程组 |
| `Ring dropped_oldest` 持续增长 | 编码跟不上采集，考虑加大 `--ring-slots` 或降码率 |
