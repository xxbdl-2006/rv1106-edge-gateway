# Luckfox Pico Max 原理图 & 排针引脚速查

> 来源：官方原理图 `Luckfox-Pico-Pro-Max.pdf`（`github.com/LuckfoxTECH/Luckfox-Pico-docs`
> → `Hardware/Schematic/`），并与**板端自带的官方排针图**（`/usr/bin/luckfox-config`
> 里的 `luckfox_pico_pro_max_pin_diagram_file()`）逐脚对账 —— 两处**完全一致**。

## 1. 文件位置

| 文件 | 说明 |
|---|---|
| `datasheets/Luckfox-Pico-Pro-Max.pdf` | **整板原理图**（A4 横版，单页，1.18 MB，矢量） |
| `datasheets/Core1106.pdf` | 核心板 Core1106 原理图 |
| `datasheets/Core1106-PinOut.xls` | Core1106 引脚定义表 |
| `datasheets/schematic-full.png` | 原理图整页渲染（5052×3570） |
| `datasheets/header-u3-zoom.png` | **排针 U3 放大图**（5280×1344） |
| `datasheets/gpio-header.png` | 排针 U3 符号截图 |

原理图是**矢量 PDF**（27,795 个绘图对象、零位图），所以用 PyMuPDF 提取文本/渲染
都无损清晰。本机已装好渲染环境：

```powershell
# 已创建托管 venv 并装 pymupdf
C:\Users\adms\.workbuddy\binaries\python\envs\default\Scripts\python.exe -c "
import pymupdf
d = pymupdf.open('F:/luckfox_share/datasheets/Luckfox-Pico-Pro-Max.pdf')
d[0].get_pixmap(matrix=pymupdf.Matrix(24,24)).save('out.png')"
```

## 2. 🔴 40 针排针引脚表（USB 朝上，pin 1 左上，左奇右偶）

**这张表是接线唯一依据。** 排针号 ≠ SoC 引脚号 —— 同一条网络可以引出到多个排针
（例如 `GPIO1_D1` 同时在 pin 33 和 37，`GPIO2_A7` 同时在 pin 14 和 38）。

| pin | SoC 引脚 | 复用功能 | | pin | SoC 引脚 | 复用功能 |
|---:|---|---|---|---:|---|---|
| **1** | GPIO1_B2 | `FIQtty_TX` | | **2** | **VBUS** | 5V 输入 |
| **3** | GPIO1_B3 | `FIQtty_RX` | | **4** | **VSYS** | 电池/系统电压 |
| **5** | **GND** | | | **6** | **GND** | |
| **7** | GPIO1_C7 | `UART4_M1_CTS` | | **8** | **3V3_EN** | 3.3V 使能 |
| **9** | GPIO1_C6 | `UART4_M1_RTS` | | **10** | **3V3_OUT** | ✅ **3.3V 输出** |
| **11** | GPIO1_C5 | `UART4_M1_TX` | | **12** | NC | 悬空 |
| **13** | GPIO1_C4 | `UART4_M1_RX` | | **14** | GPIO2_A7 | ✅ **`I2C3_M0_SDA`** |
| **15** | **GND** | | | **16** | **GND** | |
| **17** | GPIO1_D2 | `I2C3_M1_SDA` | | **18** | GPIO4_C1 | `SARADC_M1` |
| **19** | GPIO1_D3 | `I2C3_M1_SCL` | | **20** | GPIO4_C0 | `SARADC_M0` |
| **21** | GPIO2_B1 | `I2C1_M1_SDA` | | **22** | RESET | 复位 |
| **23** | GPIO1_C0 | `SPI0_M0_CS0` | | **24** | GPIO2_A6 | ✅ **`I2C3_M0_SCL`** |
| **25** | **GND** | | | **26** | **GND** | |
| **27** | GPIO1_C1 | `SPI0_M0_CLK` | | **28** | GPIO2_A3 | |
| **29** | GPIO1_C2 | `SPI0_M0_MOSI` | | **30** | GPIO2_A2 | |
| **31** | GPIO1_C3 | `SPI0_M0_MISO` | | **32** | GPIO2_A1 | |
| **33** | GPIO2_B0 | `I2C1_M1_SCL` | | **34** | GPIO2_A0 | |
| **35** | **GND** | | | **36** | **GND** | |
| **37** | GPIO1_D0 | `UART3_M1_TX` | | **38** | GPIO2_A5 | `UART1_M1_RX` |
| **39** | GPIO1_D1 | `UART3_M1_RX` | | **40** | GPIO2_A4 | `UART1_M1_TX` |

