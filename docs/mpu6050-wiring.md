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

**这些常数已经在 `src/mpu6050.h` 里（`MPU6050_ACCEL_BIAS_*` / `MPU6050_GYRO_BIAS_*`），
别再手工粘一份。** 下面只解释它们是什么，代码以头文件为准。

#### ⚠️ 加速度计和陀螺仪的偏置**语义不同**

这是最容易写错、而且写错了测试还全过的地方：

| 轴 | 偏置值 | 为什么 |
|---|---|---|
| gyro X/Y/Z | `552.52 / -611.66 / -240.35` | 陀螺仪测**角速度**，静止时真值是 0，所以**整个均值都是零偏**，直接减 |
| accel Z | **`810.92`**（不是 `17194.92`！） | 加速度计测**比力**，静止时垂直轴读的是**重力本身**（16384 ≈ 1g）。只有 `17194.92 - 16384 = 810.92` 才是真正的零偏 |
| accel X/Y | `-2639.36 / -67.48` | 见下方「倾斜」说明 |

**如果照抄 `17194.92` 当 Z 偏置会怎样**：重力被一起减掉，一个摆平的传感器
三轴全读 `0.00 g`，姿态解算永远不会收敛 —— 而且**不会报任何错**。
实测验证过这个 bug：`tools/mpu6050-probe` 会打印 `|a| calibrated = 0.000 g`，
单测 `test_calibration_preserves_gravity` 和 `test_calibration_magnitude_matches_libm`
会直接失败（共 7 条断言）。**这两个测试就是为防这一刀而写的。**

#### 关于 X/Y：那不是零偏，是模块在倾斜

`|mean| = 1.0618 g` **不是**传感器误差，是**模块物理倾斜**了约 9.3°：
`asin(2639/16384) = 9.3°`（X 轴）、`asin(67/16384) = 0.24°`（Y 轴）。
MPU6050 模块插在垂直排针上必然带点角度。

**当前常数把这份倾斜当作"零偏"吸收了**，所以只在「模块保持这个安装角度」时有效
（对现在这个项目来说够用，而且比运行时自动标定更可靠 —— 自动标定无法区分
「静止且水平」和「静止但倾斜」）。严谨做法是六面翻滚分离出真正的电气零偏，
等安装方式定下来再做。

#### 完整性与自检

`mpu6050_apply_calibration()` 会拒绝在**非 ±2g / 250dps** 下应用这批常数 ——
偏置是**原始计数**，换了量程还照减就是减了正确数字的错误单位，而且不报错。

`mpu6050_calibrated.accel_magnitude_g` 顺带算好了合加速度（牛顿迭代开方，
不依赖 libm），静止时必须是 `1.000`：

- `0.000` → 偏置把重力吃掉了（就是上面那个 bug）
- `1.062` → 偏置根本没应用
- `1.000` → 正确

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

## 5. 代码结构（2026-09-21 重构为 bit-bang 传输）

遵循和 RTSP 相同的分层规则 —— **不可测的部分尽量小，可测的部分尽量大**：

| 文件 | 职责 | 能否在 Windows 单测 |
|---|---|---|
| `src/mpu6050.h` | 接口、寄存器地址、量程常量、引脚默认值 | — |
| `src/mpu6050.c` | **解码数学**：字节序、量纲换算、温度公式 | ✅ 68 项单测 |
| `src/mpu6050_gpio.h` | **引脚抽象接口**（开漏语义的 5 个函数指针） | — |
| `src/i2c_bitbang.c` | **I2C 时序**：START/STOP、位收发、ACK、总线自愈 | ✅ **50 项单测** |
| `src/gpio_sysfs.c` | **唯一**碰 `/sys/class/gpio` 的文件 | ❌ 只能语法检查 |
| `src/mpu6050_i2c.c` | MPU6050 寄存器策略（唤醒、量程、DLPF） | ❌ 只能语法检查 |
| `tools/mpu6050-probe.c` | 板端扫描/识别/打印工具 | ❌ |
| `tests/test_i2c_bitbang.c` | 时序单测（假 GPIO + 模拟从机） | ✅ |
| `tests/test_mpu6050.c` | 解码层单测 | ✅ |

