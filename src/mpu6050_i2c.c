/*
 * MPU6050 transport over bit-banged I2C.
 *
 * The driver used to talk to /dev/i2c-N. That stopped being possible on this
 * board: the i2c3 controller serving header pins 24/14 is status = "disabled"
 * in the device tree, and the runtime overlay route that would flip it turned
 * out not to work (see dts/i2c3-enable.dts for the evidence). Driving the pins
 * directly sidesteps the controller entirely, because a disabled controller
 * never claims its pinctrl and leaves the pins as plain GPIO.
 *
 * Split of responsibility:
 *
 *   i2c_bitbang.c   I2C timing, host testable, no I/O
 *   gpio_sysfs.c    the sysfs pin backend, Linux only
 *   this file       MPU6050 register policy on top of both
 *
 * Kept separate from mpu6050.c so the decoding stays host-testable. This file
 * is inherently Linux-only; on Windows it compiles to nothing, which lets the
 * Makefile list it unconditionally without breaking `make test`.
 */

#include "mpu6050.h"

#ifdef __linux__

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gpio_sysfs.h"
#include "i2c_bitbang.h"

/*
 * EREMOTEIO is the errno a Linux I2C stack returns when a slave did not
 * acknowledge. It is spelled EREMOTEIO in glibc but some uclibc
 * configurations do not export it, so it is defined here as a fallback rather
 * than allowed to break the build. The value 121 is the Linux ABI value and is
 * stable, so the fallback is not a guess.
 */
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif

/*
 * Header pin 24 is SCL and pin 14 is SDA, both on the i2c3 M0 mux group.
 * Their plain GPIO numbers are what the sysfs backend wants, and the mapping
 * is SoC-pin = bank*32 + group*8 + index, so GPIO2_A6 -> 70 and GPIO2_A7 -> 71.
 *
 * These are the values from the board's own pinmux output, not a guess:
 *   pin 70 (gpio2-6) (MUX UNCLAIMED)
 *   pin 71 (gpio2-7) (MUX UNCLAIMED)
 * UNCLAIMED is what makes this whole approach legal; a claimed pin would mean
 * the controller is already running and we should have used /dev/i2c-3.
 */
#define MPU6050_BITBANG_SCL_GPIO 70
#define MPU6050_BITBANG_SDA_GPIO 71

/*
 * 25 us per half period, so roughly 20 kHz. sysfs costs 10-50 us per access on
 * this SoC, so a smaller number would mostly be swallowed by that overhead and
 * would only add jitter. An I2C target has no minimum clock rate, and the
 * 14 byte burst still finishes in a few milliseconds, which is far faster than
 * the 100 Hz sampling this feeds.
 */
#define MPU6050_BITBANG_HALF_PERIOD_US 25

struct mpu6050_imu {
    struct sysfs_gpio *gpio;
    struct i2c_bitbang bus;
    uint8_t address;
    float accel_scale;
    float gyro_scale;
    int accel_fsr;
    int gyro_fsr;
};

static int write_reg(struct mpu6050_imu *imu, uint8_t reg, uint8_t value)
{
    int rc = i2c_bb_write_regs(&imu->bus, imu->address, reg, &value, 1);

    if (rc == I2C_BB_ENOACK)
        errno = EREMOTEIO;
    else if (rc != I2C_BB_OK)
        errno = EIO;
    return (rc == I2C_BB_OK) ? 0 : -1;
}

static int read_regs(struct mpu6050_imu *imu,
                     uint8_t reg,
                     uint8_t *out,
                     size_t length)
{
    int rc = i2c_bb_read_regs(&imu->bus, imu->address, reg, out, length);

    if (rc == I2C_BB_ENOACK)
        errno = EREMOTEIO;
    else if (rc != I2C_BB_OK)
        errno = EIO;
    return (rc == I2C_BB_OK) ? 0 : -1;
}

int mpu6050_read_who_am_i(struct mpu6050_imu *imu, uint8_t *value)
{
    if (imu == NULL || value == NULL)
        return -1;
    return read_regs(imu, MPU6050_REG_WHO_AM_I, value, 1);
}

/*
 * The scanner's entry point, kept for interface compatibility with the old
 * /dev/i2c-N implementation. The board-side tool now owns the bus itself
 * (see tools/mpu6050-probe.c), so this is only reached by callers that
 * already hold an open device; the fd form made sense when the handle was a
 * file descriptor and does not carry over.
 */
int mpu6050_probe_address(int fd, uint8_t address)
{
    (void)fd;
    (void)address;
    errno = ENOSYS;
    return -1;
}

