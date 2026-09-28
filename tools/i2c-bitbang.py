#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
i2c-bitbang.py -- 用 sysfs GPIO 位翻转模拟 I2C 主机，读写总线上的从机。

为什么要这个工具：
    Luckfox Pico Max 的 i2c3 在设备树里是 `status = "disabled"`，
    而本固件的 configfs device-tree overlay 通路是坏的（写非法 dtbo 也返回成功、
    内核不产生任何 fragment 日志），所以 /dev/i2c-3 出不来。
    但 i2c3 的 M0 引脚（GPIO2_A6/A7 = gpio70/71 = 排针 pin24/pin14）在
    i2c3 关闭时是干净的普通 GPIO（pinmux UNCLAIMED），可以自由位翻转。

    本工具不依赖任何内核驱动、不改设备树、不重启，纯用户态 ops。

硬件前提：
    - SCL / SDA 上必须有上拉电阻（GY-521 模块自带 4.7k，实测有效）。
    - 没有上拉就无法产生高电平，一切读到 0。

速率说明：
    sysfs GPIO 单次操作约 10~50us，做不到标准 100kHz。
    但 I2C 从机对时钟速率没有下限，MPU6050 在约 10kHz 下完全正常。
    --delay 控制半周期，默认 25us（约 20kHz）。

用法：
    python3 i2c-bitbang.py scan                      扫描总线 0x03-0x77
    python3 i2c-bitbang.py scan -s 0x68              只扫一个地址
    python3 i2c-bitbang.py read  0x68 0x75           读单字节(WHO_AM_I)
    python3 i2c-bitbang.py read  0x68 0x3B 14        连读 14 字节(加速度+温度+陀螺)
    python3 i2c-bitbang.py write 0x68 0x6B 0x00      写单字节(唤醒 MPU6050)
    python3 i2c-bitbang.py dump  0x68 0x00 0x50      读一片寄存器
    python3 i2c-bitbang.py id                        身份确认 + 唤醒 + 读一次六轴
    python3 i2c-bitbang.py calib 0x68 100            静止偏差采集 -> 驱动校准常数

板端实测结论（2026-09-21）：
    - 扫描到唯一设备 0x68，接线正确。
    - WHO_AM_I = 0x70 而非 0x68 => 模块上是 MPU6500 兼容片，不是原厂 MPU6050。
      输出寄存器布局（0x3B accel / 0x41 temp / 0x43 gyro）与 MPU6050 完全一致，
      驱动代码无需区分型号。
    - 不要用 WHO_AM_I == 0x68 做存在性判断，见 cmd_id 里的 KNOWN 表。