### 为什么时序能单测

关键是把**引脚操作**抽成 `struct i2c_gpio_ops`（`line_low` / `line_release` /
`line_read` / `delay` / `release_all`），`i2c_bitbang.c` 只调这 5 个函数，
**不碰文件、不 sleep**。单测塞进一个"录制器"把所有边沿记下来，
再用一个模拟从机在 SDA 上应答，于是可以断言**真实波形**：

```c
uint8_t written[8];
size_t got = decode_written_bytes(&rec, written, sizeof(written));
CHECK(written[0] == 0xD0);   /* 0x68 << 1 | write */
CHECK(written[1] == 0x3B);   /* register */
CHECK(written[2] == 0xD1);   /* 0x68 << 1 | read  <- 读/写位最容易搞错 */
```

这比"函数返回 0"强得多：一个时序错的 bit-bang 主机**从调用方看完全正常**，
只在真硅片上一会儿好一会儿坏。

### 关键设计

1. **读传感器必须一次 14 字节突发读取**（`0x3B` 起）。分 6 次读会让六个轴来自
   六个不同时刻，姿态解算直接失真。
2. **寄存器指针用 repeated START 而非 STOP**：`S, addr+W, reg, Sr, addr+R, data, P`。
   MPU6050 两种都吃，但数据手册规定的是重复起始位，有些器件看到 STOP 会重置指针。
3. **开漏语义**：`line_release` 实现为 `direction=in`（交给上拉），**不是写 1**。
4. **异常路径必须释放总线**：`read_regs`/`write_regs` 用 `goto out` 统一收尾，
   失败时调 `i2c_bb_release()`。SCL 卡在低电平会让整条总线永久死掉。
5. **`i2c_bb_bus_recover()`**：发 9 个时钟 + STOP，救回被从机拽死的总线
   （`mpu6050_open` 和扫描前各调一次）。

```c
/* 一次性读 0x3B..0x48，14 字节 */
#define MPU6050_BURST_START MPU6050_REG_ACCEL_XOUT_H  /* 0x3B */
#define MPU6050_BURST_LEN   14
```

### 引脚默认值

```c
#define MPU6050_SCL_GPIO_DEFAULT 70   /* GPIO2_A6 = 排针 pin 24 */
#define MPU6050_SDA_GPIO_DEFAULT 71   /* GPIO2_A7 = 排针 pin 14 */
```

可用 `--scl/--sda` 覆盖，换板子不用改代码。

---

## 6. 板端编译与上板验证

### 一键脚本（推荐）

**Windows 侧没有 SDK，必须在 VM 里跑**（代码在 `/mnt/hgfs/luckfox_share/rv1103`）：

```bash
cd /mnt/hgfs/luckfox_share/rv1103
./scripts/verify-mpu6050.sh
```

脚本按顺序做五件事，**每步都有自己的前置检查**，失败时会说是哪一步：

| 步骤 | 检查什么 | 失败的含义 |
|---|---|---|
| 1. 找工具链 | `arm-rockchip830-...-gcc` 是否可用 | PATH 没配好，或 SDK 路径不对 |
| 2. 交叉编译 | 编译成功 + **ELF `e_machine=0x2800`** | 编出的是 host 二进制（推上去会报"找不到"） |
| 3. 查板子 | adb 在线 + **pin 70/71 `MUX UNCLAIMED`** | 引脚被驱动占了，用户态无法救 |
| 4. 推送运行 | **push 后字节数核对** + 跑 `--dump` | push 被中断会留下 0 字节文件 |
| 5. 判定结果 | 四个数字自动检查 | 见下表 |

