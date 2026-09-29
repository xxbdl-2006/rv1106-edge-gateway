# 项目对接文档（给下一个 Agent）

> 建立时间：2026-09-29 12:10（GMT+8），最后更新 2026-09-29 16:4x
> （P0+P1 完成 → P2 文档纠偏完成 → 冷启动验证完成，并修复了一个"每次上电都不出流"的缺陷）。
> 仓库：`github.com/xxbdl-2006/rv1106-edge-gateway`，分支 `main`，HEAD = `f724774` 之后（见 `git log`）。
> 本文件由实测整理而成，**与 `README.md` / `docs/handoff.md` 冲突时以本文件为准**（那两份文档已过时，见 §8）。

---

## 0. 三十秒速览

```text
一句话：一块 Luckfox Pico Pro/Max（RV1106G）上的边缘视频网关，
        摄像头 → V4L2 → Rockit MPI H.264 → RTSP over TCP，
        外加一条「传感器 → OSD 叠加」的支线；两条都已上板验证并进入生产配置。
        支线喂的是真实 MPU6500（软件 bit-bang I²C），不是 mock。

当前可对外播放：rtsp://172.32.0.93:8554/live/0  （1280x720 H.264，30fps，画面上带真实姿态）
生产配置      ：/userdata/gateway.env 已含 `--osd --osd-source mpu6050`（备份 gateway.env.bak）
当前状态      ：P0（真实 IMU 源）+ P1（并入生产配置）均已完成并实测；
                P2 文档纠偏已完成（README / handoff / status / roadmap）；
                冷启动验证已完成 —— **并因此发现一个每次上电都不出流的严重缺陷，已修复**（§3 P2 / §7.6）；
                仍缺：网络断线恢复测试、端到端延迟正式测量、架构图、演示视频。
```

维基式结论：**主线与支线都已闭环，且冷启动这条唯一没走过的路径也已走通并修好了其中的坑；
剩下的都不是功能，是文档和素材。**

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
| **开机自启** | 上电到出流**约 19 秒**（2026-09-29 修的冷启动竞态；旧记录「11 秒」已作废，见 §3 P2） |
| **8 小时长稳（线程化）** | **864,001 帧 / 08:00:00.033 / 30.00fps / drop=0 dup=0 / exit=0 / 失步 0**；零泄漏（fds 24、threads 6 全程不动，末 2 小时 RSS 增长 0 KB） |

### 2.2 MPU6050 —— 硬件链路与数据面**均已完成**

已完成且上板验证：

- 接线定死：**排针 pin 14 (SDA) / pin 24 (SCL)**，走 bit-bang GPIO（因为运行时 device-tree overlay 在这块板上是坏的，写入路径哑掉）。
- C 版驱动读通：`0x68: present`、`WHO_AM_I=0x70`（MPU6500 料，寄存器布局同 MPU6050）、**`0 io errors`**、校准后 `|a| = 0.996`。
- 校准常数已固化在 `src/mpu6050.h`（**别再手工抄一份**）。注意 Z 轴偏置是 `17194.92 - 16384 = 810.92` 而不是原始均值（见 §6 红线）。
- 板端黄金对照仍在：`/userdata/i2c-bitbang.py`（Python 版，同一块板同一颗芯片同一组引脚）。
  **Python 找得到而 C 版找不到 = 移植错了，不是硬件问题。**

**数据面接缝已接上（2026-09-29）**：新增 `src/mpu6050_source.{h,c}`（实现 `sensor_source` 接口）
与 `tools/imu_sample.c`（板端取数工具）。`--osd-source mpu6050` 已可选。**已上板验证并进生产配置**，见 §3 P0 与 §2.4。

### 2.3 Sensor 数据面 —— 已完成

`src/sensor_source.h` / `sensor_ring.c` / `sensor_attitude.c` / `mock_sensor.c` / `sensor_math.h`（无 libm）。
`test-sensor` 通过，**但注意检查项数字每次运行不同**（见 §7.3，不是 bug）。

### 2.4 OSD —— 完成并上板验证 PASS（2026-09-29）

