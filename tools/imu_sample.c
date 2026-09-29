/*
 * Read the real IMU through the sensor_source interface and print what it says.
 *
 * Board side only (cross compiled, driven from scripts/verify-imu.sh):
 *
 *     imu-sample --seconds 10
 *
 * Why this exists when tools/mpu6050-probe.c already talks to the part: the
 * probe exercises the driver, this exercises the *seam* - the same
 * sensor_source the OSD consumes. A driver that reads correctly and a source
 * that feeds the wrong numbers into the overlay are different failures, and
 * only one of them is visible in the other tool.
 *
 * It answers three questions, which are also the acceptance criteria:
 *
 *   1. Is the part really there, and does it say so?        (WHO_AM_I line)
 *   2. Does it run at rate with no bus errors?               (per second line)
 *   3. Is a part sitting still reading 1 g, not 0 g?         (the |a| line)
 *
 * The third is the one that cannot be checked by looking at the video. A
 * calibration that cancelled gravity produces a perfectly plausible overlay -
 * level, quiet, confident - and is wrong, so it is printed as a number here
 * with the expected value next to it.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mpu6050.h"
#include "mpu6050_source.h"
#include "sensor_source.h"

#define DEFAULT_SECONDS 10U
#define DEFAULT_PERIOD_US 10000UL /* 100 Hz */

static uint64_t now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/*
 * Absolute deadlines rather than a relative sleep: the cumulative drift of
 * sleeping "the remaining time" after variable-length work is what turns a
 * nominal 100 Hz into 97 Hz over a minute, and the rate is one of the things
 * this tool reports.
 */
static void sleep_until(uint64_t deadline_us)
{
    struct timespec ts;
    uint64_t now = now_us();

    if (now >= deadline_us)
        return;

    ts.tv_sec = (time_t)((deadline_us - now) / 1000000ULL);
    ts.tv_nsec = (long)((deadline_us - now) % 1000000ULL) * 1000L;
    (void)nanosleep(&ts, NULL);
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --seconds N     how long to sample (default: %u)\n"
            "  --period-us N   poll period in microseconds (default: %lu)\n"
            "  --no-verify     open even if WHO_AM_I is unrecognised\n"
            "  -h, --help      show this help\n",
            program, DEFAULT_SECONDS, DEFAULT_PERIOD_US);
}

int main(int argc, char **argv)
{
    struct mpu6050_source_config config;
    struct mpu6050_sensor *sensor = NULL;
    struct sensor_source source;
    struct sensor_sample last;
    uint64_t start_us;
    uint64_t deadline_us;
    uint64_t next_tick_us;
    uint64_t next_report_us;
    unsigned seconds = DEFAULT_SECONDS;
    unsigned long period_us = DEFAULT_PERIOD_US;
    unsigned long reads = 0;
    unsigned long samples = 0;
    unsigned long idle = 0;
    unsigned long errors = 0;
    unsigned long second_marks = 0;
    int have_sample = 0;
    int verify = 1;

    memset(&config, 0, sizeof(config));
    memset(&last, 0, sizeof(last));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--period-us") == 0 && i + 1 < argc) {
            period_us = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--no-verify") == 0) {
            verify = 0;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "imu-sample: unknown argument '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    if (seconds == 0U || period_us == 0UL) {
        fprintf(stderr, "imu-sample: seconds and period must be non-zero\n");
        return 2;
    }

    config.verify_who_am_i = verify;

    if (mpu6050_source_open(&config, &sensor) != 0) {
        fprintf(stderr, "imu-sample: cannot open the IMU: %s\n", strerror(errno));
        return 1;
    }
    source = mpu6050_sensor_source(sensor);

    printf("imu-sample: source=%s WHO_AM_I=0x%02X (%s), period=%lu us for %u s\n",
           source.name, (unsigned)mpu6050_source_who_am_i(sensor),
           mpu6050_who_am_i_name(mpu6050_source_who_am_i(sensor))
               ? mpu6050_who_am_i_name(mpu6050_source_who_am_i(sensor))
               : "unrecognised",
           period_us, seconds);
    fflush(stdout);

    start_us = now_us();
    deadline_us = start_us + (uint64_t)seconds * 1000000ULL;
    next_tick_us = start_us;
    next_report_us = start_us + 1000000ULL;

    while (now_us() < deadline_us) {
        struct sensor_sample sample;
        int result;

        sleep_until(next_tick_us);
        next_tick_us += period_us;

        reads++;
        result = source.read(source.context, &sample);

        if (result == 1) {
            samples++;
            last = sample;
            have_sample = 1;
        } else if (result == 0) {
            /*
             * Idle is not a fault. The source rate limits itself so the video
             * thread is not spending its frame budget on the bus, and a poll
             * that lands inside that window legitimately has nothing new.
             */
            idle++;
        } else {
            errors++;
        }

        if (now_us() >= next_report_us) {
            second_marks++;
            printf("  t=%lus reads=%lu samples=%lu idle=%lu errors=%lu%s\n",
                   second_marks, reads, samples, idle, errors,
                   have_sample ? "" : " (no reading yet)");
            if (have_sample) {
                printf("    |a|=%.3f g pitch=%.2f roll=%.2f temp=%.2f C\n",
                       (double)last.accel_magnitude_g, (double)last.pitch_deg,
                       (double)last.roll_deg, (double)last.temperature_c);
            }
            fflush(stdout);
            next_report_us += 1000000ULL;
        }
    }

    printf("imu-sample: total reads=%lu samples=%lu idle=%lu errors=%lu\n",
           reads, samples, idle, errors);

    if (samples == 0UL) {
        fprintf(stderr, "imu-sample: FAIL - no samples in %u s\n", seconds);
        source.close(source.context);
        return 1;
    }

    if (errors != 0UL) {
        fprintf(stderr, "imu-sample: FAIL - %lu read errors\n", errors);
        source.close(source.context);
        return 1;
    }

    printf("imu-sample: last sample |a|=%.3f g pitch=%.2f roll=%.2f "
           "temp=%.2f C\n",
           (double)last.accel_magnitude_g, (double)last.pitch_deg,
           (double)last.roll_deg, (double)last.temperature_c);
    printf("imu-sample: gyro=%.2f/%.2f/%.2f dps (expect about 0 at rest)\n",
           (double)last.gyro_dps[0], (double)last.gyro_dps[1],
           (double)last.gyro_dps[2]);

    /*
     * The magnitude is printed with its expectation next to it rather than
     * silently judged. 1.000 at rest, and the two ways of getting it wrong
     * have opposite signatures worth naming: 0.000 means the bias ate the
     * gravity, 1.062 means no calibration reached this sample at all.
     */
    if (last.accel_magnitude_g < 0.98f || last.accel_magnitude_g > 1.02f) {
        fprintf(stderr,
                "imu-sample: FAIL - |a|=%.3f g at rest, expected 1.000 "
                "(0.000 means the bias ate the gravity, 1.062 means no "
                "calibration)\n",
                (double)last.accel_magnitude_g);
        source.close(source.context);
        return 1;
    }

    printf("imu-sample: PASS - %lu samples, 0 errors, |a|=%.3f g\n",
           samples, (double)last.accel_magnitude_g);

    /*
     * Closing releases the GPIO pins. Not optional: a run that left them
     * exported would stop the next run from exporting them, and the failure
     * would appear in a different process from the one that caused it.
     */
    source.close(source.context);
    return 0;
}