**为什么要写成脚本而不是文档里的命令清单**：有五步必须按序做，而几种最典型的失败
（字节序错、二进制过期、工具链不在 PATH）**从外面看都是「传感器坏了」**。
每步自查能直接指出是哪一环。

如果工具链不在默认位置：

```bash
CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- ./scripts/verify-mpu6050.sh
SDK_ROOT=/你的/luckfox-pico ./scripts/verify-mpu6050.sh
```

### 手工步骤（脚本失败时排查用）

```bash
cd /mnt/hgfs/luckfox_share/rv1103
make mpu6050-probe CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
```

**运行前提**：以 root 跑（要写 `/sys/class/gpio/export`），且那两个引脚
`MUX UNCLAIMED`。`adb shell` 默认就是 root。

```bash
adb push mpu6050-probe /userdata/
adb shell "/userdata/mpu6050-probe"                # 扫描
adb shell "/userdata/mpu6050-probe --dump"         # 扫描 + 读取
adb shell "/userdata/mpu6050-probe --delay 10 --dump --count 20"
```

**推上去先核字节数**（`adb push` 被打断会留 0 字节文件，而它照样出现在 `ls` 里）：

```bash
wc -c < mpu6050-probe                              # 本地
adb shell "wc -c < /userdata/mpu6050-probe"        # 板端，应一致
```

**最有力的交叉验证**：板子上还留着**已经跑通过的 Python 版**
`/userdata/i2c-bitbang.py`。C 版的结果必须和它一致 ——
**如果 Python 找得到而 C 版找不到，那就是移植错了，不是硬件问题**：

```bash
adb shell "python3 /userdata/i2c-bitbang.py scan"
```

**实测输出**（2026-09-28，修复 sysfs 写序后，**这是本项目 C 版第一次在真硅片上读通**）：

```
bus: SCL=gpio70  SDA=gpio71  half period=25 us
scanning 0x08..0x77 for ACKs
  0x68: present   <- MPU6050-family, AD0 = low
  112 transaction(s), 0 nack(s), 0 io error(s)

WHO_AM_I = 0x70 (MPU6500)
           not the 0x68 of a stock MPU6050, but the
           output registers are layout-compatible

 #  accel[g] raw               | accel[g] calibrated         | gyro[dps] calibrated     | temp[C]
 0   -0.148  +0.001  +1.045  |  +0.013  +0.005  +0.995  |    +0.06    -0.04    +0.03 |  47.69
 1   -0.147  -0.000  +1.042  |  +0.014  +0.004  +0.993  |    +0.06    -0.01    -0.00 |  47.69
 2   -0.149  +0.002  +1.051  |  +0.012  +0.006  +1.001  |    +0.06    +0.01    +0.07 |  47.68
 ...
|a| raw        = 1.056 g (bench part reads ~1.062: it leans)
|a| calibrated = 0.996 g (expect ~1.000 at rest)
gyro[dps] means: +0.057 +0.011 +0.006
```

**与 Python 黄金对照的一致性**（同板、同引脚、同片子，差异都在帧间噪声量级）：

| 量 | Python 版 | C 版 | 差异 |
|---|---|---|---|
| accel Z | 17120 LSB | 17121 LSB | **+1** |
| accel X | −2352 LSB | −2425 LSB | −73（静止时本身就有 ±100 LSB 抖动） |
| temp | 47.8 °C | 47.69 °C | −0.11 |
| `\|a\|` | 1.045 g | 1.045 g（raw） | 一致 |

→ **移植保真，不是"凑巧能读"。**

**四个判据**（`verify-mpu6050.sh` 会自动检查这四条并给出 PASS/FAIL）：

| 看什么 | 期望 | 说明 |
|---|---|---|
| `0x68: present` | 出现 | 通信通了（`0x70` 也收，见 §2.5 兼容族） |
| `nack(s) = 0` | 0 | 时序干净，没有半途丢应答 |
| `io error(s) = 0` | **0** | 引脚写得动。**非 0 且 `nack=0` 高度指向 sysfs 写序问题**（见 §7 第六个 bug） |
| `\|a\| raw` | ≈ 1.05~1.06 | 未校准值，**应该是 1.06 而不是 1.00**（因为模块倾斜了 9.3°） |
| `\|a\| calibrated` | ≈ 1.000 | 校准常数生效。**这是最关键的判据** |

