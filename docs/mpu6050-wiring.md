# MPU6050 接线与接入

这篇文档解决一个问题：**MPU6050 到底接在哪两根线上**。

结论先说：**不要接 GPIO3_B7 / GPIO3_C0**，那两个脚接不了。要接的是排针的 **pin 24 / pin 25**。

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

## 2. 正确接法：排针 pin 24 / pin 25

i2c4 有三个复用组，排针上引出的是 **`i2c4m0`**：

| 复用组 | SDA | SCL | 排针引出 |
|---|---|---|---|
| `i2c4m0` | GPIO2_A0 (**pin 64**) | GPIO2_A1 (**pin 65**) | ✅ 排针 pin 24 / pin 25 |
| `i2c4m1` | GPIO1_B2 (pin 50) | GPIO1_B3 (pin 51) | ❌ 未引出 |
| `i2c4m2` | GPIO3_B7 (pin 119) | GPIO3_C0 (pin 120) | ❌ GPI 脚，摄像头排线专用 |

实测 pin 64 / 65 **完全空闲、未被占用、支持输入输出**：

```
$ adb shell "cat /sys/kernel/debug/pinctrl/pinctrl-rockchip-pinctrl/pinmux-pins | grep -E 'pin (64|65) '"
pin 64 (gpio2-0): (MUX UNCLAIMED) (GPIO UNCLAIMED)
pin 65 (gpio2-1): (MUX UNCLAIMED) (GPIO UNCLAIMED)
```

### 接线表

| MPU6050 | Luckfox Pico Pro/Max | 说明 |
|---|---|---|
| **VCC** | pin 36 `3V3(OUT)` | **必须 3.3V**。MPU6050 的 VDD 耐压 3.4V，接 5V 会烧 |
| **GND** | pin 3 / 8 / 13 / 18 / 23 / 28 / 33 | 任一 GND 脚 |
| **SCL** | **pin 25** (GPIO2_A1) | `I2C4_SCL_M0` |
| **SDA** | **pin 24** (GPIO2_A0) | `I2C4_SDA_M0` |
| **AD0** | GND 或悬空 | 接地 → 地址 `0x68`；接 3V3 → `0x69` |
| **INT** | 不接 | 当前用轮询读取，不需要中断 |
| **XDA / XCL** | 不接 | 辅助 I2C，用于挂磁力计，本项目不用 |

⚠️ **注意**：`pin 24/25` 在排针丝印上标的是 `UART1_CTS_M1` / `UART1_RTS_M1`。
这是复用功能，同一时刻只能选一个 —— 用 I2C 就用不了这组 UART1。本项目没用到 UART1，没有冲突。

---

## 3. 改设备树把 i2c4 从 m2 切到 m0

**这是唯一需要改设备树的地方**，而且必须做 —— 因为当前 `i2c4m2` 占着 GPI 脚，
`i2c4m0` 的 pin 64/65 还没被 mux 成 i2c 功能，不切过去的话排针上量不到信号。

在 SDK 的板级 dts 里改 `i2c4` 节点（RV1106 的板级文件通常在
`<SDK>/sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-luckfox-pico-pro-max.dts`）：

```dts
/* 在 &pinctrl 节点下，把 i2c4 的组定义改成 m0 */
&pinctrl {
    i2c4 {
        i2c4m0_xfer: i2c4m0-xfer {
            rockchip,pins =
                <2 RK_PA0 3 &pcfg_pull_none_smt>,   /* SDA: GPIO2_A0 */
                <2 RK_PA1 3 &pcfg_pull_none_smt>;   /* SCL: GPIO2_A1 */
        };
    };
};

/* 然后让 i2c4 引用 m0 组 */
&i2c4 {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&i2c4m0_xfer>;    /* 原来是 &i2c4m2_xfer */
    clock-frequency = <100000>;    /* 400k 也行，但首次调试建议 100k */
};
```

⚠️ **注意两点**：