- 层内容：`osd_font`（5x7 点阵，生成物）/ `osd_overlay`（1bpp 画布 + NV12 合成）/ `osd_format`（定点格式化）/ `osd_telemetry`（遥测行）/ `osd_feed`（50Hz 采样）/ `osd_annotate`（私有副本里合成）。
- **上板实测结果**：`--osd` 跑 300 帧 → `annotated=300 passed_through=0 composite_refused=0`，sensor `polls=300 samples=300`，**30.003 fps vs 无 OSD 30.000 fps（零可测开销）**；像素判定 overlay 区 21.0% 像素变化 >40 灰阶、对照带 0.0%；裁剪图肉眼可见面板。
- 一键验证脚本存在且已跑通：`scripts/verify-osd.sh`（Windows 入口 `scripts/verify-osd.cmd`）。

✅ **生产配置已开 OSD（2026-09-29 15:2x）**：`GATEWAY_ARGS="... --threads --ring-slots 4
--quiet --osd --osd-source mpu6050"` —— 真实 IMU 数据烧进直播流。

🔴 **生产命令行的唯一来源是仓库里的 `scripts/gateway.env`**，由 `scripts/install_autostart.ps1`
推成板上的 `/userdata/gateway.env`。**不要直接在板上改** —— 下一次重装会静默覆盖它
（这一点原来是错的：安装脚本把参数写死在自己内部，所以重装一次叠加层就没了，
而且没有任何地方记录生产命令行长什么样）。改配置 = 改 `scripts/gateway.env` 再重装。
而 `imu-soak.sh` / `start-2h-soak.sh` 现在也从同一个文件读参数，避免三处各写一份。

回滚：`adb shell "cp /userdata/gateway.env.bak /userdata/gateway.env"` 后 `S99gateway restart`
（那个备份是本轮之前的无 OSD 配置，仍有效）。

**已经在真实流上确认过**（不只是编码计数器）：从 Windows 侧 `ffmpeg -rtsp_transport tcp -i
rtsp://172.32.0.93:8554/live/0` 抽两帧 PNG，肉眼可见面板，且**两帧之间数值在变**
（`FRAME 2438→2451`、`TEMP 49.3→49.1`、`ROLL +0.2→+0.5`、`ACC 1.00→0.99`）——
是活的传感器数据，不是静态渲染。面板内容：`PITCH/ROLL`、`ACC/TEMP`、`STATUS FRAME/FPS`、
`IMU MPU6050 OK E=0`。

⚠️ 这一条值得坚持：**编码器打印 `annotated=N` 只证明它做了合成，不证明客户端收到了**
（`--sink rtsp` 没有客户端连接时，RTP 根本不发包）。生产配置上线后必须真拉一次流抽帧看。

---

## 3. 未完成（按推荐优先级）

### P0 — 真实 IMU 数据源：**✅ 已完成并上板验证**（2026-09-29）

**已完成（Windows 侧全绿，9 套单测）**

- `src/mpu6050_source.{h,c}`：实现 `sensor_source` 接口。分两半——`mpu6050_source_convert()`
  是纯算术（解码样本 → 校准 → 工程单位 → 姿态），**host 可测**；open/read/close 包在
  `#ifdef __linux__` 里（要调 `mpu6050_open/read`，只在板端存在），沿用驱动原有的分层。
- `src/v4l2_mpp_encode.c`：`--osd-source` 现在接受 `mock` 或 `mpu6050`；cleanup 改为
  **通过 `osd_source.close(context)` 关闭**，不再按类型分支（两种源各自管自己的收尾，
  IMU 还要释放 gpio70/71）。
- `tools/imu_sample.c`：板端工具，通过 **sensor_source 接口本身**（而不只是驱动）取数，
  每秒打印 samples/idle/errors 与 `|a|/pitch/roll/temp`，并据此退出。
  `make CROSS_COMPILE=...- imu-sample`。
- `tests/test_mpu6050_source.c`：28 项。钉死三件事：静止校准后 `|a| = 1.000`（实测 0.9999），
  未校准 `|a| = 1.0617`（模块倾斜，不是误差，必须保持），零向量不产生 attitude。
  **量程不符必须拒绝**（4g scale 配 2g 偏置 → 返回 -1），且失败时 `out` 被清零而非留旧值。
- `scripts/verify-imu.sh`：Windows 侧一键验证（推二进制 → 停网关 → 跑 imu-sample →
  查 gpio 残留 → 带 OSD 录 300 帧 → 还原网关，EXIT trap 保证还原）。