"""

import os
import sys
import time

SCL = 70          # GPIO2_A6 -> 排针 pin 24
SDA = 71          # GPIO2_A7 -> 排针 pin 14
GPIO_ROOT = "/sys/class/gpio"

# sysfs GPIO 半周期延时（秒）。越小越快但可能被 sysfs 开销淹没。
DELAY = 25e-6


# --------------------------------------------------------------------------
# sysfs GPIO 底层
# --------------------------------------------------------------------------

def gpio_write(path, value):
    with open(path, "w") as f:
        f.write(value)


def gpio_read(path):
    with open(path, "r") as f:
        return f.read().strip()


def gpio_setup(num):
    root = os.path.join(GPIO_ROOT, "gpio%d" % num)
    if not os.path.isdir(root):
        try:
            gpio_write(os.path.join(GPIO_ROOT, "export"), str(num))
        except IOError as e:
            raise SystemExit("cannot export gpio%d: %s" % (num, e))
        # 给 udev/内核一点时间把属性建出来
        for _ in range(100):
            if os.path.isdir(root):
                break
            time.sleep(0.005)
    if not os.path.isdir(root):
        raise SystemExit("gpio%d directory not created" % num)
    return root


class Bus(object):
    """开漏 I2C 总线的位翻转实现。

    开漏语义：输出低 = 拉低；输出高 = 让外部上拉拉高（不是推高）。
    所以"释放"就是切成输入，让上拉电阻把线拉起来。
    """

    def __init__(self, scl, sda, delay=DELAY):
        self.delay = delay
        self.scl_dir = os.path.join(gpio_setup(scl), "direction")
        self.scl_val = os.path.join(GPIO_ROOT, "gpio%d" % scl, "value")
        self.sda_dir = os.path.join(gpio_setup(sda), "direction")
        self.sda_val = os.path.join(GPIO_ROOT, "gpio%d" % sda, "value")
        self.release_scl()
        self.release_sda()

    # -- 基本原语 ----------------------------------------------------------
    def _dly(self):
        if self.delay > 0:
            time.sleep(self.delay)

    def scl_low(self):
        gpio_write(self.scl_dir, "out")
        gpio_write(self.scl_val, "0")
        self._dly()

    def scl_high(self):
        gpio_write(self.scl_dir, "in")     # 交给上拉
        self._dly()

    def sda_low(self):
        gpio_write(self.sda_dir, "out")
        gpio_write(self.sda_val, "0")
        self._dly()

    def sda_release(self):
        gpio_write(self.sda_dir, "in")
        self._dly()

    def sda_read(self):
        gpio_write(self.sda_dir, "in")
        self._dly()
        return gpio_read(self.sda_val) == "1"

    def scl_read(self):
        gpio_write(self.scl_dir, "in")
        self._dly()
        return gpio_read(self.scl_val) == "1"

    # -- 释放总线（让引脚回到输入，别霸着）----------------------------------
    def release_scl(self):
        gpio_write(self.scl_dir, "in")

    def release_sda(self):
        gpio_write(self.sda_dir, "in")

    def close(self):
        self.release_scl()
        self.release_sda()
        for num in (SCL, SDA):
            try:
                gpio_write(os.path.join(GPIO_ROOT, "unexport"), str(num))
            except IOError:
                pass

    # -- I2C 时序 ----------------------------------------------------------
    def start(self):
        """START: SCL 高时 SDA 由高 -> 低"""
        self.sda_release()
        self.scl_high()
        self.sda_low()
        self.scl_low()

    def stop(self):
        """STOP: SCL 高时 SDA 由低 -> 高"""
        self.sda_low()
        self.scl_high()
        self.sda_release()

    def write_byte(self, byte):
        """发 1 字节，返回从机 ACK（True=应答）"""
        for i in range(8):
            if byte & 0x80:
                self.sda_release()
            else:
                self.sda_low()
            byte = (byte << 1) & 0xFF
            self.scl_high()
            self.scl_low()
        # 第 9 个时钟：读 ACK
        self.sda_release()
        self.scl_high()
        ack = not self.sda_read()      # 从机拉低 = ACK
        self.scl_low()
        return ack

    def read_byte(self, ack=True):
        """读 1 字节，末位主机发 ACK(True) / NACK(False)"""
        value = 0
        for _ in range(8):
            value = (value << 1) & 0xFF
            self.scl_high()
            if self.sda_read():
                value |= 1
            self.scl_low()
        # 第 9 个时钟：主机回 ACK/NACK
        if ack:
            self.sda_low()
        else:
            self.sda_release()
        self.scl_high()
        self.scl_low()
        self.sda_release()
        return value

    def bus_recover(self):
        """总线自愈：若某从机把 SDA 拉死，发 9 个 SCL 再补一个 STOP。"""
        self.sda_release()
        for _ in range(9):
            self.scl_low()
            self.scl_high()
        self.stop()

    # -- 事务 ---------------------------------------------------------------
    def read_reg(self, addr, reg, count=1):
        """标准 I2C 读：START, addr+W, reg, START, addr+R, data..., STOP"""
        self.start()
        if not self.write_byte((addr << 1) | 0):
            self.stop()
            raise IOError("no ACK from device 0x%02X (addr+W)" % addr)
        if not self.write_byte(reg):
            self.stop()
            raise IOError("no ACK from device 0x%02X (reg 0x%02X)" % (addr, reg))
        self.start()
        if not self.write_byte((addr << 1) | 1):
            self.stop()
            raise IOError("no ACK from device 0x%02X (addr+R)" % addr)
        data = []
        for i in range(count):
            last = (i == count - 1)
            data.append(self.read_byte(ack=not last))
        self.stop()
        return data

    def write_reg(self, addr, reg, values):
        """标准 I2C 写：START, addr+W, reg, data..., STOP"""
        if not isinstance(values, (list, tuple)):
            values = [values]
        self.start()
        if not self.write_byte((addr << 1) | 0):
            self.stop()
            raise IOError("no ACK from device 0x%02X (addr+W)" % addr)
        if not self.write_byte(reg):
            self.stop()
            raise IOError("no ACK from device 0x%02X (reg 0x%02X)" % (addr, reg))
        for v in values:
            if not self.write_byte(v & 0xFF):
                self.stop()
                raise IOError("no ACK from device 0x%02X (data 0x%02X)" % (addr, v))
        self.stop()


# --------------------------------------------------------------------------
# 命令实现
# --------------------------------------------------------------------------

def cmd_scan(bus, start, end):
    print("scanning 0x%02X..0x%02X (SCL=gpio%d SDA=gpio%d delay=%.0fus)"
          % (start, end, SCL, SDA, bus.delay * 1e6))
    found = []
    # 先自愈一次，避免上次异常把总线留在中间状态
    bus.bus_recover()
    for addr in range(start, end + 1):
        # 只用"addr+W 能不能拿到 ACK"判定，比读寄存器更宽容
        try:
            bus.start()
            ok = bus.write_byte((addr << 1) | 0)
            bus.stop()
        except Exception:
            ok = False
            try:
                bus.stop()
            except Exception:
                pass
        if ok:
            found.append(addr)
            print("  found 0x%02X" % addr)
            sys.stdout.flush()
    print("done, %d device(s) found" % len(found))
    return found


def cmd_read(bus, addr, reg, count):
    data = bus.read_reg(addr, reg, count)
    print("read 0x%02X[0x%02X:%d] = %s"
          % (addr, reg, count, " ".join("%02X" % b for b in data)))
    return data


def cmd_write(bus, addr, reg, values):
    bus.write_reg(addr, reg, values)
    print("write 0x%02X[0x%02X] <- %s"
          % (addr, reg, " ".join("%02X" % v for v in values)))


def cmd_dump(bus, addr, reg, count):
    print("dump 0x%02X regs 0x%02X..0x%02X" % (addr, reg, reg + count - 1))
    for base in range(reg, reg + count, 16):
        n = min(16, reg + count - base)
        try:
            data = bus.read_reg(addr, base, n)
        except IOError as e:
            print("  0x%02X: ERROR %s" % (base, e))
            return
        line = "  %02X: " % base
        line += " ".join("%02X" % b for b in data)
        line += "   |"
        line += "".join(chr(b) if 32 <= b < 127 else "." for b in data)
        line += "|"
        print(line)


def cmd_id(bus, addr):
    """MPU6050 快速上手：读 WHO_AM_I -> 唤醒 -> 读一次六轴+温度"""
    import struct

    print("=" * 62)
    print("MPU6050 quick check @ 0x%02X" % addr)
    print("=" * 62)

    who = bus.read_reg(addr, 0x75, 1)[0]
    print("[1] WHO_AM_I (0x75) = 0x%02X" % who)
    # 注意：不要用 == 0x68 做硬判断！
    #   0x68 = MPU6050      0x70 = MPU6500      0x71 = MPU9250
    #   0x73 = MPU9255      0x72/0x98 等 = 兼容/再生片
    # 板端实测这块模块是 0x70，但 0x3B/0x41/0x43 数据布局与 MPU6050 一致。
    KNOWN = {
        0x68: "MPU6050 (original InvenSense)",
        0x70: "MPU6500 (pin/register compatible with MPU6050)",
        0x71: "MPU9250 (9-axis; MPU6050-compatible accel/gyro block)",
        0x73: "MPU9255 (9-axis)",
        0x72: "MPU6515 / related",
        0x98: "ICM-series or clone",
    }
    if who in KNOWN:
        print("    -> %s" % KNOWN[who])
        print("    -> accel/gyro/temp layout identical to MPU6050; code unchanged")
    else:
        print("    -> UNKNOWN id; check wiring / address / power first")
        return

    # 软复位，确保从干净状态开始
    bus.write_reg(addr, 0x6B, 0x80)          # PWR_MGMT_1: DEVICE_RESET
    time.sleep(0.1)
    bus.write_reg(addr, 0x6B, 0x00)          # 唤醒，内建 8MHz 时钟
    time.sleep(0.05)
    bus.write_reg(addr, 0x1C, 0x00)          # ACCEL_CONFIG: +-2g
    bus.write_reg(addr, 0x1B, 0x00)          # GYRO_CONFIG: +-250 dps

    # 读回配置确认写入生效
    pwr = bus.read_reg(addr, 0x6B, 1)[0]
    acc = bus.read_reg(addr, 0x1C, 1)[0]
    gyr = bus.read_reg(addr, 0x1B, 1)[0]
    print("[2] PWR_MGMT_1=0x%02X ACCEL_CFG=0x%02X GYRO_CFG=0x%02X (all should be 0x00)"
          % (pwr, acc, gyr))

    print("[3] sampling 5 frames (accel + temp + gyro) ...")
    for n in range(5):
        d = bus.read_reg(addr, 0x3B, 14)
        ax = struct.unpack(">h", bytes(d[0:2]))[0]
        ay = struct.unpack(">h", bytes(d[2:4]))[0]
        az = struct.unpack(">h", bytes(d[4:6]))[0]
        temp = struct.unpack(">h", bytes(d[6:8]))[0]
        gx = struct.unpack(">h", bytes(d[8:10]))[0]
        gy = struct.unpack(">h", bytes(d[10:12]))[0]
        gz = struct.unpack(">h", bytes(d[12:14]))[0]
        print("    #%d  a=(%6d,%6d,%6d) g=(%6d,%6d,%6d)  %.1f degC"
              % (n, ax, ay, az, gx, gy, gz, temp / 340.0 + 36.53))
        time.sleep(0.1)

    # 判断静止还是晃动：静置时 az 应接近 16384 (1g @ +-2g)
    d = bus.read_reg(addr, 0x3B, 6)
    az = struct.unpack(">h", bytes(d[4:6]))[0]
    print("[4] verdict: |az|=%.3f g" % (abs(az) / 16384.0))
    if 0.7 < abs(az) / 16384.0 < 1.3:
        print("    -> gravity magnitude is consistent with a healthy, still sensor")


def cmd_calib(bus, addr, samples):
    """采集静止偏差，输出可直接写进驱动的校准常数。

    静止时：accel 三轴合成应为 1g，gyro 三轴应为 0。
    accel 偏差按"减去样本均值、Z 轴再补足 1g"处理；
    gyro 偏差直接取均值。
    """
    import struct

    print("=" * 66)
    print("static bias calibration @ 0x%02X , %d samples" % (addr, samples))
    print("keep the sensor perfectly still on a level surface ...")
    print("=" * 66)

    bus.write_reg(addr, 0x6B, 0x80)
    time.sleep(0.1)
    bus.write_reg(addr, 0x6B, 0x00)
    time.sleep(0.05)
    bus.write_reg(addr, 0x1C, 0x00)     # +-2g
    bus.write_reg(addr, 0x1B, 0x00)     # +-250 dps
    # DLPF 打开，压掉高频噪声
    bus.write_reg(addr, 0x1A, 0x03)

    sa = [0.0] * 3
    sg = [0.0] * 3
    st = 0.0
    for n in range(samples):
        d = bus.read_reg(addr, 0x3B, 14)
        for i in range(3):
            sa[i] += struct.unpack(">h", bytes(d[i * 2:i * 2 + 2]))[0]
        st += struct.unpack(">h", bytes(d[6:8]))[0]
        for i in range(3):
            sg[i] += struct.unpack(">h", bytes(d[8 + i * 2:10 + i * 2]))[0]
        time.sleep(0.02)
        sys.stdout.write("\r  sampling %d/%d" % (n + 1, samples))
        sys.stdout.flush()
    print()

    ma = [v / samples for v in sa]
    mg = [v / samples for v in sg]
    mt = st / samples

    print("accel mean (LSB): %8.2f %8.2f %8.2f" % tuple(ma))
    print("gyro  mean (LSB): %8.2f %8.2f %8.2f" % tuple(mg))
    print("temp  mean      : %8.2f  -> %.2f degC" % (mt, mt / 340.0 + 36.53))
    print("accel |mean|    : %.4f g  (should be ~1.0 if level)" %
          ((ma[0] ** 2 + ma[1] ** 2 + ma[2] ** 2) ** 0.5 / 16384.0))

    print()
    print("--- paste into driver as static calibration ---")
    print("#define IMU_ACCEL_BIAS_X   (%.0ff)" % ma[0])
    print("#define IMU_ACCEL_BIAS_Y   (%.0ff)" % ma[1])
    print("#define IMU_ACCEL_BIAS_Z   (%.0ff)" % (ma[2] - 16384.0))
    print("#define IMU_GYRO_BIAS_X    (%.0ff)" % mg[0])
    print("#define IMU_GYRO_BIAS_Y    (%.0ff)" % mg[1])
    print("#define IMU_GYRO_BIAS_Z    (%.0ff)" % mg[2])
    print("#define IMU_ACCEL_LSB_PER_G   16384.0f")
    print("#define IMU_GYRO_LSB_PER_DPS  131.0f")
    print("#define IMU_TEMP_LSB_PER_DEG  340.0f")
    print("#define IMU_TEMP_OFFSET_C     36.53f")


# --------------------------------------------------------------------------

def usage():
    print(__doc__)
    raise SystemExit(1)


def main(argv):
    global DELAY
    if len(argv) < 2:
        usage()

    # 全局可选 --delay <us>
    args = []
    i = 1
    while i < len(argv):
        if argv[i] == "--delay":
            i += 1
            DELAY = float(argv[i]) * 1e-6
        elif argv[i] in ("-h", "--help"):
            usage()
        else:
            args.append(argv[i])
        i += 1

    if not args:
        usage()

    cmd = args[0]
    bus = Bus(SCL, SDA, DELAY)
    try:
        if cmd == "scan":
            start, end = 0x03, 0x77
            if "-s" in args:
                start = end = int(args[args.index("-s") + 1], 0)
            elif len(args) >= 3:
                start, end = int(args[1], 0), int(args[2], 0)
            cmd_scan(bus, start, end)

        elif cmd == "read":
            if len(args) < 3:
                usage()
            addr = int(args[1], 0)
            reg = int(args[2], 0)
            count = int(args[3], 0) if len(args) > 3 else 1
            cmd_read(bus, addr, reg, count)

        elif cmd == "write":
            if len(args) < 4:
                usage()
            addr = int(args[1], 0)
            reg = int(args[2], 0)
            values = [int(v, 0) for v in args[3:]]
            cmd_write(bus, addr, reg, values)

        elif cmd == "dump":
            if len(args) < 3:
                usage()
            addr = int(args[1], 0)
            reg = int(args[2], 0)
            count = int(args[3], 0) if len(args) > 3 else 0x50
            cmd_dump(bus, addr, reg, count)

        elif cmd == "id":
            addr = int(args[1], 0) if len(args) > 1 else 0x68
            cmd_id(bus, addr)

        elif cmd == "calib":
            addr = int(args[1], 0) if len(args) > 1 else 0x68
            samples = int(args[2], 0) if len(args) > 2 else 100
            cmd_calib(bus, addr, samples)

        else:
            usage()

    except IOError as e:
        print("I2C ERROR: %s" % e)
        print("hint: run 'scan' first to see which addresses answer.")
        return 2
    finally:
        bus.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
