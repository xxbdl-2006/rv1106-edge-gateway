# 项目对接文档（给下一个 Agent）

> 建立时间：2026-09-29 12:10（GMT+8）
> 仓库：`github.com/xxbdl-2006/rv1106-edge-gateway`，分支 `main`，HEAD = `4c39f13`，工作区干净。
> 本文件由实测整理而成，**与 `README.md` / `docs/handoff.md` 冲突时以本文件为准**（那两份文档已过时，见 §8）。

---

## 0. 三十秒速览

```text
一句话：一块 Luckfox Pico Pro/Max（RV1106G）上的边缘视频网关，
        摄像头 → V4L2 → Rockit MPI H.264 → RTSP over TCP，
        外加一条「传感器 → OSD 叠加」的支线；视频主线已全部上板验证通过，
        OSD 支线已通到「能烧进码流」，但喂它的数据目前还是 mock（假）的。

当前可对外播放：rtsp://172.32.0.93:8554/live/0  （1280x720 H.264，30fps）
当前最大缺口  ：src/mpu6050_source.c 不存在 —— 真实 IMU 数据还没接进来。
```

维基式结论：**主线闭环完成且验证充分；支线缺最后一段数据源。**

---

## 1. 环境与分工（物理约束，别搞反）

| 角色 | 在哪台机器 | 能做什么 |
| --- | --- | --- |
| **板子** | USB 接在 **Windows** 上 | 只跑二进制；`adb`，RNDIS `172.32.0.93`，adb id `1fb4645eb23b6f31` |
| **Ubuntu VM** | 只做交叉编译 | `make CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-`，SDK 在 `/home/aaazhx/luckfox-pico` |
| **代码共享** | VM 的 `/mnt/hgfs/luckfox_share/rv1103` == Windows 的 `F:\luckfox_share\rv1103` | 同一份，改哪边都等价，编译产物自动可见 |

🔴 **Windows 侧没有 ARM SDK，编不了板端程序**（只有 MinGW gcc，只能编 x86 自测）。
🔴 **板上没有 gcc / make**，只有 python3。
**流程只能是：VM 编译 → Windows 推二进制 → 板子跑验证。** 试图在 Windows 上直接编译板端程序必然失败（症状是板端 `--help` 报 `/bin/sh: not found` —— 那是推了 x86 版本）。

---

## 2. 已完成（每项都有上板实测证据）

### 2.1 视频主线 —— 全部完成并通过

| 模块 | 证据 |
| --- | --- |
| V4L2 采集 1280x720 NV12 | 长期运行取到数目一致的帧数 |
| Rockit MPI H.264 编码 | Baseline、CBR 2Mbps，VLC/ffplay 可播 |
| Packet Sink 抽象（file / queue / rtsp） | `make test` `test-packet-queue` 43 项 |
| **RTSP over TCP + RTP** | `rtsp://172.32.0.93:8554/live/0`，ffplay 延迟约 0.75s |
| **多客户端 fan-out**（上限 4） | 4 路同拉，关任一路不影响其他 |
| **Frame Ring + 采集线程**（`--threads`） | `test-frame-ring` 64 项；8 小时长稳走的是这条路径 |
| **开机自启** | `adb reboot` 后 11 秒自动出流 |
| **8 小时长稳（线程化）** | **864,001 帧 / 08:00:00.033 / 30.00fps / drop=0 dup=0 / exit=0 / 失步 0**；零泄漏（fds 24、threads 6 全程不动，末 2 小时 RSS 增长 0 KB） |

### 2.2 MPU6050 —— 硬件链路完成，**只差把它接进数据面**

已完成且上板验证：

- 接线定死：**排针 pin 14 (SDA) / pin 24 (SCL)**，走 bit-bang GPIO（因为运行时 device-tree overlay 在这块板上是坏的，写入路径哑掉）。
- C 版驱动读通：`0x68: present`、`WHO_AM_I=0x70`（MPU6500 料，寄存器布局同 MPU6050）、**`0 io errors`**、校准后 `|a| = 0.996`。
- 校准常数已固化在 `src/mpu6050.h`（**别再手工抄一份**）。注意 Z 轴偏置是 `17194.92 - 16384 = 810.92` 而不是原始均值（见 §6 红线）。
- 板端黄金对照仍在：`/userdata/i2c-bitbang.py`（Python 版，同一块板同一颗芯片同一组引脚）。
  **Python 找得到而 C 版找不到 = 移植错了，不是硬件问题。**