### 电源脚速记

- **5V 输入** = pin 2 (VBUS) 或 pin 4 (VSYS)
- **3.3V 输出** = **pin 10**（`3V3_OUT`）— pin 8 是 `3V3_EN`（输入，不是输出）
- **GND** = pin 5 / 6 / 15 / 16 / 25 / 26 / 35 / 36

## 3. I2C 分布（本次核对重点）

原理图里所有 I2C 控制器及其可用 mux 档位：

| 控制器 | mux | SCL | SDA | 引出位置 |
|---|---|---|---|---|
| **I2C3** | **M0** | **pin 24** (GPIO2_A6) | **pin 14** (GPIO2_A7) | ✅ 排针现成可用 |
| I2C3 | M1 | pin 19 (GPIO1_D3) | pin 17 (GPIO1_D2) | ✅ 排针现成可用 |
| I2C1 | M1 | pin 33 (GPIO2_B0) | pin 21 (GPIO2_B1) | ✅ 排针现成可用 |
| I2C0 | M1 | — | — | 在 eMMC/FSPI 组 |
| I2C2 | M1 | — | — | 在 eMMC/FSPI 组 |
| I2C4 | M0 | GPIO2_A1 | GPIO2_A0 | 见下 |
| I2C4 | M1 | GPIO1_C2 | GPIO1_C3 | 见下 |
| I2C4 | M2 | GPIO3_C7 | GPIO3_D0 | 见下 |

### ⚠️ 关键结论：排针上现成能用的 I2C 是 **i2c3m0**（pin 14/24）

原理图 `U1B` 符号的 mux 表明确写出：

```
I2C3_SCL_M0   ← 复用自 GPIO2_A6   (SoC pin 107)
I2C3_SDA_M0   ← 复用自 GPIO2_A7   (SoC pin 106)
```

而 GPIO2_A6 / GPIO2_A7 正好在排针 **pin 24 / pin 14**。板端 `luckfox-config` 的
排针图也一字不差地标着：

```
 PWM2_M2  - SPI0_M0_CS0   - GPIO1_C0 |       | GPIO2_A6 - I2C3_M0_SCL -
          -               -      GND |       | GND      -             -
 PWM4_M2  - SPI0_M0_CLK   - GPIO1_C1 |       | GPIO2_A3 -             -
 ...
 PWM9_M1  - UART4_M1_TX   - GPIO1_C5 |       | NC       -             -
 PWM8_M1  - UART4_M1_RX   - GPIO1_C4 |       | GPIO2_A7 - I2C3_M0_SDA -
```

**注意**：`i2c4` 的三个 mux 全都落在 **MIPI/摄像头接口**（GPIO3_C7 / GPIO3_D0 /
GPIO1_C2 / GPIO1_C3 / GPIO2_A0 / GPIO2_A1），与 `VI_CIF_*`、`MIPI/LVDS_*` 复用，
且本固件 **i2c0/1/2/3 在设备树里全是 `disabled`、只有 i2c4 是 `okay`**。
所以：

- 想用 **i2c3m0**（排针 pin 14/24）→ 必须**改设备树把 i2c3 打开 + 重刷固件**
- 想用当前已 `okay` 的 **i2c4** → 只能走 MIPI 那组脚，与摄像头互斥

两条路都要改 dts。**没有"免重刷"的接线方案。**

## 4. 排针号 ↔ SoC 引脚号 换算

```
SoC 号 = bank * 32 + group * 8 + X
```
例：`GPIO2_A6` → `2*32 + 0*8 + 6 = 70`

查 pinmux / 设备树用 **SoC 号**；接线只看 **排针号**。

## 5. 板端自查命令

```bash
# 打印官方排针图 (USB 朝上)
sed -n '/luckfox_pico_pro_max_pin_diagram_file/,/^}/p' /usr/bin/luckfox-config

# 看某个 pinmux 当前档位
cat /sys/kernel/debug/pinctrl/*/pinmux-pins | grep -i i2c

# 看 i2c 控制器是否 enabled
for d in /proc/device-tree/i2c@*; do echo "$d: $(cat $d/status 2>/dev/null)"; done
```