1. **改了 pinmux 会不会影响摄像头？** 不会。摄像头挂在 i2c4m2 的**线上**，
   但 m2 和 m0 是**同一控制器的不同引脚出口**。切到 m0 之后，
   **摄像头排线上的 I2C 就没信号了，摄像头会挂**。所以：
   - 如果你要**同时**用摄像头和 MPU6050 → **不能切**，得把 MPU6050 也接到摄像头排线上（不现实），
     或者用别的空闲 I2C 控制器（如 i2c3，见第 4 节）。
   - 如果只是**单独调试 MPU6050** → 切过去没问题。

2. **改设备树必须重编内核 + 重刷固件**，会打断正在跑的测试。所以：

---

## 4. 更推荐：不动设备树，用别的空闲 I2C

既然改 dts 代价大又要重刷，先看有没有别的空闲控制器。查一遍：

```bash
adb shell "cat /sys/kernel/debug/pinctrl/pinctrl-rockchip-pinctrl/pinmux-functions | grep i2c"
```

RV1106 的 i2c0/1/2/3 在板上多数未启用，其中 **i2c3 的 `i2c3m0`**
（pin 29 = `I2C3_SCL_M0`、pin 34 = `I2C3_SDA_M0`）在排针上是现成的，
设备树里 `status` 设成 `okay` 即可，不必碰 i2c4，也就**完全不影响摄像头**。

先确认这两个脚是不是空闲的：

```bash
adb shell "cat /sys/kernel/debug/pinctrl/pinctrl-rockchip-pinctrl/pinmux-pins | grep -E 'pin (100|101|102|103|104|105|106|107) '"
```

（GPIO2_A6 = pin 70，GPIO2_A7 = pin 71 —— 具体号以实测输出为准。）

**决策建议**：

| 场景 | 方案 |
|---|---|
| 只想先把 MPU6050 调通，摄像头暂时不用 | 改 i2c4 → m0，重编重刷 |
| 摄像头和 MPU6050 都要用 | 启用 i2c3（排针 pin 29/34），i2c4 原样不动 |
| 不想重刷固件 | **用下面第 5 节的软件位操作**，暂时验证 |

---

## 5. 不重刷固件的临时验证法（软件手动 mux）

内核启动后，用寄存器直接改 pinmux 也能让 pin 64/65 变成 I2C 功能。
既然当前 i2c4 控制器已经 `okay`，只要**把这两个脚 mux 到 i2c 功能**，
就能直接在排针上测到信号：

```bash
# 需要 root（板端默认就是 root）
# GPIO2_A0/A1 的 iomux：GPIO2 IOC 块 + iomux 偏移
# RV1106 GPIO2 的 m0 复用值是 3
```

⚠️ 这属于**非正规做法**：寄存器地址随内核版本变化，且会与 pinctrl 子系统打架。
**不建议作为长期方案**，但作为"接好线先确认传感器活着"的验证手段可以接受。

**更省事的替代**：先不切引脚，直接用 **i2c4 现有的 m2 总线**验证代码逻辑 ——
把 MPU6050 的 SDA/SCL **临时点焊到摄像头 FPC 座的对应脚上**（很考验手工），
或者**先只验证软件**（见下）。

---

## 6. 先验证软件，再动硬件

即使一根线没接，程序也应该**干净地报告"总线上什么都没有"**，而不是崩溃或挂死。
这是可以先做的事：

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

## 7. 接好线之后的验证步骤

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

## 8. 代码结构

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

## 9. 单测抓到的真实 bug（记录）

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

## 10. 这个改动会影响线程化测试吗

**不会。零交集。**

| | 线程化改动 | MPU6050 改动 |
|---|---|---|
| 文件 | `frame_ring.*`、`capture_thread.*`、`capture_signal.h` | `mpu6050.*`、`mpu6050_i2c.c`、`tools/mpu6050-probe.c` |
| 数据源 | V4L2 摄像头 NV12 | `/dev/i2c-4` 寄存器 |
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
如果按第 4 节启用 i2c3，或先只做软件验证（第 6 节），则**完全不打断**。