**关键设计决定（改之前先读）**

- **量程不可配置**：偏置是**原始计数**，只在 ±2g / ±250dps 下有意义。source 内部固定写
  `MPU6050_CAL_ACCEL_FSR` / `_GYRO_FSR`，`convert()` 还会用传进来的 scale 反查一次。
  静默地在 4g 下减 2g 的偏置会得到 0.98g ——落在所有容差内且是错的。
- **source 自带限流**（默认 **100 ms** / 10 Hz）：bit-bang 一次 14 字节突发实测 **约 28 ms**，
  全花在喂编码器的那个线程上，所以不能每帧都读总线（详见上面「根因」那一节）。
  窗口内的 read 返回 **0（空闲）而不是错误**——接口本来就有这个语义，只有这一层知道区别。
- **姿态是相对"标定时的安装角"**，不是相对世界水平：单点标定分不清倾斜和零偏，
  X/Y 的偏置里本来就含那 9.3°。要真正分开得做六面翻转，等安装固定了再说。

**上板结果：PASS，但帧率是真的塌了**（2026-09-29 13:44，`scripts/verify-imu.sh`）

```
WHO_AM_I=0x70 (MPU6500)      713 samples / 20 s, errors=0
静止 |a| = 0.990 g            pitch=-0.29 roll=-2.06 temp=47.82 C
annotated=300 composite_refused=0   sensor polls=300 samples=300 errors=0
gpio70/71 无残留 export
Average FPS : 21.359      dropped_busy=122      ← 基线 30.001
```

数据面、校准、姿态、引脚回收**全部正确**；唯一的问题是帧率。

**根因：bit-bang 总线一次突发约 28 ms，不是"几毫秒"**

实测来源：`imu-sample` 按 10 ms 周期轮询 20 s 只拿到 **713** 个样本（理论 2000），
即每次循环 ≈ 28 ms。一次 `mpu6050_read()` 就是**单次 14 字节突发**（没有多余事务），
所以这 28 ms 是总线本身的代价 —— 换算速率约 5 kHz。先前注释里写的"a few milliseconds"
是错的，已按实测改正。

**对照实验（`scripts/fps-osd-compare.sh`，同场次三种配置各 300 帧）**

| 配置 | fps | dropped_busy |
|---|---|---|
| none（无叠加） | 25.000 | 0 |
| mock | 25.000 | 0 |
| mpu6050 | 24.427 | 7 |

读法：none→mock 的差是**合成**的成本 = **0**；mock→mpu6050 的差才是 **I²C** 的成本。

**⚠️ 跨场次比 fps 是不可信的**（这次就踩了）：上面这场基线是 25 而不是 30，
因为脚本刚新起 `rkaiq_3A_server`、曝光还没收敛 → 采集只给 25 fps。
3A 收敛后采集才回到 30 fps。**必须同场次对照**，这就是这个脚本存在的理由。

两组数据其实自洽，模型如下：

```
单次 I²C 突发 ≈ 28 ms，编码 ≈ 12 ms，每帧预算 = 1000 / 采集帧率
  采集 30 fps（33.3 ms 预算）：28+12 = 40 > 33.3  → 崩到 21.4 fps   ✓ 与实测吻合
  采集 25 fps（40.0 ms 预算）：40 = 40            → 勉强，掉 7 帧    ✓ 与实测吻合
  改成 100 ms 间隔后 @30fps：每帧只摊 9.3 ms      → 21.3 < 33.3，有余量
```

**已做的修复**

- `MPU6050_SOURCE_DEFAULT_MIN_INTERVAL_US` 10000 → **100000**（10 Hz）。
  不是 100 Hz：瓶颈在总线不在传感器。
- 新增 `--osd-imu-interval-ms N`（0~10000，0 = 用 source 默认）。**这个 flag 必须存在**：
  正确值是板子上那条总线的属性，只能靠实测找，而找的过程需要能不改代码就扫一遍
  ——每次回 VM 重编译一趟太贵了。
- 日志行改成 `part at 100 Hz, bus read every N ms`：把"芯片自采样率"和"总线读取率"
  分开写，混在一起正是这个成本被读错的原因。

**复验完成：修复有效**（2026-09-29 14:07~14:10）

`bash scripts/verify-imu.sh`（间隔 100 ms）：