🔴 **未完成的部分**：`src/mpu6050_source.c` **文件根本不存在**。主程序的 `--osd-source SRC` 只接受 `mock`。

### 2.3 Sensor 数据面 —— 已完成

`src/sensor_source.h` / `sensor_ring.c` / `sensor_attitude.c` / `mock_sensor.c` / `sensor_math.h`（无 libm）。
`test-sensor` 通过，**但注意检查项数字每次运行不同**（见 §7.3，不是 bug）。

### 2.4 OSD —— 完成并上板验证 PASS（2026-09-29）

- 层内容：`osd_font`（5x7 点阵，生成物）/ `osd_overlay`（1bpp 画布 + NV12 合成）/ `osd_format`（定点格式化）/ `osd_telemetry`（遥测行）/ `osd_feed`（50Hz 采样）/ `osd_annotate`（私有副本里合成）。
- **上板实测结果**：`--osd` 跑 300 帧 → `annotated=300 passed_through=0 composite_refused=0`，sensor `polls=300 samples=300`，**30.003 fps vs 无 OSD 30.000 fps（零可测开销）**；像素判定 overlay 区 21.0% 像素变化 >40 灰阶、对照带 0.0%；裁剪图肉眼可见面板。
- 一键验证脚本存在且已跑通：`scripts/verify-osd.sh`（Windows 入口 `scripts/verify-osd.cmd`）。

⚠️ **当前生产配置没有开 OSD**：板端 `/userdata/gateway.env` 是
`GATEWAY_ARGS="-d /dev/video11 -w 1280 -H 720 --warmup 30 --sink rtsp --rtsp-port 8554 --threads --ring-slots 4 --quiet"`
—— 里面**没有 `--osd`**。所以直播流现在是干净画面。这是有意的（OSD 刚验证完，还没决定并入默认配置）。

---

## 3. 未完成（按推荐优先级）

### P0 — 真实 IMU 数据源（唯一的架构性缺口）

- **目标**：让 `--osd-source mpu6050` 可用，屏幕上出现真实姿态。
- **缺失文件**：`src/mpu6050_source.c`（不存在）。
- **契约照抄**：`sensor_source.h` 的 `struct sensor_source` 接口，参考实现 `mock_sensor.c`。
- **底层已经好了**：`mpu6050_open / mpu6050_read_accel_gyro / mpu6050_apply_calibration` 都在 `src/mpu6050.c`，上板跑过。
- **改哪里接线**：`src/v4l2_mpp_encode.c` 的 `--osd-source` 解析（当前只认 `mock`，帮助文本在 ~line 116）。
- **验收判据**（抄 §16 传感器标准 + 本项目约束）：
  - 静止时 `|a|` 校准后必须是 `1.000 ± 0.02` —— **不是 0.000**（0.000 = 偏置把重力吃掉了）；
  - 校准前的原始 `|a|` 约 1.05，校准后要求回到 1.000，两者都要打印，差值就是这份倾斜；
  - 100 Hz 连续 10 分钟 `0 io error`；
  - **引脚自清理**：跑完 `gpio70/71` 无残留 export。
- **注意**：bit-bang I2C 走的是 GPIO 而非 `/dev/i2c-N`，板上**没有** i2c 设备节点，别去找。

### P1 — 决策：OSD 是否并入默认生产配置

- 当前 OSD 只在 `--sink file` 路径上做过板端验证；**`--sink rtsp` 带 OSD 的真实推流还没跑过**。
- 如果开：编辑 `/userdata/gateway.env` 加 `--osd --osd-source mock`（先 mock），或直接接 P0 的真源。

### P2 — 工程化收尾

