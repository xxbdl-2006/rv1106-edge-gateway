/*
 * mpu6050-probe: find and identify an MPU6050 on an I2C bus.
 *
 * Written because the first question after wiring a sensor is always "is
 * anything actually there", and i2cdetect-style scanning is not available on
 * this rootfs. Two modes:
 *
 *   mpu6050-probe                 scan bus 4 for every address that answers
 *   mpu6050-probe --dump          scan, then read and print live samples
 *   mpu6050-probe --bus 4 --dump  same, explicit bus
 *
 * Exits non-zero when nothing is found, so the shell can branch on it.
 */

#include "mpu6050.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--bus N] [--addr 0x68|0x69] [--dump] [--count N]\n"
            "  --bus N      I2C bus number (default 4, i.e. /dev/i2c-4)\n"
            "  --addr A     only probe this 7-bit address\n"
            "  --dump       read and print samples after a successful probe\n"
            "  --count N    samples to print in --dump mode (default 10)\n",
            argv0);
}

/*
 * Walk every address the MPU6050 could plausibly be at, plus the usual
 * suspects, and report the ones that acknowledge.
 *
 * Every address is probed, not just 0x68/0x69, because a wrong AD0 strap is
 * the second most common wiring mistake and the scan output makes it obvious.
 * Addresses below 0x08 and above 0x77 are reserved by the I2C specification
 * and probing them can confuse other devices, so they are skipped.
 */
static int scan_bus(int fd, int only_address)
{
    int found = 0;

    printf("scanning bus for ACKs (0x08..0x77)...\n");
    for (int addr = 0x08; addr <= 0x77; addr++) {
        if (only_address >= 0 && addr != only_address)
            continue;

        int rc = mpu6050_probe_address(fd, (uint8_t)addr);
        if (rc == 1) {
            const char *note = "";
            if (addr == MPU6050_ADDR_AD0_LOW)
                note = "  <- MPU6050 with AD0 = low";
            else if (addr == MPU6050_ADDR_AD0_HIGH)
                note = "  <- MPU6050 with AD0 = high";

            printf("  0x%02X: present%s\n", addr, note);
            found++;
        } else if (rc == 0 && addr == only_address) {
            printf("  0x%02X: no response\n", addr);
        }
    }

    if (found == 0)
        printf("  nothing on the bus\n");
    return found;
}

static void dump_samples(struct mpu6050_imu *imu, int count)
{
    printf("\n#  accel[g]            gyro[dps]           temp[C]\n");
    for (int i = 0; i < count; i++) {
        struct mpu6050_sample s;

        if (mpu6050_read(imu, &s) != 0) {
            fprintf(stderr, "read failed: %s\n", strerror(errno));
            return;
        }
        printf("%2d  %+7.3f %+7.3f %+7.3f   %+8.2f %+8.2f %+8.2f   %6.2f\n",
               i,
               (double)s.accel_g[0], (double)s.accel_g[1],
               (double)s.accel_g[2],
               (double)s.gyro_dps[0], (double)s.gyro_dps[1],
               (double)s.gyro_dps[2],
               (double)s.temp_c);
        usleep(200000);
    }

    /*
     * Flat-on-the-table sanity: whichever way the board is sitting, exactly
     * one accelerometer axis should read close to +/-1 g. Reporting this here
     * saves a trip through a calculator.
     */
    struct mpu6050_sample s;
    if (mpu6050_read(imu, &s) == 0) {
        float mag = 0.0f;
        for (int i = 0; i < 3; i++)
            mag += s.accel_g[i] * s.accel_g[i];
        printf("\n|a| = %.3f g (expect ~1.000 when still)\n",
               (double)(mag > 0.0f ? mag : 0.0f));
    }
}

int main(int argc, char **argv)
{
    int bus = 4;
    int dump = 0;
    int count = 10;
    int only_address = -1;
    char path[64];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bus") == 0 && i + 1 < argc) {
            bus = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
            only_address = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--dump") == 0) {
            dump = 1;
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (bus < 0 || bus > 32) {
        fprintf(stderr, "bus %d is not a sane bus number\n", bus);
        return 2;
    }
    if (count < 1)
        count = 1;

    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        fprintf(stderr,
                "hint: the adapter is only present when the i2c controller is\n"
                "      enabled in the device tree. Check that the node is\n"
                "      'okay' and that the pinmux group is wired to the pins\n"
                "      you soldered to.\n");
        return 1;
    }

    printf("bus: %s (fd %d)\n", path, fd);
    int found = scan_bus(fd, only_address);
    close(fd);

    if (found == 0)
        return 1;

    if (dump) {
        struct mpu6050_config cfg;
        struct mpu6050_imu *imu = NULL;

        memset(&cfg, 0, sizeof(cfg));
        cfg.i2c_dev = path;
        cfg.address = (uint8_t)(only_address >= 0 ? only_address
                                                  : MPU6050_ADDR_AD0_LOW);
        cfg.accel_fsr = MPU6050_FSR_ACCEL_2G;
        cfg.gyro_fsr = MPU6050_FSR_GYRO_250;
        cfg.dlpf = MPU6050_DLPF_44HZ;
        cfg.sample_rate_div = 9; /* 100 Hz */
        cfg.verify_who_am_i = 0;

        if (mpu6050_open(&cfg, &imu) != 0) {
            fprintf(stderr, "open at 0x%02X failed: %s\n", cfg.address,
                    strerror(errno));
            return 1;
        }

        uint8_t who = 0;
        if (mpu6050_read_who_am_i(imu, &who) == 0)
            printf("\nWHO_AM_I = 0x%02X (expect 0x68)\n", who);

        dump_samples(imu, count);
        mpu6050_close(imu);
    }

    return 0;
}