```
WHO_AM_I=0x70   samples=191 / 20 s = 9.6 Hz   idle=1810   errors=0
静止 |a| = 0.995 g          gpio70/71 无残留
OSD: part at 100 Hz, bus read every 100 ms
Average FPS : 25.000   dropped_busy=0   annotated=300 composite_refused=0
```

25 fps 是那场的**采集**上限（3A 刚起未收敛），关键数字是 `dropped_busy` **122 → 0**。

`SWEEP_MS=10,50,100 bash scripts/fps-osd-compare.sh 300`（**同一场次**，3A 已收敛，基线 30 fps）：

| 采样间隔 | fps | dropped_busy | captured |
|---|---|---|---|
| none（基线） | **30.001** | 0 | 301 |
| 10 ms | **21.409** | **122** | 426 |
| 50 ms | 26.612 | 38 | 340 |
| **100 ms（默认）** | **29.998** | **0** | 302 |

读法：10 ms 那档**逐字复现**了最初的故障（122 / 426 / 21.409，首次是 122 / 426 / 21.359）
—— 它是**对照组里的"已知坏"档**，证明这台测量装置确实能测出效应，
否则"100 ms 与基线持平"就分不清是改好了还是根本没测到。50 ms 仍不够（拐点在 50~100 之间）。

**结论：100 ms 这个默认值是实测选出来的。**`--osd-imu-interval-ms` 现在只是留给将来换板子的
旋钮，日常不需要显式写。

#### 30 分钟长稳（2026-09-29 14:25~15:05，`bash scripts/imu-soak.sh`）

跑的是 `/userdata/gateway.env` 那条命令行原样加 `--osd --osd-source mpu6050`，**`--sink rtsp`**
（⚠️ 那次**没有客户端连上来**，所以它证明的是"编码+合成在 RTSP 路径上稳"，不是"客户端收到了"；
真的拉流验证是 15:2x 补的，见 §2.4）：

```
Captured 54000 frames        Average FPS : 30.000
Ring      pushed=54001  dropped_busy=0  dropped_oldest=0  peak_depth=3
OSD       annotated=54000  passed_through=0  composite_refused=0
Sensor    polls=53672  samples=15057  no_sample=38614  errors=1
gpio70/71 无残留
```

| 指标 | 起始 | 结束 | 判读 |
|---|---|---|---|
| fds | 28 | 28（30 次采样**全部** 28） | 无泄漏。28 = 基线 24 + 2 引脚×2，与 `gpio_sysfs.c` 预开 fd 的设计吻合 |
| threads | 6 | 6 | 无线程泄漏 |
| RSS | 17444 KB | 17960 KB | **前 11 min +440 KB，后 11 min +68 KB，末尾连停 3 个采样点** → 分配器热身，不是泄漏 |
| CPU | 32% | 32% | 基线 14~17% → **约翻倍**，见下 |
| 温度 | 55.9°C | 55.9°C（稳态） | 基线 52.5°C，+3.4°C |

- ✅ **满帧**：整场 `30.000 fps`、`dropped_busy=0` —— 帧率问题到此闭环。
- ⚠️ **`errors=1`**：15057 次采样里 1 次总线错误（≈7e-5）。软件 bit-bang I²C 的固有特性，
  不是回归。程序行为正确：标记 stale、沿用上一姿态，不崩不泄漏（host 单测覆盖了
  "failing source 与 quiet source 可区分"）。**不建议为它加重试** —— 重试路径在板上
  无法被确定性地触发，而一段没被验证过的错误恢复代码本身就是负债。
- ⚠️ **CPU 32%（基线 14~17%）**：单次突发 28 ms × 10 次/s ≈ 一个核的 28%。这是选
  100 ms 而非 10 ms 的另一条理由（10 ms 会把 CPU 打满，正是当初掉到 21 fps 的原因）。
  要省 CPU 就把间隔调到 200 ms，减半，显示上无差别。

### P1 — OSD 并入默认生产配置：**✅ 已完成（2026-09-29 15:2x）**

决策是**直接上真源、并且不经过 mock**（mock 是假数据，真源已经实测过 54000 帧）。