> 实测 `1.056` / `0.996` 略低于 `1.062` / `1.000`，是**本次安装角度与校准时略有差异**
> 所致（校准常数把当时那个倾斜角当零偏吸收了，见 §2.5 的警告）。两项都落在容差内。

`|a| raw` 和 `|a| calibrated` **成对**出现是刻意的：
- 两个都是 1.062 → 校准静默失效
- calibrated 是 0.000 → 偏置把重力吃掉了（见 §2 的警告）
- raw 就是 1.000 → 这颗不是常数所描述的那颗传感器

> 脚本的判定逻辑用**人造输出**验证过四种情形（正常 / 重力被抹掉 / 校准未生效 / 有 NACK），
> 四种都判对了，且正常情形不误报。

**诊断线索**：
- `io error(s)` 在涨 → 引脚写不动：**先查 §7 第六个 bug 的 sysfs 写序**，再查权限/驱动占用
- `nack(s)` 在涨而 `io error(s) = 0` → 总线能动但没有器件应答：接线/地址/供电
- `|a| raw` 明显偏（比如 8.0）→ 量程配置错；但单测已覆盖换算，更可能是硬件
- `calibration refused` → 量程不是 ±2g/250dps，见 §2 说明

**探针的自清理已验证**：运行前后 `gpio70/71` 均无残留 export，
引脚回到 `MUX UNCLAIMED / GPIO UNCLAIMED`（完全释放，不会卡住总线）。
若引脚在运行前就已被别人导出，探针**刻意不去 unexport 它**（见 `pin_export()` 注释）。

> ⚠️ **`tools/mpu6050-probe.c` 原来有个真 bug**：算合加速度时只做了三轴平方**相加**
> 就贴上 `|a|` 标签（漏了开方），会把 1.05 报成 1.10 而看着挺合理。
> 已改为牛顿迭代开方（不开 `-lm`，rootfs 只有 libc 也能链）。
> 这类错误没单测是发现不了的 —— 它不崩溃，只是安静地给出错数。

---

## 7. 单测抓到的真实 bug（记录）

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

### 第四个：加速度计偏置差点把重力一起减掉（本轮，最危险的一个）

这个值得单独写，因为它是**概念错误**而不是打错字，而且**其余所有测试都会通过**。

给加速度计写标定时，我第一版把 Z 偏置写成了原始均值 `17194.92`。
理由看着很自然："静止时读到的就是零偏，减掉它"。但那是陀螺仪的逻辑 ——
陀螺仪测**角速度**，静止时真值确实是 0，所以均值全是零偏。

加速度计测的是**比力**，静止时垂直轴读到的**就是重力本身**（16384 ≈ 1g）。
照抄 `17194.92` 的后果：重力被当成零偏减掉，一个摆平的传感器三轴全读 `0.00 g`，
姿态解算永远不会收敛 —— **而且不报任何错，`|a|` 会显示 0.000 而不是崩溃**。

抓到它的过程也没什么戏剧性：手算了一遍 `16384 - 17194.92 = -810`，
发现校准后 Z 轴是负的而不是 +1g，才意识到方向反了。正确值是 `810.92`。

真正起作用的是**测试让我不可能再退回去**：修好之后补的
`test_calibration_preserves_gravity` 和 `test_calibration_magnitude_matches_libm`
把两个方向都钉死了 —— 既要求"校准后必须还是 1.000 g"，
也要求"不能被减成 0.000"。变异测试确认过：换成错值立刻 7 条失败。

