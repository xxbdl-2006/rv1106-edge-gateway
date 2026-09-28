/*
 * mpu6050-probe: find, identify and read an MPU6050 over a bit-banged I2C bus.
 *
 * Written because the first question after wiring a sensor is always "is
 * anything actually there", and i2cdetect-style scanning is not available on
 * this rootfs for a bus the kernel does not know about.
 *
 * Modes:
 *
 *   mpu6050-probe                   scan 0x08..0x77 for anything that answers
 *   mpu6050-probe --dump            scan, then read and print live samples
 *   mpu6050-probe --addr 0x68       probe one address only
 *   mpu6050-probe --scl 70 --sda 71 override the pins
 *   mpu6050-probe --delay 10        faster half period, in microseconds
 *
 * Exits non-zero when nothing responds, so a shell script can branch on it.
 *
 * Why bit-bang rather than /dev/i2c-N: the i2c3 controller serving header pins
 * 24/14 is disabled in this board's device tree, and the runtime overlay that
 * would enable it does not work here. See src/i2c_bitbang.h and
 * dts/i2c3-enable.dts. A disabled controller leaves its pins unclaimed, which
 * is what makes driving them from userspace possible.
 */

#include "mpu6050.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gpio_sysfs.h"
#include "i2c_bitbang.h"

/* ------------------------------------------------------------------------- */

struct probe {
    struct sysfs_gpio *gpio;
    struct i2c_bitbang bus;
};

static int probe_open(struct probe *p, unsigned scl, unsigned sda,
                      unsigned delay_us)
{
    p->gpio = sysfs_gpio_open(scl, sda, delay_us);
    if (p->gpio == NULL)
        return -1;

    if (i2c_bb_init(&p->bus, sysfs_gpio_ops(), p->gpio) != I2C_BB_OK) {
        sysfs_gpio_close(p->gpio);
        p->gpio = NULL;
        return -1;
    }
    return 0;
}

static void probe_close(struct probe *p)
{
    if (p->gpio != NULL) {
        i2c_bb_release(&p->bus);
        sysfs_gpio_close(p->gpio);
        p->gpio = NULL;
    }
}

/*
 * Walk every address an I2C device could legally occupy and report the ones
 * that acknowledge.
 *
 * The whole range, not just 0x68/0x69, because a wrong AD0 strap is the second
 * most common wiring mistake and a full scan makes it obvious at a glance.
 * Addresses below 0x08 and above 0x77 are reserved and probing them can upset
 * other devices, so they are skipped.
 */
static int scan_bus(struct probe *p, int only_address)
{
    int found = 0;

    printf("scanning 0x08..0x77 for ACKs\n");

    /*
     * Unwedge first. If a previous run died mid-transaction a target may still
     * be holding SDA low, and without this every address would look absent.
     */
    if (i2c_bb_bus_recover(&p->bus) != I2C_BB_OK)
        printf("  warning: bus recovery reported an error\n");

    for (int addr = 0x08; addr <= 0x77; addr++) {
        int rc;

        if (only_address >= 0 && addr != only_address)
            continue;

        rc = i2c_bb_probe_address(&p->bus, (uint8_t)addr);
        if (rc == 1) {
            const char *note = "";

            if (addr == MPU6050_ADDR_AD0_LOW)
                note = "   <- MPU6050-family, AD0 = low";
            else if (addr == MPU6050_ADDR_AD0_HIGH)
                note = "   <- MPU6050-family, AD0 = high";

            printf("  0x%02X: present%s\n", addr, note);
            found++;
        } else if (rc < 0) {
            /*
             * A transport failure is reported rather than silently treated as
             * "absent". The two need different fixes: nobody home means look
             * at the wiring, while an I/O error means look at the pins and
             * their permissions.
             */
            printf("  0x%02X: transport error (%d)\n", addr, rc);
        }
    }

    if (found == 0)
        printf("  nothing responds\n");

    printf("  %u transaction(s), %u nack(s), %u io error(s)\n",
           p->bus.transactions, p->bus.ack_failures, p->bus.io_errors);
    return found;
}

static void print_sample_row(int index, const struct mpu6050_sample *s)
{
    printf("%2d  %+7.3f %+7.3f %+7.3f   %+8.2f %+8.2f %+8.2f   %6.2f\n",
           index,
           (double)s->accel_g[0], (double)s->accel_g[1], (double)s->accel_g[2],
           (double)s->gyro_dps[0], (double)s->gyro_dps[1],
           (double)s->gyro_dps[2],
           (double)s->temp_c);
}

