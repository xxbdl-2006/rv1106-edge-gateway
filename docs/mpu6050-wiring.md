# MPU6050 接线与接入

这篇文档解决一个问题：**MPU6050 到底接在哪两根线上**。

> ## ⚠️ 2026-09-28 重要更正
>
> 本文早期版本给出的「接排针 **pin 24 / pin 25**（SDA=pin24/SCL=pin25）」是**错的**。
> 那是把 **SoC 引脚号**（i2c4m0 = GPIO2_A0/A1 = SoC pin 64/65）当成了**排针物理编号**。
>
> **实测依据**：板子自己的 `/usr/bin/luckfox-config` 里有官方排针图
> （`luckfox_pico_pro_max_pin_diagram_file()`），逐行数出来的物理引脚如下：

| 物理脚 | 左列（奇数） | 物理脚 | 右列（偶数） |
|---|---|---|---|
| 1 | GPIO1_B2 | 2 | VBUS |
| 3 | GPIO1_B3 | 4 | VSYS |
| 5 | GND | 6 | GND |
| 7 | GPIO1_C7 | 8 | 3V3_EN |
| 9 | GPIO1_C6 | **10** | **3V3_OUT（3.3V 供电）** |
| 11 | GPIO1_C5 | 12 | NC |
| 13 | GPIO1_C4 | **14** | **GPIO2_A7 = I2C3_M0_SDA** |
| 15 | GND | 16 | GND |
| 17 | GPIO1_D2 (I2C3_M1_SDA) | 18 | GPIO4_C1 |
| 19 | GPIO1_D3 (I2C3_M1_SCL) | 20 | GPIO4_C0 |
| 21 | GPIO2_B1 | 22 | RESET |
| 23 | GPIO1_C0 | **24** | **GPIO2_A6 = I2C3_M0_SCL** |
| 25 | GND | 26 | GND |
| 27 | GPIO1_C1 | 28 | GPIO2_A3 |
| 29 | GPIO1_C2 | 30 | GPIO2_A2 |
| 31 | GPIO1_C3 | 32 | GPIO2_A1 |
| 33 | GPIO2_B0 | 34 | GPIO2_A0 |
| 35 | GND | 36 | GND |
| 37 | GPIO1_D0 | 38 | GPIO2_A5 |
| 39 | GPIO1_D1 | 40 | GPIO2_A4 |

（USB 口朝上时，pin 1 在左上角。）

**关键结论**：

1. **排针上现成的 I2C 是 `i2c3m0`，不是 i2c4** —— 位置是 **pin 14 (SDA) / pin 24 (SCL)**。
2. **排针上的 GPIO2_A0 / GPIO2_A1（pin 34 / pin 32）在官方图里没有 I2C 标注** ——
   要用 must 改 pinmux，且 `i2c4m0` 会与摄像头抢 i2c4 控制器。
3. **3.3V 在排针 pin 10**（不是 36，36 是 GND）。
4. **本固件所有 I2C 控制器只有 i2c4 是 `okay`**，i2c0/1/2/3 在设备树里都是 `disabled`
   （实测 `/proc/device-tree/i2c@*`）。所以「启用 i2c3」同样要**改设备树 + 重刷固件**。

---

## 1. 为什么不能用 GPIO3_B7 / GPIO3_C0

板上正在跑的 i2c-4 用的是 `i2c4m2` 复用组，对应 SoC 的 pin 119 / 120，也就是
**GPIO3_B7 (SDA) / GPIO3_C0 (SCL)**：

```
$ adb shell "cat /sys/kernel/debug/pinctrl/pinctrl-rockchip-pinctrl/pinmux-pins | grep i2c4"
pin 119 (gpio3-23): ff470000.i2c function i2c4 group i2c4m2-xfer
pin 120 (gpio3-24): ff470000.i2c function i2c4 group i2c4m2-xfer
```

看上去"接这两个脚就行了"。**但这两个脚在 RV1106 上是 GPI，只能输入不能输出。**