| 项 | 状态 |
| --- | --- |
| `README.md` / `docs/handoff.md` 内容过时 | 仍写着「尚未实现 OSD 和 MPU6050」「待 8 小时验收」，实际都已完成——本次已修 README 的"当前状态/后续路线"两处，handoff §3/§13/§14 仍旧 |
| 网络断开/恢复的专项测试 | roadmap 列了，**没跑过** |
| 架构图、演示视频 | roadmap「最终交付物」列出，**未产出** |
| 延迟的正式测量 | 只有 ffplay 约 0.75s 的粗测，无文档化方法 |
| `docs/status.md` 更新到 2026-09-29 | 最新文件写到 2026-09-28，缺 OSD 上板章节 |
| 仓库根目录有一批散落的测试 `.exe` 与 `op.log`、`.prev` | 已被 .gitignore 覆盖，git 状态干净 |

---

## 4. 命令速查

```bash
# 交叉编译（必须在 VM）
cd /mnt/hgfs/luckfox_share/rv1103
make clean && make CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
od -An -tx1 -j18 -N2 v4l2_mpp_encode     # "28 00" = ARM，别只看文件大小

# 主机自测（Windows MinGW）
PATH="/d/path/c_c++/mingw64/bin:$PATH" mingw32-make test HOSTCC="D:/path/c_c++/mingw64/bin/gcc.exe"
mingw32-make host-syntax board-flags HOSTCC="..."

# 推板——每次都要核字节数（ext4 跨重启保留，推送被打断会留 0 字节空文件）
export MSYS_NO_PATHCONV=1                 # 必须！否则 /userdata 被改写成本地路径
adb push v4l2_mpp_encode /userdata/
adb shell "wc -c < /userdata/v4l2_mpp_encode"

# OSD 上板验证（Windows，约 40 秒，自动停网关、录两段、比像素、还原网关）
bash scripts/verify-osd.sh --verify-only   # 或双击 scripts/verify-osd.cmd

# MPU6050 驱动一键验证
bash scripts/verify-mpu6050.sh            # 第 1 步（找工具链）只能在 VM 跑

# 网关状态（判活用进程名 v4l2_mpp_encode，不是 gateway）
adb shell "pidof v4l2_mpp_encode; pidof gateway-supervise.sh; pidof rkaiq_3A_server"
```

---

## 5. 架构红线（破坏了会引入很难查的 bug）

1. **socket 只出现在 `src/rtsp_server.c` 一个文件里**；协议/SDP 在 `rtsp_proto.c`，RTP 打包在 `rtp_h264.c`。新增网络功能沿用此划分，这样 Windows 上才有得测。
2. **多客户端是三层线程**：listener / reader（队列的唯一消费者，每帧只打包一次后扇出）/ 每连接一个 client。**别改回"每个客户端各自取队列"** → 全员花屏。
3. **任何 RTSP handler 不得持锁调用 `client_send_response()`**，否则死锁。
4. **主循环禁止会增长的动态分配**（handoff §18）。
5. **`frame_ring` 借出的缓冲是 `const`（契约）**：不能就地涂改，OSD 那条路径正是在私有副本里合成才合法。
6. **`composite_nv12()` 刻意没有 `channel_off` 参数**（清除像素 = 透明），别加回去，否则整片糊成暗块。
7. **画布宽度按 `OSD_TELEMETRY_WIDTH`(48) 分配，别改成行缓冲容量 64** —— 合成对放不下的帧是**拒绝**而非裁剪，改了 overlay 会在窄帧上静默消失。
8. **无样本打印 `--`，绝不打印 `0.0`**（0 是真实水平读数，把"没数据"显示成"水平静止"最误导人）。
9. **MPU6050 的 accel Z 偏置是 `810.92`（原始均值减 16384）**：静止时加速度计读的就是重力，直接减原始均值会让三轴读 0.00g、姿态永不收敛，**而且不报错**。
10. **`SENSOR_SRC` 不进 `MEDIA_SRC`**（传感器与视频流水线刻意零交集）。

---

## 6. 传感器 / 数学约定（改之前必读）