**教训**：写传感器标定前先问一句"这个量静止时的**真值**是什么"。
陀螺仪是 0，加速度计是 1g。混了这两者，代码会安静地毁掉所有姿态数据。
另外，`|a| = 0.000` 这种"过于完美"的结果本身就是可疑信号 ——
真实的物理量很少精确等于零。

### 第五个：所有主机检查都通过，第一次真编板端就挂了（本轮，VM 里实测）

第一次在 VM 里跑 `verify-mpu6050.sh`，第 2 步交叉编译就失败：

```
tools/mpu6050-probe.c:30:10: fatal error: mpu6050.h: No such file or directory
```

`mpu6050.h` 在 `src/`，而 `tools/mpu6050-probe.c` 在 `tools/` ——
板端的编译规则**没带 `-Isrc`**，所以这个 include 解析不了。

**为什么主机上一切正常**：`host-syntax` 用的是 `TEST_CFLAGS`，而那个变量**带着 `-Isrc`**。
所以这个文件在 Windows 上通过了所有检查，直到真正交叉编译才暴露。

> **核心教训**：**检查用的标志和真实构建不一致，就等于没检查。**
> 一个带 `-Isrc` 的语法检查，无法证明一个不带 `-Isrc` 的构建能通过。

**修复**：把 `-Isrc` 抽成 `TOOLS_CPPFLAGS`，板端规则引用它。

**并补上防线**：新增 `make board-flags` —— 用**板端同一套标志变量**（`TOOLS_CPPFLAGS`
+ `CFLAGS`）做语法检查，已加进 `make test` 默认流程。

**变异测试确认有效**：把 `TOOLS_CPPFLAGS` 清空，`board-flags` 立刻报出
**和 VM 里一字不差的同一个错误**：

```
tools/mpu6050-probe.c:30:10: fatal error: mpu6050.h: No such file or directory
mingw32-make: *** [Makefile:177: board-flags] Error 1
```

顺便解释一个容易困惑的点：**为什么视频流水线的目标（`v4l2_mpp_encode` 等）
不带 `-Isrc` 也能编过**？因为它们**住在 `src/` 里**，`#include "xxx.h"` 会先找
**当前源文件所在目录** —— 而头就在同一个目录。`tools/` 下的文件没有这个便利。

### 第六个：编过了、传上去了、跑起来了，然后 225 个 io error（本轮，真硅片上）

修完 `-Isrc` 之后，流程终于走通：VM 编出 ARM 二进制（`e_machine=0x2800`）、
adb 推上板、字节数核对一致、程序真的跑起来了。然后它**读不到任何寄存器**：

```
0x68: transport error (-2)
... 共 112 个地址全部 transport error ...
225 io error(s)
```

**第一步不是看 C 代码，是找对照**：同一块板、同样 gpio70/71、同一颗片子，
`/userdata/i2c-bitbang.py`（Python 版，之前验证硬件时写的）**`found 0x68`，一切正常**。

> 这是一个"把问题一分为二"的典型：Python 能过、C 不能过，且两者跑在同一块硅片上，
> 那么**问题一定在两者实现不同的那一段**，不在硬件、不在接线、不在内核。
> 没有这个对照，很可能会去怀疑上拉电阻或设备树，那是完全错误的方向。

于是把差异缩小到 GPIO 的写序列。手工在板上做实验：

```sh
echo 70 > /sys/class/gpio/export
d=/sys/class/gpio/gpio70
echo out > $d/direction
echo 0   > $d/value          # ← 成功
echo in  > $d/direction
echo 0   > $d/value          # ← 失败：write error: Operation not permitted
```

**根因**：Linux 的 sysfs GPIO，**在 `direction=in` 时写 `value` 会返回 EPERM**。
而 `sysfs_line_low` 当时的顺序是「先写 value，再切 out」——
**第一次调用时方向还是 `in`，所以那一写必然失败**，`line_low` 返回 -1，总线根本没起来。

对照 Python 版（`tools/i2c-bitbang.py` 第 111-113 行）是「**先 out，再写 0**」——
顺序正好相反。**这就是全部差异。**