- 板端 `/userdata/gateway.env` 已加 `--osd --osd-source mpu6050`，备份在 `gateway.env.bak`。
- 走**真实开机路径**（`S99gateway stop` → `start`）验证：日志 `OSD: IMU attached,
  WHO_AM_I=0x70`、`OSD: on, source=mpu6050 mode=wave`，8554 在听，从 Windows 拉真实流
  抽帧确认数值在动态更新（见 2.4 末尾）。
- 前置 fail-soft（`f724774`）已编译进板端二进制；**它在正常路径下不改变任何行为**
  （回归实测 `FRAMES=300`：`IMU attached / 30.002 fps / annotated=300 / errors=0 / gpio 干净`）。
- 短版验证用的是 `imu-soak.sh`（`FRAMES=300`），因为它**不依赖 `imu-sample`** ——
  `make clean` 之后那个采样工具常被漏编，`verify-imu.sh` 会因此直接 die。

**冷启动：已验证，并发现另一半是坏的（2026-09-29）**。补做「上电 → 3A 首次收敛」时发现
**每次上电都不出流** —— 接管逻辑与厂商 `RkLunch.sh` 后台启动 rkipc 之间的竞态。
根因、修复与复验见 **§7.6**，完整工程记录见 `docs/status.md` §2.8。
修复后冷启动复验 PASS：上电到出流约 19 秒，`IMU attached`，真拉流抽帧面板数值正常。
**顺带作废「开机 11 秒出流」这个沿用已久的数字**（它测的是不可靠的路径）。

### P2 — 工程化收尾

| 项 | 状态 |
| --- | --- |
| `README.md` / `docs/handoff.md` 内容过时 | ✅ **已修（2026-09-29）**：README 全文（当前状态/传感器行/OSD 参数/仓库结构/测试套件/后续路线勾选）；`docs/handoff.md` §2/§3/§5/§7/§11/§13/§14/§15/§16/§19 并加文首状态横幅 |
| `docs/roadmap.md` 未按完成度更新勾选 | ✅ **已修**：文首加完成度核对表，各阶段标题加 ✅/⚠️，传感器与 OSD 两处写上与原计划的偏差 |
| `docs/status.md` 更新到 2026-09-29 | ✅ **已修**：新增 §1.3（OSD 上板）、§1.4（真实 IMU 进生产配置）、§2.6（bit-bang 28 ms 根因）、§2.7（配置多处=副本）；补全 §3 架构与 §4 套件表、§5 板端操作 |
| 网络断开/恢复的专项测试 | roadmap 列了，**没跑过**（仍待做） |
| 架构图、演示视频 | roadmap「最终交付物」列出，**未产出**（仍待做） |
| 延迟的正式测量 | 只有 ffplay 约 0.75s 的粗测，无文档化方法（仍待做） |
| 冷启动（上电 → 3A 首次收敛）完整验证 | ✅ **已做（含物理拔插断电重启），并发现+修复严重缺陷**（每次上电都不出流，见 §7.6 / status.md §2.8） |
| restart 端到端回归 | ✅ **PASS**：接管 4 秒、走"没人会来"分支、零白等、拉流成功（status.md §2.8 复验三） |
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

# 真实 IMU 源一键验证（Windows；二进制需先在 VM 编好）
bash scripts/verify-imu.sh

# 部署生产配置（开机三件套 S99gateway / gateway-supervise.sh / gateway.env）
powershell -File scripts/install_autostart.ps1          # 只装，不碰正在跑的进程
powershell -File scripts/install_autostart.ps1 -Start   # 装完立刻起
adb shell "/etc/init.d/S99gateway restart"              # 让新的 GATEWAY_ARGS 生效

# 🔴 生产命令行的唯一来源 = scripts/gateway.env（仓库里）。
#    改配置改这里再重装，不要在板上改 —— 板上那份是它的副本。

# 网关状态（判活用进程名 v4l2_mpp_encode，不是 gateway）
adb shell "pidof v4l2_mpp_encode; pidof gateway-supervise.sh; pidof rkaiq_3A_server"

# 从另一端真拉流抽帧（判「叠加层真的到了客户端」的唯一方法）
ffmpeg -rtsp_transport tcp -i rtsp://172.32.0.93:8554/live/0 -t 8 -frames:v 2 out-%02d.png
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

### 7.5 同一份配置写在多处 → 重装会静默回退

