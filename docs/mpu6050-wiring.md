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

## 3. 需要改设备树把 i2c3 打开

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

所以要把 i2c3 打开，需要在板级 dts（`<SDK>/sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-luckfox-pico-pro-max.dts`）
里：

```dts
&pinctrl {
    i2c3 {
        i2c3m0_xfer: i2c3m0-xfer {
            rockchip,pins =
                <2 RK_PA6 3 &pcfg_pull_none_smt>,   /* SCL: GPIO2_A6 */
                <2 RK_PA7 3 &pcfg_pull_none_smt>;   /* SDA: GPIO2_A7 */
        };
    };
};

&i2c3 {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&i2c3m0_xfer>;
    clock-frequency = <100000>;    /* 400k 也行，首次调试建议 100k */
};
```

⚠️ **改设备树必须重编内核 + 重刷固件**，会打断正在跑的测试。

---

## 4. 不重刷固件的替代路线：用 i2c4m0 临时验证

如果**只调 MPU6050、暂时不管摄像头**，可以切 `i2c4` 到 m0，这样引脚就是排针的
**pin 34 (SDA) / pin 32 (SCL)**（注意：这两个脚在官方图里**没有** I2C 标注，
是 i2c4m0 的复用出口，需要改 dts 才会变成 I2C 功能）：

```dts
&i2c4 {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&i2c4m0_xfer>;    /* 原来是 &i2c4m2_xfer */
    clock-frequency = <100000>;
};
```

**注意**：这与摄像头**互斥** —— 切过去摄像头就没 I2C 了。

**决策建议**：

| 场景 | 方案 | 是否重刷 |
|---|---|---|
| 摄像头和 MPU6050 都要用 | 启用 **i2c3**（排针 pin 14/24） | 需重刷 |
| 只调 MPU6050，摄像头暂时不用 | i2c4 切 m0（排针 pin 34/32） | 需重刷 |
| 先验证软件逻辑，不动硬件 | 见第 5 节，先跑 `mpu6050-probe` 看"总线上什么都没有" | 不需要 |

---

## 5. 先验证软件，再动硬件

即使一根线没接，程序也应该**干净地报告"总线上什么都没有"**，而不是崩溃或挂死。
这是可以先做的事（**现在就能做，不用重刷**）：

```bash
# 交叉编译探测工具
cd /mnt/hgfs/luckfox_share/rv1103
make mpu6050-probe CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-

# 推到板子
adb push mpu6050-probe /userdata/

# 扫描 —— 没接东西时会报 "nothing on the bus"
adb shell "/userdata/mpu6050-probe --bus 4"
```

预期输出（**未接线时**）：

```
bus: /dev/i2c-4 (fd 3)
scanning bus for ACKs (0x08..0x77)...
  0x30: present          <- 摄像头
  0x31: present          <- 摄像头
  ...（MPU6050 不会出现）
```

接好线后应该多出：

```
  0x68: present  <- MPU6050 with AD0 = low
```

**这个对比就是接线是否成功的最直接判据。** 看到 `0x68` 就说明线接对了、地址对了、供电对了。

---

## 6. 接好线之后的验证步骤

```bash
# 1. 扫描确认 0x68 出现
adb shell "/userdata/mpu6050-probe --bus 4"

# 2. 读 WHO_AM_I + 打印实时数据（静止时应看到某个轴 ≈ ±1.000 g）
adb shell "/userdata/mpu6050-probe --bus 4 --dump --count 20"
```

预期 `--dump` 输出：

```
WHO_AM_I = 0x68 (expect 0x68)
 #  accel[g]            gyro[dps]           temp[C]
 0   +0.012  -0.003  +0.998    +0.15    -0.08    +0.05    36.51
 ...
|a| = 1.000 g (expect ~1.000 when still)
```

**三个判据**：
- `WHO_AM_I = 0x68` → 通信通了
- 三轴合成 `|a| ≈ 1.000 g` → 量程/标定正确（地球重力）
- `temp ≈ 36.5°C`（室温偏高一点是正常的，芯片自发热）→ 温度换算正确

若 `|a|` 明显偏离 1.0（比如 8.0），说明量程配置错了 —— 但单测已经覆盖了这个换算，所以更可能是硬件问题。

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