static int dump_samples(struct mpu6050_imu *imu, int count)
{
    double sum_sq = 0.0;

    printf("\n #  accel[g]            gyro[dps]           temp[C]\n");
    for (int i = 0; i < count; i++) {
        struct mpu6050_sample s;

        if (mpu6050_read(imu, &s) != 0) {
            fprintf(stderr, "read failed: %s\n", strerror(errno));
            return -1;
        }
        print_sample_row(i, &s);

        for (int a = 0; a < 3; a++)
            sum_sq += (double)s.accel_g[a] * (double)s.accel_g[a];

        usleep(200000);
    }

    /*
     * The magnitude, not the sum of squares. Whichever way the board is
     * sitting, the three accelerometer axes must combine to about 1 g because
     * gravity is the only steady acceleration present. This is the single most
     * useful sanity check on a newly wired sensor: it exercises the full
     * register map, the byte order and the scale factor at once, and the
     * expected answer is known without any calibration.
     *
     * Note the square root. Reporting the sum of squares under a "|a|" label
     * reads as 1.05 g on a healthy sensor and looks plausible, which is
     * exactly why the mistake survives review.
     */
    {
        double mean = sum_sq / (double)count;
        double mag;

        /*
         * Square root by Newton iteration rather than sqrt(), so the probe
         * needs no -lm and stays linkable on a rootfs that ships only libc.
         * Guard the zero case: starting from 0 would divide by zero on the
         * first step, and a sensor returning all zeros is exactly the failure
         * this check is meant to report.
         */
        if (mean <= 0.0) {
            mag = 0.0;
        } else {
            mag = mean;
            for (int i = 0; i < 20; i++)
                mag = 0.5 * (mag + mean / mag);
        }

        printf("\n|a| = %.3f g (expect ~1.000 when the sensor is still)\n", mag);
        if (mag < 0.7 || mag > 1.3) {
            printf("      -> outside the expected band; check the range setting\n"
                   "         and that the part is not being shaken\n");
            return -1;
        }
    }
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --scl N      SCL gpio number (default 70, header pin 24)\n"
            "  --sda N      SDA gpio number (default 71, header pin 14)\n"
            "  --delay N    half period in microseconds (default 25)\n"
            "  --addr A     probe only this 7-bit address\n"
            "  --dump       read and print samples after a successful probe\n"
            "  --count N    samples to print in --dump mode (default 10)\n",
            argv0);
}

int main(int argc, char **argv)
{
    struct probe p;
    unsigned scl = MPU6050_SCL_GPIO_DEFAULT;
    unsigned sda = MPU6050_SDA_GPIO_DEFAULT;
    unsigned delay_us = 25;
    int dump = 0;
    int count = 10;
    int only_address = -1;
    int found;
    int exit_code = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scl") == 0 && i + 1 < argc) {
            scl = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--sda") == 0 && i + 1 < argc) {
            sda = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--delay") == 0 && i + 1 < argc) {
            delay_us = (unsigned)strtoul(argv[++i], NULL, 0);
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

    if (only_address >= 0 && (only_address < 0x08 || only_address > 0x77)) {
        fprintf(stderr, "address 0x%02X is outside the 7-bit range\n",
                only_address);
        return 2;
    }
    if (count < 1)
        count = 1;
    if (delay_us == 0)
        delay_us = 25;

    if (probe_open(&p, scl, sda, delay_us) != 0) {
        fprintf(stderr, "cannot set up gpio %u (SCL) and %u (SDA): %s\n",
                scl, sda, strerror(errno));
        fprintf(stderr,
                "hint: check /sys/class/gpio, and that the pins are not already\n"
                "      claimed by a driver. 'grep -E \"pin (NN|MM) \"\n"
                "      /sys/kernel/debug/pinctrl/*/pinmux-pins' should say\n"
                "      MUX UNCLAIMED for both.\n");
        return 1;
    }

    printf("bus: SCL=gpio%u  SDA=gpio%u  half period=%u us\n",
           scl, sda, delay_us);

    found = scan_bus(&p, only_address);
    if (found == 0) {
        probe_close(&p);
        return 1;
    }

    if (!dump) {
        probe_close(&p);
        return 0;
    }

    {
        struct mpu6050_config cfg;
        struct mpu6050_imu *imu = NULL;
        uint8_t who = 0;

        memset(&cfg, 0, sizeof(cfg));
        cfg.scl_gpio = scl;
        cfg.sda_gpio = sda;
        cfg.address = (uint8_t)(only_address >= 0 ? only_address
                                                  : MPU6050_ADDR_AD0_LOW);
        cfg.accel_fsr = MPU6050_FSR_ACCEL_2G;
        cfg.gyro_fsr = MPU6050_FSR_GYRO_250;
        cfg.dlpf = MPU6050_DLPF_44HZ;
        cfg.sample_rate_div = 9; /* 100 Hz */
        cfg.verify_who_am_i = 1;

        /*
         * The scanner above already owns the pins, so close it first: two
         * handles on the same sysfs attributes would fight over direction.
         */
        probe_close(&p);

        if (mpu6050_open(&cfg, &imu) != 0) {
            fprintf(stderr, "open at 0x%02X failed: %s\n", cfg.address,
                    strerror(errno));
            return 1;
        }

        if (mpu6050_read_who_am_i(imu, &who) == 0) {
            const char *name = mpu6050_who_am_i_name(who);

            printf("\nWHO_AM_I = 0x%02X", who);
            if (name != NULL)
                printf(" (%s)", name);
            printf("\n");
            /*
             * Deliberately a report, not a rejection. 0x68 is the original
             * part, but modules ship with whatever was available and the one
             * on this bench answers 0x70 while producing perfect data. The
             * output layout is the same across the family, so only the id
             * differs and that is worth saying out loud rather than failing on.
             */
            if (who != MPU6050_WHO_AM_I_VALUE)
                printf("           not the 0x68 of a stock MPU6050, but the\n"
                       "           output registers are layout-compatible\n");
        }

        if (dump_samples(imu, count) != 0)
            exit_code = 1;

        mpu6050_close(imu);
    }

    return exit_code;
}