- **环模型**：`head`=下一写、`tail`=最旧、`count`=占用。**`push()` 返回 0 = "调用被接受" ≠ "已存"**（满环拒绝也返回 0）。判据是恒等式 `pushed == popped + dropped_oldest + depth`。
- **`dropped_oldest` 只计被覆盖的**，拒绝入队不计。
- **三角**：sin 折 `[pi,2pi)` 必须 `x -= pi; sign = -sign`（`2pi-x` 是反射，丢符号）；cos 独立缩约；atan 半角缩约（Taylor 在 t=1 收敛慢）。
- **mock 加速度**：`ax=-sin(p)`、`ay=sin(r)*cos(p)`、`az=cos(r)*cos(p)` —— **ax 没有 `cos(r)` 因子**。
- **偏置是原始计数**：换量程就不能用，`mpu6050_apply_calibration()` 会显式拒绝非 ±2g/250dps。
- 校准前先问「这个量静止时的真值是多少」：陀螺仪 0，加速度计 **1g**。

---

## 7. 已知的坑（花钱买来的，别重蹈）

### 7.1 只有真设备才暴露的 bug

- **sysfs GPIO 写序**：`direction=in` 时写 `value` 返回 EPERM。必须**先 `direction=out` 再写 `value=0`**（与裸机习惯相反）；**读前必须显式切 `in`**（否则读到输出锁存器）。当年症状：C 版 112 个地址全 transport error，Python 版同板同脚正常 —— **有对照就能一步把问题一分为二**。
- **运行时 device-tree overlay 是坏的**：写合法 dtbo 和写 GARBAGE 都 rc=0。**验证写入路径是否生效，喂必然非法的东西看它报不报错。**

### 7.2 环境类

- Git Bash 调 adb 前 `export MSYS_NO_PATHCONV=1`（`adb push/pull` 的独立路径参数受害最深，引号内路径不受影响）。`verify-osd.sh` 内部已自带。
- **不要并行发 adb 命令**（互杀 daemon）。
- **`adb pull` 目标必须写 Windows 路径**（`D:\...`）。
- **推脚本前查 CRLF**：`od -c f | grep -o '\r' | wc -l`（`grep -c $'\r'` 会**漏报**，别信它）。
- **`kill -0` 在这块板上对死进程也返回成功** → 等退出用 pidof 轮询；pidof **别放进管道**（拿到的是 `tr` 的状态）；pidof 为空时别拼 `/proc/$pid/`（会读到内核 boot args）。
- 板上进程名：**网关 = `v4l2_mpp_encode`**，没有叫 gateway 的进程。无 `stat`（用 `wc -c < f`）、无 `pgrep`；`killall` 存在但什么都不做。
- `setsid ... &` 拉后台，父进程要**多活 1~2 秒**（`sleep 2`），否则 adbd 杀进程组。
- **ffmpeg 所有输出都在 stderr**，长跑用 `-nostats -progress <file>`。
- `rkaiq_3A_server` 必须在跑，否则 25fps + 画面暗绿；rkipc 会占 `/dev/video11`。
- **推送前先测网络**：直连和 `127.0.0.1:7897` 各打一次 `%{http_code}`；工具环境里注入的随机高位端口代理只服务自身，会返回 502。

### 7.3 测试相关

- **`test-sensor` 的检查项数量每次运行都不固定**（实测 622 ~ 697，全部 PASS）。原因：`tests/test_sensor.c` 里 `test_ring_producer_consumer()` 有一个真线程的生产者/消费者用例，每个被消费样本计一次 CHECK，而消费多少取决于调度。**这不是失败，别去"修"它；也别把固定数字写进文档。**
- 统计 check 数量不要用 `grep "checks="`（长行会截断，历史上把 64 记成过 64/60 混、把 test-frame-ring 数错），宜单独跑二进制。
- MinGW printf 不认 `%zu`，自测编译要 `-D__USE_MINGW_ANSI_STDIO=1`。
- `pthread_condattr_setclock()` 在 winpthreads 返回 EINVAL 且**静默**保持 CLOCK_REALTIME → 按 MONOTONIC 算的死线会立刻 ETIMEDOUT。代码已按实际返回值处理，别改。
- `tests/host-stubs/linux/videodev2.h` 桩头使得 `v4l2_mpp_encode.c` 能在 Windows 做语法检查；**`host-syntax-can-fail` 是这个设施的体检**（故意写错成员，断言编译器会拒绝）。一个不会变红的绿灯比没检查更糟 —— 它一旦失败，在修好桩之前不要相信 `host-syntax` / `board-flags`。