> **反直觉之处**：对裸机/寄存器级 GPIO 来说，"先设锁存器再切输出"才是正确顺序，
> 否则切输出的一瞬间会驱动锁存器里的旧值，在共享总线上可能是根高速脉冲。
> Linux sysfs **不允许**这么写，于是只能先切方向。
> 代价是一个瞬态（切 out 到写 0 之间驱动旧锁存值），但内核在设为 input 时会清输出锁存器，
> 且 `sysfs_gpio_open` 在第一个 START 之前把两根线都留成 input —— 所以这个瞬态是**低**，无害。

**修复**（`src/gpio_sysfs.c`，两处）：

1. `sysfs_line_low`：对调两行，`pin_set_direction(pin, "out")` 移到写 value 之前。
2. `sysfs_line_read`：显式 `pin_set_direction(pin, "in")`。
   读一个还是 output 的脚，内核返回的是**我们上次写的锁存值**而不是线上电平 ——
   那样每个 ACK 都会读成"我们刚发的东西"，目标看起来永远在应答。
   `read_byte` 当前恰好每次读之前都释放过 SDA，但那是**调用序列的性质，不是这个函数能依赖的**。

**把假设变成检查**：`verify-mpu6050.sh` 新增 `check_sysfs_semantics()`，
在板上显式验证"out 后写 value 成功、in 后写 value 失败"两半。
两半都要 —— 如果哪天**第二半开始成功**，说明这个内核的顺序约束变了，
`sysfs_line_low` 里那段注释就过期了，脚本会提醒。

**教训**：**编译器不会因为「你的 GPIO 写悄悄返回 EPERM」而报警**。
`-Wall -Wextra` 全绿、357 项主机测试全绿，总线依然可以是死的 ——
因为没有任何一项主机测试碰得到内核的 sysfs 语义。
**这类"跑在真设备上才暴露"的 bug，唯一有效的防线是在设备上做一次显式检查。**

### ✅ 修复已上板确认（2026-09-28）

重新交叉编译（**76132 字节**，比带 bug 的 75896 大 236 字节）→ 推送（字节数核对一致）
→ 运行，结果：

| | 修复前 | 修复后 |
|---|---|---|
| `io error(s)` | **225** | **0** ✅ |
| `nack(s)` | — | 0 |
| 扫描结果 | 112 个地址全 `transport error (-2)` | `0x68: present` ✅ |
| `WHO_AM_I` | 读不到 | `0x70 (MPU6500)` ✅ |
| `\|a\| raw / calibrated` | 读不到 | `1.056 / 0.996` ✅ |

`check_sysfs_semantics()` 也首次在真实板上跑到：返回 `OUT_THEN_VALUE_OK` +
`IN_THEN_VALUE_FAIL`，与脚本预期完全一致。

> **这次能一次定位，靠的是"有对照"**：Python 版在同一块板、同样引脚、同一颗片子上是好的。
> 两者只有实现不同 → 问题必定在那段差异里，与硬件/接线/内核无关。
> **没有这个对照，很可能会去怀疑上拉电阻或设备树 —— 那是完全错误的方向。**
> 教训：**手上留一个已验证的参考实现，比多测十次都有用。**

---

## 8. 这个改动会影响线程化测试吗

**不会。零交集。**

| | 线程化改动 | MPU6050 改动 |
|---|---|---|
| 文件 | `frame_ring.*`、`capture_thread.*`、`capture_signal.h` | `mpu6050.*`、`i2c_bitbang.*`、`gpio_sysfs.*`、`tools/mpu6050-probe.c` |
| 数据源 | V4L2 摄像头 NV12 | GPIO2_A6/A7 位翻转 |
| Makefile | `MEDIA_SRC`（未改动） | 独立的 `SENSOR_SRC` + `mpu6050-probe` 目标 |

`Makefile` 里 MPU6050 的源文件**刻意不放进 `MEDIA_SRC`** ——
既然视频流水线一行都没引用它们，放进去只会让板端二进制变大，不改变任何行为。