int mpu6050_open(const struct mpu6050_config *config, struct mpu6050_imu **imu)
{
    struct mpu6050_imu *self;
    uint8_t address;
    unsigned scl;
    unsigned sda;
    int accel_fsr;
    int gyro_fsr;

    if (imu == NULL)
        return -1;
    *imu = NULL;
    if (config == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * config->i2c_dev is ignored now that the transport is bit-banged, but the
     * field stays so existing callers keep compiling. The pin numbers come
     * from the config when given, so a different board layout needs no edit
     * here.
     */
    address = (config->address != 0) ? config->address
                                     : MPU6050_ADDR_AD0_LOW;
    accel_fsr = config->accel_fsr;
    gyro_fsr = config->gyro_fsr;
    scl = (config->scl_gpio != 0) ? config->scl_gpio
                                  : MPU6050_BITBANG_SCL_GPIO;
    sda = (config->sda_gpio != 0) ? config->sda_gpio
                                  : MPU6050_BITBANG_SDA_GPIO;

    if (mpu6050_accel_fsr_bits(accel_fsr) < 0)
        accel_fsr = MPU6050_FSR_ACCEL_2G;
    if (mpu6050_gyro_fsr_bits(gyro_fsr) < 0)
        gyro_fsr = MPU6050_FSR_GYRO_250;

    self = calloc(1, sizeof(*self));
    if (self == NULL)
        return -1;

    self->gpio = sysfs_gpio_open(scl, sda, MPU6050_BITBANG_HALF_PERIOD_US);
    if (self->gpio == NULL) {
        free(self);
        return -1;
    }

    if (i2c_bb_init(&self->bus, sysfs_gpio_ops(), self->gpio) != I2C_BB_OK) {
        sysfs_gpio_close(self->gpio);
        free(self);
        errno = EINVAL;
        return -1;
    }

    self->address = address;
    self->accel_fsr = accel_fsr;
    self->gyro_fsr = gyro_fsr;
    self->accel_scale = mpu6050_accel_scale(accel_fsr);
    self->gyro_scale = mpu6050_gyro_scale(gyro_fsr);

    /*
     * Unwedge first. If a previous run died mid-transaction the target may
     * still be holding SDA low, and every later access would then fail for a
     * reason that has nothing to do with this program.
     */
    if (i2c_bb_bus_recover(&self->bus) != I2C_BB_OK)
        goto fail;

    if (config->verify_who_am_i) {
        uint8_t who = 0;

        if (mpu6050_read_who_am_i(self, &who) != 0)
            goto fail;

        /*
         * Deliberately not `if (who != 0x68)`. The module on the bench
         * answered 0x70, which is an MPU6500: same output register layout,
         * different id. Rejecting anything but 0x68 turns a working sensor
         * into an unexplained ENODEV, which is exactly the kind of bug that
         * costs an afternoon. Accept the whole family and let a caller that
         * genuinely cares check the value itself.
         */
        if (!mpu6050_who_am_i_supported(who)) {
            errno = ENODEV;
            goto fail;
        }
    }

    /*
     * PWR_MGMT_1 bit 6 is SLEEP, set by default on power up. DEVICE_RESET
     * (bit 7) is deliberately not used here: it clears every register and
     * costs 100 ms, which is wasted work on a part we are about to configure
     * from scratch anyway. The low bits select the PLL, which is what the
     * gyro needs to be stable; the internal 8 MHz oscillator drifts too much.
     *
     * 0x01 rather than 0x00: on this module the register accepted the value
     * and read back correctly, and CLKSEL = 1 (PLL with X gyro reference) is
     * what the datasheet recommends for the best bias stability.
     */
    if (write_reg(self, MPU6050_REG_PWR_MGMT_1, 0x01) != 0)
        goto fail;

    /* DLPF before the sample rate, per the recommended init sequence. */
    if (write_reg(self, MPU6050_REG_CONFIG, config->dlpf & 0x07) != 0)
        goto fail;
    if (write_reg(self, MPU6050_REG_SMPLRT_DIV, config->sample_rate_div) != 0)
        goto fail;
    if (write_reg(self, MPU6050_REG_GYRO_CONFIG,
                  (uint8_t)mpu6050_gyro_fsr_bits(gyro_fsr)) != 0)
        goto fail;
    if (write_reg(self, MPU6050_REG_ACCEL_CONFIG,
                  (uint8_t)mpu6050_accel_fsr_bits(accel_fsr)) != 0)
        goto fail;

    /*
     * INT_PIN_CFG bit 1 (I2C_BYPASS_EN) must stay 0, and bits 7/6 default to
     * push-pull/active-high which we do not use. Writing 0 keeps the
     * auxiliary bus disabled so the part does not try to become an I2C master
     * on a bus that already has one.
     */
    if (write_reg(self, MPU6050_REG_INT_PIN_CFG, 0x00) != 0)
        goto fail;

    *imu = self;
    return 0;

fail:
    {
        int saved = errno;

        i2c_bb_release(&self->bus);
        sysfs_gpio_close(self->gpio);
        free(self);
        errno = saved;
    }
    return -1;
}

void mpu6050_close(struct mpu6050_imu *imu)
{
    if (imu == NULL)
        return;
    i2c_bb_release(&imu->bus);
    sysfs_gpio_close(imu->gpio);
    free(imu);
}

int mpu6050_read(struct mpu6050_imu *imu, struct mpu6050_sample *sample)
{
    uint8_t burst[MPU6050_BURST_LEN];
    struct timespec ts;

    if (imu == NULL || sample == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (read_regs(imu, MPU6050_BURST_START, burst, sizeof(burst)) != 0)
        return -1;

    if (mpu6050_decode_burst(burst, sizeof(burst), imu->accel_scale,
                             imu->gyro_scale, sample) != 0) {
        errno = EINVAL;
        return -1;
    }

    /*
     * CLOCK_MONOTONIC, not CLOCK_REALTIME: the timestamps end up next to the
     * video PTS, and neither should jump when NTP or the RTC steps the wall
     * clock.
     */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        sample->timestamp_us =
            (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
    } else {
        sample->timestamp_us = 0;
    }

    return 0;
}

/* Transport diagnostics, for the probe tool. */
unsigned mpu6050_transactions(const struct mpu6050_imu *imu)
{
    return (imu != NULL) ? imu->bus.transactions : 0;
}

unsigned mpu6050_ack_failures(const struct mpu6050_imu *imu)
{
    return (imu != NULL) ? imu->bus.ack_failures : 0;
}

unsigned mpu6050_io_errors(const struct mpu6050_imu *imu)
{
    return (imu != NULL) ? imu->bus.io_errors : 0;
}

#endif /* __linux__ */