Luckfox 官方工程师在论坛 ([forums.luckfox.com/viewtopic.php?t=1596](https://forums.luckfox.com/viewtopic.php?t=1596)) 的原话：

> **GPIO3_B0~B7、GPIO3_C0~C3 都是 GPI 引脚，仅支持输入不支持输出，仅用于捕获摄像头数据**

这是 MIPI CSI 的差分信号复用脚，只在**摄像头 FPC 排线**里出现，**没有引到 40pin 排针**。

I2C 是双向总线：**SCL 必须由主机输出时钟**，SDA 也要双向拉。GPI 脚做不到，
所以 MPU6050 接不到那里 —— 这不是"能不能配置"的问题，是硅片层面的限制。

那摄像头为什么能用？因为 **sc3336 模组本身就挂在这条 i2c 上**，I2C 线走的是摄像头排线，
挂在 i2c-4 下的从设备可以证明：

```
$ adb shell "ls /sys/bus/i2c/devices/"
4-0030   4-0031   i2c-4
```

`0x30` = sc3336（主摄）、`0x31` = mis5001。这些是摄像头自己的 I2C。

---

## 2. 推荐接法：用排针上的 i2c3m0（pin 14 / pin 24）

RV1106 各 I2C 控制器的复用组（实测 `pinmux-functions`）：

```
function: i2c0, groups = [ i2c0m0-xfer i2c0m1-xfer i2c0m2-xfer ]
function: i2c1, groups = [ i2c1m0-xfer i2c1m1-xfer ]
function: i2c2, groups = [ i2c2m0-xfer i2c2m1-xfer ]
function: i2c3, groups = [ i2c3m0-xfer i2c3m1-xfer i2c3m2-xfer ]
function: i2c4, groups = [ i2c4m0-xfer i2c4m1-xfer i2c4m2-xfer ]
```

`i2c3m0` 实测对应 **pin 70/71 = GPIO2_A6/A7**，在官方排针图里正好是 **物理 pin 24 / pin 14**，
而且是官方明确标注的 I2C 脚位。交叉验证 —— RV1106 数据手册的引脚复用表里：

```
107  UART0_RTS_M1/I2S0_SDO2_SDI2/.../I2C3_SCL_M0/.../GPIO2_A6_d
106  UART0_CTS_M1/I2S0_SDO1_SDI3/.../I2C3_SDA_M0/.../GPIO2_A7
```

GPIO 编号公式 `pin = bank*32 + group*8 + X` 代入：GPIO2_A6 = 2×32+6 = **70**、
GPIO2_A7 = 2×32+7 = **71** —— 与 pinmux 实测一致。

实测这四个脚都空闲、支持输入输出：

```
$ adb shell "cat .../pinmux-pins | grep -E 'pin (70|71) '"
pin 70 (gpio2-6): (MUX UNCLAIMED) (GPIO UNCLAIMED)
pin 71 (gpio2-7): (MUX UNCLAIMED) (GPIO UNCLAIMED)
```

### 接线表

| MPU6050 | Luckfox Pico Pro/Max | 说明 |
|---|---|---|
| **VCC** | **pin 10** `3V3_OUT` | **必须 3.3V**。MPU6050 的 VDD 耐压 3.4V，接 5V 会烧 |
| **GND** | pin 6 / 16 / 26 / 36（或 5/15/25/35） | 任一 GND 脚 |
| **SCL** | **pin 24** = GPIO2_A6 = `I2C3_M0_SCL` | 官方图标注的 I2C 时钟 |
| **SDA** | **pin 14** = GPIO2_A7 = `I2C3_M0_SDA` | 官方图标注的 I2C 数据 |
| **AD0** | GND 或悬空 | 接地 → 地址 `0x68`；接 3V3 → `0x69` |
| **INT** | 不接 | 当前用轮询读取，不需要中断 |
| **XDA / XCL** | 不接 | 辅助 I2C，用于挂磁力计，本项目不用 |

⚠️ **为什么不用 i2c4m0**：`i2c4m0`（GPIO2_A0/A1，排针 pin 34/32）与摄像头用的 `i2c4m2`
是**同一个控制器**的两个引脚出口。切到 m0 → 摄像头排线上的 I2C 断掉 → **摄像头挂**。
而 i2c3 是**独立控制器**，用了不影响摄像头。

---

## 2.5 ✅ 实测结果：接线正确，传感器已经活了（2026-09-21）

接好线后的实测（**用第 4 节的 bit-bang 工具，没改一行设备树**）：

```
$ adb shell "python3 /userdata/i2c-bitbang.py scan"
scanning 0x03..0x77 (SCL=gpio70 SDA=gpio71 delay=25us)
  found 0x68
done, 1 device(s) found
```

**总线扫描到 `0x68`** —— AD0 接 GND 的默认地址，接线、供电、上拉全部正确。

```
$ adb shell "python3 /userdata/i2c-bitbang.py id 0x68"
[1] WHO_AM_I (0x75) = 0x70
[2] PWR_MGMT_1=0x00 ACCEL_CFG=0x00 GYRO_CFG=0x00 (all should be 0x00)
[3] sampling 5 frames (accel + temp + gyro) ...
    #0  a=( -2648,   -72, 17144) g=(   552,  -594,  -259)  48.0 degC
    #1  a=( -2632,   -52, 17224) g=(   567,  -641,  -259)  48.0 degC
    #2  a=( -2624,  -148, 17136) g=(   558,  -619,  -239)  48.0 degC
[4] verdict: |az|=1.054 g  -> healthy, still sensor
```

### ⚠️ 重要发现：`WHO_AM_I = 0x70`，模块上不是原厂 MPU6050

期望 `0x68`，实测 `0x70`。这**不是接线问题**，是芯片型号问题。

`0x70` 是 **MPU6500** 的 ID。GY-521 模块换料很常见。

**决定性证据** —— 我还做了寄存器回环与掩码测试：

| 测试 | 真 MPU6050 应该 | 实测 | 说明 |
|---|---|---|---|
| `SMPLRT_DIV(0x19)` 写 `0xAB` 读回 | `AB` | **`AB`** | ✅ 真实可读写寄存器 |
| `SMPLRT_DIV(0x19)` 写 `0x5A` 读回 | `5A` | **`5A`** | ✅ 不是影子/缓存 |
| `CONFIG(0x1A)` 写 `0xFF` 读回 | 只保留低 3 位 = `07` | **`FF`** | ⚠️ **8 位全可写，位掩码不同** |
| `0x00-0x0F` 反复读 | 大部分 0 | 恒定 `CF D3 DF E1 94 19 FA E3 EB 00 07 F8 00 60 60 89` | ⚠️ 出厂自检/偏移值，非随机 |

**对项目的影响：零。**

| 项目 | MPU6050 | MPU6500 | 本模块 |
|---|---|---|---|
| 加速度输出 | `0x3B` | `0x3B` | ✅ 一致 |
| 温度输出 | `0x41` | `0x41` | ✅ 一致 |
| 陀螺仪输出 | `0x43` | `0x43` | ✅ 一致 |
| 数据格式 | 16 位补码 | 16 位补码 | ✅ 一致 |
| ±2g 灵敏度 | 16384 LSB/g | 16384 LSB/g | ✅ 一致 |
| ±250dps 灵敏度 | 131 LSB/dps | 131 LSB/dps | ✅ 一致 |
| 温度公式 | `raw/340 + 36.53` | 同 | ✅ 一致 |

**两条编码规则（务必遵守）**：

1. ❌ **不要**用 `if (who == 0x68)` 做存在性/型号判断。改成接受
   `{0x68, 0x70, 0x71, 0x72, 0x73, 0x98}` 这一族。
2. ❌ **不要**依赖 `CONFIG(0x1A)` 的位掩码行为（`0xFF` 会原样存回来）。

### 静止偏差校准常数（实测采集，100 样本）

```
accel mean (LSB): -2639.36     -67.48   17194.92
gyro  mean (LSB):   552.52    -611.66    -240.35
temp  mean      :  3882.30  -> 47.95 degC
accel |mean|    : 1.0618 g
```

生成可直接粘贴的常数：

```c
#define IMU_ACCEL_BIAS_X   (-2639f)
#define IMU_ACCEL_BIAS_Y   (-67f)
#define IMU_ACCEL_BIAS_Z   (811f)      /* 17195 - 16384 */
#define IMU_GYRO_BIAS_X    (553f)
#define IMU_GYRO_BIAS_Y    (-612f)
#define IMU_GYRO_BIAS_Z    (-240f)
#define IMU_ACCEL_LSB_PER_G   16384.0f
#define IMU_GYRO_LSB_PER_DPS  131.0f
#define IMU_TEMP_LSB_PER_DEG  340.0f
#define IMU_TEMP_OFFSET_C     36.53f
```

> **关于 `|mean| = 1.0618 g`**：这**不是**传感器误差，是**模块物理倾斜**了约 9.3°。
> `asin(2639/16384) = 9.3°`（X 轴）、`asin(67/16384) = 0.24°`（Y 轴）。
> MPU6050 模块插在垂直排针上必然带点角度。**注意 `IMU_ACCEL_BIAS_Z = 811`
> 里已经隐含了这个倾斜**（把 X/Y 的偏置当成"零偏"处理了）——
> 严谨做法是**先摆平再采集**，或改用姿态算法时**不做 Z 轴归零**。
> 当前常数适用于"模块保持这个安装角度"的场景。

---

## 3. i2c3 是 disabled，但**不需要**打开它（见第 4 节）

本固件实测只有 i2c4 是 `okay`：

```
$ adb shell "for d in /proc/device-tree/i2c@*; do echo \$(basename \$d) \$(cat \$d/status); done"
i2c@ff310000 status=disabled
i2c@ff320000 status=disabled
i2c@ff450000 status=disabled
i2c@ff460000 status=disabled
i2c@ff470000 status=okay      <- 这个就是 i2c4
```

（`ff470000` = i2c4，由 pinmux 输出的 `ff470000.i2c function i2c4` 反证。）

**如果将来要真正启用 i2c3**，改法如下（板级 dts：
`<SDK>/sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-luckfox-pico-pro-max.dts`）：

```dts
&i2c3 {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&i2c3m0_xfer>;   /* 固件里已经定义好了，直接引用 */
    clock-frequency = <100000>;
};
```

> **好消息**：本固件（`uboot-09/13/2026`）的 `pinctrl-0` **已经指向 `i2c3m0-xfer`**
> （实测 `i2c@ff460000/pinctrl-0 = 0x2f, 0x30`，其中 `0x30` 的 phandle 解析出来
> 就是 `pinctrl/i2c3/i2c3m0-xfer`，pins = `bank2 pin6 mux5` + `bank2 pin7 mux5` = GPIO2_A6/A7）。
> **所以只需要翻一个 `status`，pinctrl 一行都不用改。**

⚠️ 但改设备树要**重编内核 + 重刷固件**，会打断正在跑的测试。
**除非要上线，否则不要走这条路** —— 第 4 节的 bit-bang 方案不用改任何东西。

---

## 3.5 ❌ 运行时 device-tree overlay 在这块板子上是坏的（已实测证伪）

理论上最优雅的方案是运行时打 overlay 翻 `status`，不动 flash。
**实测证明这条路在这块板子上走不通。** 证据链：

**（1）内核配置是开的，configfs 也挂了**

```
CONFIG_OF_OVERLAY=y
none on /sys/kernel/config type configfs (rw,relatime)
```

**（2）overlay 目录可以建、dtbo 确实写进去了**

```
$ mkdir /sys/kernel/config/device-tree/overlays/i2c3
$ cat i2c3-enable.dtbo > .../i2c3/dtbo
$ wc -c < .../i2c3/dtbo
223                      <- 与源文件字节数一致，数据没丢
```

**（3）但它完全不生效 —— 决定性对照实验**

| 写入内容 | 正常内核应该 | 这块板子实际 |
|---|---|---|
| 合法 dtbo（223B）      | `status=applied`，dmesg 有 fragment 日志 | `rc=0`，dmesg **空白** |
| **`GARBAGE` 字符串**   | **返回 `-EINVAL`** | **`rc=0` 成功** ← 致命 |

```
$ echo GARBAGE > /sys/kernel/config/device-tree/overlays/badtest/dtbo
rc=0
$ cat /sys/kernel/config/device-tree/overlays/badtest/status
0
```

**写入非法数据也"成功"** ⇒ 这个 configfs 的 `dtbo` 属性是**哑写入路径**：
数据被存下来了，但内核**根本没走 overlay 解析与 notifier 流程**。

补充证据：`status` 属性的内容是**纯数字 `0`**，而正常内核会返回
`unapplied` / `applied` 之类的文本状态。dmesg 里 `grep -iE 'overlay|of_ov|fragment'`
**一条都没有**。

**结论**：`/dev/i2c-3` 永远不会通过 overlay 出现。别再在这条路上花时间。

---

## 4. ✅ 真正可用的方案：GPIO 位翻转（bit-bang）I2C

### 为什么可行

i2c3 既然是 `disabled`，它的 pinctrl 就没被 claim，pin 70/71 是**干净的普通 GPIO**：

```
$ cat /sys/kernel/debug/pinctrl/*/pinmux-pins | grep -E 'pin (70|71) '
pin 70 (gpio2-6): (MUX UNCLAIMED) (GPIO UNCLAIMED)
pin 71 (gpio2-7): (MUX UNCLAIMED) (GPIO UNCLAIMED)
```

（顺带：整个 GPIO2 bank 32 个脚**全部 UNCLAIMED**，非常自由。）

`gpio70` = SCL = 排针 **pin 24**，`gpio71` = SDA = 排针 **pin 14**。
用 `/sys/class/gpio/gpioNN/{direction,value}` 位翻转，**纯用户态**就能模拟 I2C 主机。

| 优点 | 说明 |
|---|---|
| 零设备树改动 | 不动 flash、不重刷 |
| 零内核改动 | 不需要交叉编译 |
| 零重启 | **完全不打断网关推流** |
| 可完全回滚 | 进程退出即结束，引脚回输入 |

### 代价：时钟慢

sysfs GPIO 单次读写约 10~50μs，做不到标准 100kHz。
**但 I2C 从机对时钟频率没有下限** —— MPU6050 在约 20kHz 下工作完全正常。
实测读 14 字节（一次完整六轴突发）耗时约十几毫秒，对 100Hz 的姿态采样**绰绰有余**。

### 工具

`tools/i2c-bitbang.py`（约 18KB，纯标准库，板端 python3 3.11.6 直接跑）：

```bash
adb push tools/i2c-bitbang.py /userdata/

adb shell "python3 /userdata/i2c-bitbang.py scan"            # 扫描总线
adb shell "python3 /userdata/i2c-bitbang.py id 0x68"        # 身份 + 唤醒 + 读六轴
adb shell "python3 /userdata/i2c-bitbang.py calib 0x68 100" # 静止偏差 -> 校准常数
adb shell "python3 /userdata/i2c-bitbang.py read 0x68 0x75" # 读单寄存器
adb shell "python3 /userdata/i2c-bitbang.py dump 0x68 0x00 0x50"
adb shell "python3 /userdata/i2c-bitbang.py --delay 10 scan" # 调快半周期(us)
```

### 三个实现要点（踩过的坑）

1. **开漏语义**：输出高 = `direction=in`（让外部上拉拉高），**不是**推高。
   I2C 是多主总线，任何设备都只能拉低不能推高，否则会打架。
2. **`sda_read()` 必须先切回 `in`** —— 若引脚还配成 `out`，读 `value` 读到的是
   **输出锁存器**的值而不是**线上实际电平**，永远得到自己刚写的值。
3. **每笔事务结束都要 `stop()` + 释放引脚**。异常路径若把 SCL 留在低电平，
   从机会一直等下一个时钟，整条总线卡死。工具里有 `bus_recover()`（发 9 个 SCL + STOP）兜底。

---

## 7. 代码结构

遵循和 RTSP 相同的分层规则 —— **不可测的部分尽量小，可测的部分尽量大**：

| 文件 | 职责 | 能否在 Windows 单测 |
|---|---|---|
| `src/mpu6050.h` | 接口、寄存器地址、量程常量 | — |
| `src/mpu6050.c` | **解码数学**：字节序、量纲换算、温度公式 | ✅ 68 项单测 |
| `src/mpu6050_i2c.c` | **唯一**碰 `/dev/i2c-N` 的文件 | ❌ 只能语法检查 |
| `tools/mpu6050-probe.c` | 板端扫描/打印工具 | ❌ |
| `tests/test_mpu6050.c` | 解码层单测 | ✅ |

**关键设计**：读传感器必须**一次 14 字节突发读取**（`0x3B` 起），
不能分 6 次读 —— 分开读会让六个轴来自六个不同的时刻，姿态解算直接失真。

```c
/* 一次性读 0x3B..0x48，14 字节 */
#define MPU6050_BURST_START MPU6050_REG_ACCEL_XOUT_H  /* 0x3B */
#define MPU6050_BURST_LEN   14
```

---

## 8. 单测抓到的真实 bug（记录）

写测试时抓到一个我自己犯的错，值得记下来：

`mpu6050_decode_burst()` 最初的参数检查顺序是

```c
if (burst == NULL || sample == NULL) return -1;
if (length < 14) return -1;
memset(sample, 0, sizeof(*sample));   /* 清零在检查之后 */
```

清零放在了提前返回**之后**，意味着"调用者忽略返回值"时拿到的恰恰是**没清零的旧数据** ——
而保护这种调用者正是清零的唯一目的。

```
FAIL tests/test_mpu6050.c:307: sample zeroed on failure, got 9999
```

修法：`sample == NULL` 单独判，其余检查全部**放在 memset 之后**。

**教训**：防御性代码的位置和它本身一样重要。放在错误的分支上，等于没写。
这种错误没有测试是发现不了的 —— 它不会崩溃，只会在生产环境里悄悄返回垃圾数据。

---

## 9. 这个改动会影响线程化测试吗

**不会。零交集。**

| | 线程化改动 | MPU6050 改动 |
|---|---|---|
| 文件 | `frame_ring.*`、`capture_thread.*`、`capture_signal.h` | `mpu6050.*`、`mpu6050_i2c.c`、`tools/mpu6050-probe.c` |
| 数据源 | V4L2 摄像头 NV12 | `/dev/i2c-N` 寄存器 |
| Makefile | `MEDIA_SRC`（未改动） | 独立的 `SENSOR_SRC` + `mpu6050-probe` 目标 |

`Makefile` 里 MPU6050 的源文件**刻意不放进 `MEDIA_SRC`** ——
既然视频流水线一行都没引用它们，放进去只会让板端二进制变大，不改变任何行为。

**回归验证**（`make test`，改动前后对比）：

| 测试 | 改动前 | 改动后 |
|---|---|---|
| test-packet-queue | 43 | 43 ✅ |
| test-rtp-rtsp | 86 | 86 ✅ |
| test-frame-ring | 60 | 64 ✅ |
| test-capture-thread | 13 | 13 ✅ |
| test-mpu6050 | — | **68 新增** |
| **合计** | **202** | **274** |

**唯一会打断线程化测试的是"改设备树+重刷固件"** —— 那是硬件操作，不是代码改动。
按第 5 节先只做软件验证，则**完全不打断**。

---

## 10. 引脚号速查：别再把 SoC 号当排针号

这次踩坑的根源是两套编号体系混用：

| 编号体系 | 例子 | 用途 |
|---|---|---|
| **SoC 引脚号** | `pin 64`、`pin 65`、`pin 119` | `/sys/kernel/debug/pinctrl/*`、设备树、内核日志 |
| **排针物理号** | `pin 14`、`pin 24`、`pin 10` | **你手里要插的孔** |

换算：`SoC 号 = bank*32 + group*8 + X`（GPIO2_A6 → 2×32+6 = 70）。
排查 pinmux 时用的是 SoC 号；**接线只看排针物理号**。

> 验证手法：板端 `/usr/bin/luckfox-config` 里自带官方排针图，直接
> `sed -n '/luckfox_pico_pro_max_pin_diagram_file/,/^}/p' /usr/bin/luckfox-config`
> 就能打印出来，比翻 wiki 快且不会过期。