### 7.4 做「改动有没有效果」验证时的通用方法

本次 OSD 验证沉淀的方法（已存为 skill `board-change-verification`）：

- **必须有对照组**（两段录在同一个固定场景），否则无法把「改动的效果」和「环境本来就那样」分开。
- **像素判定用「强像素占比」（>40 灰阶）而不是 MAD**：overlay 是尖锐/局部/高对比，曝光漂移是平滑/全局，MAD 会把漂移平均进去（真机上 MAD 比值仅 1.68x，强像素比 7.8x）。
- **先想「它和干扰在形状上差在哪」，再统计那个形状。**
- 阈值设**绝对下限 + 相对下限**双条件；常量每次打印实测值，不做静默调参。
- **产物目录一直保留，下一轮开始时清**（trap 在成功时删产物 = 丢唯一证据）。
- 大段跳读别用 `dd bs=1`（逐字节 3 分钟+），用整行块 `bs=w skip=y count=rh`（1.35s）。
- 跑长脚本前**冻结文件**，中途编辑会撞上 bash 读到改了一半的文件 → 报莫名其妙的语法错误。

---

## 8. 文档地图（哪些可信）

| 文件 | 状态 | 说明 |
| --- | --- | --- |
| **本文件** | ✅ 最新（2026-09-29） | 交接首选 |
| `docs/status.md` | ⚠️ 到 2026-09-28 | 验收数据、根因分析最全，**但缺 OSD 上板章节** |
| `docs/roadmap.md` | ✅ 有效 | 阶段规划，尚未按完成度更新勾选 |
| `.workbuddy/memory/MEMORY.md` | ✅ 最新（2026-09-29 重写） | 长期项目知识，16KB |
| `.workbuddy/memory/2026-09-29.md` | ✅ | OSD 验证的完整流水 |
| `README.md` | ⚠️ | 「当前状态」原写着「尚未实现 OSD 和 MPU6050」；「后续路线」勾选框仍标 `[ ] 8 小时长稳正式验收`。本次已修正这两处措辞，其余节仍写于 OSD 之前 |
| `docs/handoff.md` | ❌ 明显过时 | §3 表把 MPU6050 标「未完成·硬件尚未接入」、长稳标「进行中」；§13「当前限制」仍写着「没有 RTSP / 没有 Frame RingBuffer / 没有开机服务」（全部早已实现）。**写于 RTSP 阶段，别拿它判断现状** |
| `docs/mpu6050-wiring.md` | ✅ 可用 | 接线、overlay 证伪 §3.5、bit-bang §4、上板步骤 §6 |

> 规则：`docs/status.md` 描述**实测到了什么**，`docs/handoff.md` 描述**当年交接时的架构**。两者冲突以 status.md 为准；**两者都过时时以本文件为准。**

---

## 9. 下一会话建议的第一步

1. 先确认板子还在推流（30 秒）：
   `adb shell "pidof v4l2_mpp_encode && netstat -tln | grep 8554"`
2. 决定走哪条路，二选一：
   - **做 P0（推荐）**：新建 `src/mpu6050_source.c`，实现 `sensor_source` 接口，把已有驱动接进去；先在 Windows 写单测（`make test` 框架里加 `test-mpu6050-source`），再走 VM 编译 → Windows 推板 → `scripts/verify-osd.sh`。
   - **做 P1/P2**：把 OSD 并入 `/userdata/gateway.env`，或用 `--sink rtsp` 做一次带 OSD 的真实推流验证。
3. 无论做哪个：**先跑一遍 `mingw32-make test`** 拿到绿基线，再动代码。