**唯一会碰到视频流水线的风险点是 GPIO**：本方案只用 gpio70/71（GPIO2_A6/A7），
与摄像头的 CSI/I2C4（gpio3 那一组）**完全不相干**。实测确认这两个引脚
`MUX UNCLAIMED`，且**整个 GPIO2 bank 32 个脚都没被占用**。

**回归验证**（`make test`，bit-bang 重构前 / 后 / 加校准 / 加 board-flags）：

| 测试 | 重构前 | bit-bang 后 | +校准 | 当前 |
|---|---|---|---|---|
| test-packet-queue | 43 | 43 ✅ | 43 ✅ | 43 ✅ |
| test-rtp-rtsp | 86 | 86 ✅ | 86 ✅ | 86 ✅ |
| test-frame-ring | 60 | 60 ✅ | 60 ✅ | 60 ✅ |
| test-capture-thread | 13 | 13 ✅ | 13 ✅ | 13 ✅ |
| test-mpu6050 | 68 | 68 ✅ | **105** ✅ | **105** ✅ |
| test-i2c-bitbang | — | 50 ✅ | 50 ✅ | 50 ✅ |
| **合计** | **270** | **320** | **357** | **357** |
| `board-flags`（语法检查） | — | — | — | **新增** |

> `board-flags` 不产生 assert 计数（它只做 `-fsyntax-only`），但它是
> **唯一用板端标志检查的工具** —— 就是它这样以后不会再出现"主机全过、板端编不过"
> 的情况（见 §7 第五个 bug）。

> `test-mpu6050` 从 68 涨到 105 的部分，全部是校准相关（见 §2）：
> 陀螺仪归零、**重力必须保留**、牛顿迭代开方与 libm 一致性、量程不匹配拒绝、
> 参数与别名校验、温度直通。
>
> 其中「重力必须保留」那组做过**变异测试**验证有效性：把 `MPU6050_ACCEL_BIAS_Z`
> 故意改回错误的 `17194.92`，套件立刻报 **7 条失败**，最直白的一条是
> `bench part reads 1.000 g after correction, got 0.000037` —— 重力被抹成零。
> 测试是真的在守这条线，不是恰好通过。

**唯一会打断线程化测试的是"改设备树+重刷固件"** —— 那是硬件操作，不是代码改动。
本次重构**一行设备树都没碰**，走的是纯用户态 GPIO，所以**完全不打断**。

### 时序单测抓到的三个真 bug（记录）

写 `tests/test_i2c_bitbang.c` 时抓出来的，都值得一提：

1. **`read_byte` 根本没写出读到的值** —— `out` 参数完全没被赋值（`-Wunused-parameter`
   报出来的）。这意味着所有读操作都会返回调用者栈上的垃圾。**编译器的告警直接抓到，
   但只有开着 `-Wextra` 才行。**
2. **测试解码器漏算 ACK 时钟** —— 我最初按"每 8 个 SCL 上升 = 1 字节"解码，
   但每个字节后还有第 9 个 ACK 时钟。这个时钟被当成下一字节的第 1 位，
   于是**每个字节都错位一位**。表现是解出 `0xE8` 而不是 `0xD0` 这类"看着像但不对"
   的值 —— 如果只断言"读到了东西"就永远发现不了。
3. **START 里的 SCL 上升被误记成数据位** —— START 是"SDA 下降且 SCL 高"，
   之后 `scl_low` → `scl_release` 会产生一个上升沿，而那时 SDA 还是高，
   解码器就记了一个 `1` 进去。修法是**在 START/STOP 处重置位计数器**，
   让解码器对齐帧边界。

**共同教训**：时序类的 bug 不会让程序崩溃，只会让数据"看起来差不多"。
必须断言**具体的边沿/字节值**，断言"成功"是没有意义的。

---

## 9. 引脚号速查：别再把 SoC 号当排针号

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