## 6. MPU6050 接线（据此表的最终结论）

| MPU6050 | 接到排针 | 说明 |
|---|---|---|
| VCC | **pin 10** | `3V3_OUT`（不是 pin 36，36 是 GND） |
| GND | pin 6 / 16 (任一) | |
| SCL | **pin 24** | `GPIO2_A6` = `I2C3_M0_SCL` |
| SDA | **pin 14** | `GPIO2_A7` = `I2C3_M0_SDA` |
| AD0 | GND 或 3V3 | 决定地址 `0x68` / `0x69` |
| INT | 任意 GPIO（如 pin 28/30/32） | 可选 |

**前置条件**：需先把设备树里的 `i2c3` 从 `disabled` 改为 `okay` 并重刷固件，
否则 `/dev/i2c-3` 不存在、扫描不到。

参见 [`mpu6050-wiring.md`](./mpu6050-wiring.md)。

---

## 附录：外部引脚图核对（2026-09-28）

用户提供了一张店铺/第三方绘制的引脚图。**核对结论：40 个引脚的名称与复用功能
全部与官方三源一致，可以放心使用。**

### 核对方法（三源交叉）

| 源 | 说明 |
|---|---|
| **A 店铺图** | 用户提供的彩色引脚图 |
| **B 板端官方图** | `luckfox-config` 内嵌的 `luckfox_pico_pro_max_pin_diagram_file()` |
| **C 原理图** | `Luckfox-Pico-Pro-Max.pdf` 的 U3 排针符号 + U1B mux 表 |

### 结果

- **40/40 引脚名称完全一致**（GPIO1_B2 … GPIO2_A4），无一处错位。
- **复用功能标注一致**，且店铺图比板端官方图**更全**：
  - 板端图 pin 1/3 只写 `FIQtty_TX`/`FIQtty_RX`（那是内核 fiq-debugger 占用的名字）；
    店铺图写的 `UART2_TX_M1`/`UART2_RX_M1` 才是 **RV1106 数据手册里的复用功能名**。
    查原理图 mux 表证实：`UART2_TX_M1 ← GPIO1_B2`、`UART2_RX_M1 ← GPIO1_B3`。**店铺图对。**
  - 板端图 pin 23 标注 `SPI0_M0_CS0`，与店铺图的 `SPI0_CS0_M0` 是**同一个功能的两种写法**。
  - 板端图 pin 32/34 完全没标 I2C，店铺图标了 `I2C4_SCL_M0`/`I2C4_SDA_M0` ——
    原理图 mux 表证实 `I2C4_SCL_M0 ← GPIO2_A1`、`I2C4_SDA_M0 ← GPIO2_A0`。**店铺图对。**
- **板载 LED 标注 `GPIO3_C6_d` 已验证为真**：实测 device tree
  `leds/work/gpios = <0x33 0x16 0x0>`，而 `gpio3` (ff550000) 的 `phandle = 0x33`、
  pin `0x16 = 22` → `bank3 + C6` = **GPIO3_C6**，对应 `/sys/class/leds/work`。

### 需要注意的两点

1. **店铺图也是「USB 朝上、pin 1 左上」的朝向**，与官方一致。**但实板排针的 pin 1
   定义仍建议以板端丝印为准**（画错朝向是最容易犯的错）。
2. **复用功能名带 `_M0/_M1/_M2` 后缀，是 mux 档位，不是引脚号。**
   例如 `I2C3_SDA_M0` 和 `I2C3_SDA_M1` 是**同一个 I2C3 控制器的两种引脚出口**：
   - `I2C3_M0` → pin 24 (SCL) / pin 14 (SDA)  ✅ 本固件推荐
   - `I2C3_M1` → pin 19 (SCL) / pin 17 (SDA)

   接 MPU6050 用 **M0**（pin 24/14），别看成 M1。

3. 店铺图**没有**标注 `I2C2`、`EMMC`、`VO_LCDC` 等功能 —— 它只画了「用户可用」的复用项，
   不是遗漏。完整 mux 表见原理图 U1B 符号。

### 最终结论

**这张店铺图是准确的，可以照着接线。** MPU6050 仍按
`VCC→pin 10 / GND→pin 6或16 / SCL→pin 24 / SDA→pin 14`，
**前提是先把设备树里的 i2c3 打开**（本固件 i2c0/1/2/3 全 `disabled`，只有 i2c4 是 `okay`）。