2026-09-29 的实例：生产的 `GATEWAY_ARGS` 曾同时存在于四个地方 —— `install_autostart.ps1`
（在内部拼字符串生成）、`start-2h-soak.sh`、`imu-soak.sh`、以及板上的 `/userdata/gateway.env`。
把叠加层加进板上那份之后，**任何一次重装或长稳都会把它抹掉**，而且全程没有任何报错：
现象是「OSD 某天开始不见了」，排查方向会指向 OSD 代码，而原因在一个安装脚本里。

现在唯一来源是 `scripts/gateway.env`：安装脚本推它，两个长稳脚本 source 它。
安装脚本里唯一还自己算的是 `--quiet` 的能力检测（老二进制给了这个 flag 会拒绝启动），
且明确只改 `GATEWAY_ARGS=` 那一行，不动解释性注释。

**判据**：如果一件事「改了 A 之后 B 会把它改回去」，那 A 不是配置，是**副本**。

### 7.6 冷启动的接管竞态：vendor chain 是后台跑的（2026-09-29，最严重的一个）

**症状**：`adb reboot` 后板子**不出流**。`S99gateway status` 全是 not running，
只剩 `rkipc` 在跑，8554 没在听。

**根因**：`RkLunch.sh` 的**最后一行是 `post_chk &`** —— 后台执行。而 rkipc 是
`post_chk` 的**最后一步**，之前还要等 /userdata 挂载、insmod、`network_init &`、
多次 `lsmod|grep`、拷 ini、`rk_mpi_ao_test`（放测试 WAV）、`luckfox-config`。

```text
开机 +6.0s   S99gateway 跑：pidof rkipc 为空 -> "no rkipc, nothing is coming" -> 继续
开机 +6.1s   stop_by_name rkipc -> pids before: []   （杀了个空列表）
开机 +7.5s   rkipc 出现，抢走 /dev/video11
开机 +7.5s   "rkipc survived as [N]" -> return 1 -> 永久放弃
```

| 场景 | S99gateway 看到的 | 真实情况 | 旧代码结果 |
| --- | --- | --- | --- |
| **冷启动** | 没有 rkipc | 1.5 秒后就来 | ❌ 放弃 → **开机无流** |
| **restart** | 没有 rkipc | 永远不会有 | ✅ 正确 |

**"现在没有 X" ≠ "X 不会来"。** 旧代码把这两件事合并了，凭的是"重启时 rkipc
不会来"—— 而那句话只在重启时成立。这句代码本身是上一轮修「restart 白等 20 秒」
时加的，**那次修复没错，只是缺一个"这次是开机还是重启"的信息**。

**踩过的弯**：想用「`RkLunch.sh` 进程还在不在」判断厂商链是否还在跑 —— **不可行**。
该进程长期残留（实测 uptime 224s 仍在：ppid=1、状态 S、`wchan=do_wait`、
子进程列表为空）。**它的"不在"有意义，"在"没有意义。**

**修法**（`scripts/S99gateway`）：用 `/proc/uptime` 区分开机与重启 —— **这就是当初缺的
那条信息，而且是免费的**。新增 `wait_for_late_rkipc()`，rkipc 缺席时有**三条退出路径**：

1. rkipc 出现了（冷启动的正常结局）
2. 厂商链进程消失（链跑完了没起 rkipc，确实没得等）
3. **uptime 超过 `VENDOR_CHAIN_GRACE_SECONDS`（30s）** → 太晚不可能是开机 → 一定是重启

第 3 条保证**不把上一轮修的「白等」改回来**。另把 `sleep 2` 盲等改成有界轮询
（等 rkipc 进程与 `/dev/video11` 都真正空闲），并新增"rkipc 退出后若还有**别的**
进程占着节点就大声失败"的断言。

**验证**（三次，逐次加强）：

1. `adb reboot` 冷启动 PASS：`boot+5 start requested` → `boot+9 rkipc appeared late (uptime 9s)`
   → `boot+19 gateway up`。
2. **物理拔插断电重启 PASS**（最严格的一次：板子由 USB 供电，连 IMU 与摄像头一起上电
   的那段也真的走过了）——`uptime` 从 544s 归零到 37.6s、`/tmp` 已清空，boot log 同样
   `rkipc appeared late (uptime 9s)`；8554 在听、真拉流抽帧面板数值正常。
