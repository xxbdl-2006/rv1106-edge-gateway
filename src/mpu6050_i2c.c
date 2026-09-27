/*
 * MPU6050 transport: the only file in this driver that touches /dev/i2c-N.
 *
 * Kept separate from mpu6050.c so the decoding stays host-testable. This file
 * is inherently Linux-only; on Windows it compiles to nothing, which lets the
 * Makefile list it unconditionally without breaking `make test`.
 */

#include "mpu6050.h"

#ifdef __linux__

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/*
 * EREMOTEIO is the errno a Linux I2C adapter returns when a slave did not
 * acknowledge. It is spelled EREMOTEIO in glibc but some uclibc
 * configurations do not export it, so it is defined here as a fallback rather
 * than allowed to break the build. The value 121 is the Linux ABI value and
 * is stable, so the fallback is not a guess.
 */
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif

struct mpu6050_imu {
    int fd;
    uint8_t address;
    float accel_scale;
    float gyro_scale;
    int accel_fsr;
    int gyro_fsr;
};

/*
 * Check that the adapter can do raw slave transfers.
 *
 * I2C_FUNCS is the documented capability query, but the macro is missing from
 * some uclibc headers, so it is probed with #ifdef rather than assumed. When
 * the adapter cannot be queried the code proceeds optimistically: the read
 * and write paths below fail cleanly anyway, and refusing to open a bus just
 * because it cannot describe itself would be worse than trying.
 */
static int check_adapter_caps(int fd)
{
#ifdef I2C_FUNCS
    unsigned long funcs = 0;

    if (ioctl(fd, I2C_FUNCS, &funcs) < 0)
        return 0; /* cannot tell; let the real transfers decide */

    /*
     * SMBus byte-level access is what the probe write relies on. Adapters
     * that lack it still support plain read/write in slave mode, so this is
     * reported through errno only when the basic capability is absent too.
     */
    if (!(funcs & (I2C_FUNC_I2C | I2C_FUNC_SMBUS_WRITE_BYTE))) {
        errno = ENOSYS;
        return -1;
    }
#else
    (void)fd;
#endif
    return 0;
}

/* Force the adapter into slave mode and select the device address. */
static int select_slave(int fd, uint8_t address)
{
    if (ioctl(fd, I2C_SLAVE, (long)address) < 0) {
        /*
         * I2C_SLAVE fails with EBUSY when the driver already owns the address.
         * I2C_SLAVE_FORCE is the documented escape hatch, and some adapters
         * only implement that one.
         */
        if (errno == EBUSY) {
            if (ioctl(fd, I2C_SLAVE_FORCE, (long)address) < 0)
                return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

static int write_reg(struct mpu6050_imu *imu, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };

    if (select_slave(imu->fd, imu->address) < 0)
        return -1;
    if (write(imu->fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
        return -1;
    return 0;
}

static int read_regs(struct mpu6050_imu *imu,
                     uint8_t reg,
                     uint8_t *out,
                     size_t length)
{
    if (select_slave(imu->fd, imu->address) < 0)
        return -1;

    /*
     * Write the register pointer without a stop condition where the adapter
     * supports it. A plain write followed by a plain read is accepted by the
     * MPU6050 as well, because it tolerates a stop between the two phases.
     */
    if (write(imu->fd, &reg, 1) != 1)
        return -1;
    if (read(imu->fd, out, length) != (ssize_t)length)
        return -1;
    return 0;
}

int mpu6050_read_who_am_i(struct mpu6050_imu *imu, uint8_t *value)
{
    if (imu == NULL || value == NULL)
        return -1;
    return read_regs(imu, MPU6050_REG_WHO_AM_I, value, 1);
}

int mpu6050_probe_address(int fd, uint8_t address)
{
    /*
     * An SMBus quick write is one byte on the bus and no data. The MPU6050
     * does not care about the payload, so a plain one byte write is a valid
     * presence probe, and it avoids depending on I2C_RDWR iovec support.
     */
    if (select_slave(fd, address) < 0)
        return (errno == ENODEV || errno == EREMOTEIO) ? 0 : -1;

    uint8_t dummy = 0;
    if (write(fd, &dummy, 1) != 1) {
        if (errno == ENXIO || errno == EREMOTEIO)
            return 0; /* nobody answered: absent */
        return -1;
    }
    return 1;
}

int mpu6050_open(const struct mpu6050_config *config, struct mpu6050_imu **imu)
{
    struct mpu6050_imu *self;
    const char *path;
    uint8_t address;
    int accel_fsr;
    int gyro_fsr;

    if (imu == NULL)
        return -1;
    *imu = NULL;
    if (config == NULL) {
        errno = EINVAL;
        return -1;
    }

    path = (config->i2c_dev != NULL) ? config->i2c_dev
                                     : MPU6050_I2C_DEV_DEFAULT;
    address = (config->address != 0) ? config->address
                                     : MPU6050_ADDR_AD0_LOW;
    accel_fsr = config->accel_fsr;
    gyro_fsr = config->gyro_fsr;

    if (mpu6050_accel_fsr_bits(accel_fsr) < 0)
        accel_fsr = MPU6050_FSR_ACCEL_2G;
    if (mpu6050_gyro_fsr_bits(gyro_fsr) < 0)
        gyro_fsr = MPU6050_FSR_GYRO_250;

    self = calloc(1, sizeof(*self));
    if (self == NULL)
        return -1;

    self->fd = open(path, O_RDWR);
    if (self->fd < 0) {
        free(self);
        return -1;
    }
    self->address = address;
    self->accel_fsr = accel_fsr;
    self->gyro_fsr = gyro_fsr;
    self->accel_scale = mpu6050_accel_scale(accel_fsr);
    self->gyro_scale = mpu6050_gyro_scale(gyro_fsr);

    if (check_adapter_caps(self->fd) != 0)
        goto fail;

    if (config->verify_who_am_i) {
        uint8_t who = 0;
        if (mpu6050_read_who_am_i(self, &who) != 0)
            goto fail;
        if (who != MPU6050_WHO_AM_I_VALUE) {
            errno = ENODEV;
            goto fail;
        }
    }

    /*
     * PWR_MGMT_1 bit 6 is SLEEP, set by default on power up. DEVICE_RESET
     * (bit 7) is deliberately not used here: it clears every register and
     * costs 100 ms, which is wasted work on a part we are about to configure
     * from scratch anyway. Bit 0 selects the PLL, which is what the gyro
     * needs to be stable; the internal 8 MHz oscillator drifts too much.
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
     * INT_PIN_CFG bit 1 (I2C_BYPASS_EN) must stay 0, and BITS 7/6 default to
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
        close(self->fd);
        free(self);
        errno = saved;
    }
    return -1;
}

void mpu6050_close(struct mpu6050_imu *imu)
{
    if (imu == NULL)
        return;
    if (imu->fd >= 0)
        close(imu->fd);
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

int mpu6050_fd(const struct mpu6050_imu *imu)
{
    return (imu != NULL) ? imu->fd : -1;
}

#endif /* __linux__ */