3. **restart 端到端回归 PASS**：接管 4 秒、`no rkipc, nothing is initialising ... proceeding`
   同一秒内返回（**没把上一轮修的「白等」改回来**）、`/dev/video11 is free`、拉流成功。

> 🔴 **「开机 11 秒出流」已作废**，正确值是**上电到出流约 19~20 秒**（接管 14s，含等厂商链 ~4s）。
> 另外 `install_autostart.ps1` 里"禁用 rkipc init 脚本"一步在这块板上是**空操作**
> （日志：`no rkipc init script found`）—— rkipc 不是 init.d 起的。
>
> **教训**：交接文档里凡是写着「**只验证了一半**」的地方，直接去把那另一半跑掉 ——
> 本次就是这么发现一个"每次上电都坏"的缺陷的。
> **判活不必依赖 adb**：RTSP 走 TCP/IP，`ping` + `/dev/tcp/<ip>/8554` 就能判断。

---

## 8. 文档地图（哪些可信）

| 文件 | 状态 | 说明 |
| --- | --- | --- |
| **本文件** | ✅ 最新（2026-09-29） | 交接首选 |
| `docs/status.md` | ✅ **到 2026-09-29** | 验收数据、根因分析最全；已补 OSD 上板（§1.3）、真实 IMU（§1.4）、bit-bang 28 ms 根因（§2.6）、配置多处即副本（§2.7）、**冷启动竞态（§2.8，最严重）** |
| `docs/roadmap.md` | ✅ 已按完成度更新 | 文首完成度核对表 + 各阶段 ✅/⚠️ 标记 |
| `.workbuddy/memory/MEMORY.md` | ✅ 最新（2026-09-29 重写） | 长期项目知识，16KB |
| `.workbuddy/memory/2026-09-29.md` | ✅ | OSD 验证的完整流水 |
| `README.md` | ✅ **本轮已全量修正** | 当前状态、传感器行、OSD 参数表、仓库结构树、测试套件（十套 1205 项）、后续路线勾选全部对齐现状 |
| `docs/handoff.md` | ⚠️ 历史快照（**已加文首横幅并修正主要章节**） | 仍写于 RTSP 阶段，但 §2/§3/§5/§7/§11/§13/§14/§15/§16/§19 已重写为现状；**判断现状仍以本文件为准**，该文件的其余章节作为设计意图的历史记录保留 |
| `docs/mpu6050-wiring.md` | ✅ 可用 | 接线、overlay 证伪 §3.5、bit-bang §4、上板步骤 §6 |

> 规则：`docs/status.md` 描述**实测到了什么**，`docs/handoff.md` 描述**当年交接时的架构**。两者冲突以 status.md 为准；**两者都过时时以本文件为准。**

---

## 9. 下一会话建议的第一步

1. 先确认板子还在推流（30 秒）：
   `adb shell "pidof v4l2_mpp_encode && netstat -tln | grep 8554"`
   顺手看一眼叠加层是活的还是降级了：
   `adb shell "grep -E 'IMU attached|IMU unavailable' /userdata/gateway.log | tail -2"`
2. **P0/P1 已全部完成**（真实 IMU 进生产配置，见 §2.4 与 §3 P1）。
   **P2 的文档纠偏也已完成**（README / handoff / status / roadmap 四份已对齐现状，见 §3 P2 表）。
   **冷启动验证已完成（含物理断电重启）并顺带修掉一个严重缺陷**；restart 回归也 PASS（§7.6）。
   剩余 P2：网络断线恢复测试、端到端延迟正式测量、架构图、演示视频 —— **都是功能之外的活**。
3. 改代码前的基线：`mingw32-make test`（现为 **10 个测试二进制套件全绿**，
   合计约 1205 项 0 失败；外加 `board-flags` 与 `host-syntax-can-fail` 两项标志检查。
   `test-sensor` 的项数每次运行浮动，属正常），再动代码。
4. 想复跑一次带 IMU 的验证：`bash scripts/imu-soak.sh`（`FRAMES=300` 快速版，
   **不依赖 `imu-sample`**）；要跑 `verify-imu.sh` 得先让用户在 VM 补编 `imu-sample`。
5. 回滚生产配置：`cp /userdata/gateway.env.bak /userdata/gateway.env` + `S99gateway restart`。
