/*
 * The real IMU as a sensor_source.
 *
 * Two halves, split the way the driver below it is split:
 *
 *   mpu6050_source_convert()   arithmetic over a decoded sample, no I/O,
 *                              builds and is tested on the host.
 *   everything else            opens the part, reads it, closes it. Linux
 *                              only, because mpu6050_open/read live in
 *                              mpu6050_i2c.c and exist only there.
 *
 * The split is not tidiness for its own sake. The conversion carries the one
 * fact about this sensor that is expensive to get wrong and invisible when it
 * is wrong - that at rest the calibrated vector is 1 g long, not 0 g - and it
 * is the half that can be checked without a board. The other half is a dozen
 * lines of glue over a transport that is already verified on the bench.
 */

#include "mpu6050_source.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sensor_attitude.h"

/* ------------------------------------------------------------------------ */
/* Conversion: host testable, no I/O.                                        */
/* ------------------------------------------------------------------------ */

int mpu6050_source_convert(const struct mpu6050_sample *raw,
                           float accel_scale,
                           float gyro_scale,
                           struct sensor_sample *out)
{
    struct mpu6050_calibrated calibrated;

    if (raw == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Zeroed before the call that can fail, so a caller that ignores the
     * return value sees an empty sample and not the previous one's numbers.
     * "No reading" and "the last reading" must never look the same: an OSD
     * showing a stale angle as if it were fresh is worse than one showing
     * nothing, and the feed layer cannot tell them apart once they have been
     * mixed.
     */
    memset(out, 0, sizeof(*out));

    /*
     * The scales are passed in rather than assumed, even though this source
     * only ever configures one range. If they are ever wrong the calibration
     * refuses them, and a refusal here is far cheaper than a plausible-looking
     * 0.98 g that came from subtracting a 2 g bias out of a 4 g reading.
     */
    if (mpu6050_apply_calibration(raw, accel_scale, gyro_scale,
                                  &calibrated) != 0) {
        errno = EINVAL;
        return -1;
    }

    out->timestamp_us = calibrated.timestamp_us;
    out->temperature_c = calibrated.temp_c;
    out->accel_magnitude_g = calibrated.accel_magnitude_g;

    memcpy(out->accel_g, calibrated.accel_g, sizeof(out->accel_g));
    memcpy(out->gyro_dps, calibrated.gyro_dps, sizeof(out->gyro_dps));

    /*
     * Attitude last, from the calibrated vector. Gravity is still in it by
     * design: that 1 g is the measurement the angles are derived from, and a
     * calibration that removed it would leave pitch and roll computed from
     * noise.
     */
    (void)sensor_attitude_fill(out);

    return 0;
}

/* ------------------------------------------------------------------------ */
/* Transport: Linux only.                                                    */
/* ------------------------------------------------------------------------ */

#ifdef __linux__

#include <time.h>

struct mpu6050_sensor {
    struct mpu6050_imu *imu;
    float accel_scale;
    float gyro_scale;
    uint8_t who_am_i;

    /*
     * Rate limiting. The bus is driven from the same thread that feeds the
     * encoder, and one bit-banged burst costs a few milliseconds, so the
     * source will not start a new transaction until this interval has passed.
     * Reads inside the window return 0 without touching the part: to the feed
     * that is "no new sample yet", which is exactly what it is.
     */
    uint64_t min_interval_us;
    uint64_t last_read_us;

    unsigned long samples;
    unsigned long errors;
};

static uint64_t now_monotonic_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

int mpu6050_source_open(const struct mpu6050_source_config *config,
                        struct mpu6050_sensor **sensor)
{
    struct mpu6050_config device_config;
    struct mpu6050_sensor *self;

    if (sensor == NULL) {
        errno = EINVAL;
        return -1;
    }
    *sensor = NULL;

    memset(&device_config, 0, sizeof(device_config));
    device_config.scl_gpio = (config != NULL) ? config->scl_gpio : 0U;
    device_config.sda_gpio = (config != NULL) ? config->sda_gpio : 0U;
    device_config.address = (config != NULL) ? config->address : 0U;
    device_config.dlpf = (config != NULL) ? config->dlpf : (uint8_t)MPU6050_DLPF_44HZ;
    device_config.sample_rate_div =
        (config != NULL && config->sample_rate_div != 0U)
            ? config->sample_rate_div
            : (uint8_t)MPU6050_SOURCE_DEFAULT_RATE_DIV;

    /*
     * The ranges are not taken from the caller. See the header: the biases are
     * raw counts measured at this range and mean nothing at another one.
     */
    device_config.accel_fsr = MPU6050_CAL_ACCEL_FSR;
    device_config.gyro_fsr = MPU6050_CAL_GYRO_FSR;

    /*
     * Verified by default. Skipping the check is what turns "nothing is
     * connected" into an hour of reading plausible zeroes, so it has to be
     * asked for explicitly.
     */
    device_config.verify_who_am_i =
        (config == NULL || config->verify_who_am_i != 0) ? 1 : 0;

    self = calloc(1, sizeof(*self));
    if (self == NULL) {
        errno = ENOMEM;
        return -1;
    }

    if (mpu6050_open(&device_config, &self->imu) != 0) {
        int saved = errno;

        free(self);
        errno = saved;
        return -1;
    }

    self->accel_scale = mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR);
    self->gyro_scale = mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR);
    self->min_interval_us = (config != NULL && config->min_interval_us != 0U)
                                ? config->min_interval_us
                                : MPU6050_SOURCE_DEFAULT_MIN_INTERVAL_US;

    /*
     * Read the id even when it was already checked during open, so the caller
     * can log which part this is. A failure here is not fatal: the part is
     * already talking, and refusing to run because an optional identification
     * read failed would be inventing a fault.
     */
    {
        uint8_t who = 0;

        if (mpu6050_read_who_am_i(self->imu, &who) == 0)
            self->who_am_i = who;
    }

    *sensor = self;
    return 0;
}

void mpu6050_source_close(struct mpu6050_sensor *sensor)
{
    if (sensor == NULL)
        return;

    /*
     * mpu6050_close() releases the pins as well as the handle. That matters
     * beyond tidiness: a run that left them exported would keep the next one
     * from exporting them at all, and the symptom would be a bus that appears
     * dead for a reason in a process that has already exited.
     */
    mpu6050_close(sensor->imu);
    free(sensor);
}

static int mpu6050_source_read(void *context, struct sensor_sample *out)
{
    struct mpu6050_sensor *self = context;
    struct mpu6050_sample raw;
    uint64_t now;

    if (self == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Rate limit before touching the bus. Note this is checked against the
     * last attempt, not the last success: a part that has stopped answering
     * must not be hammered at the poll rate, and a failure is not a reason to
     * spend more time on the bus than a success would.
     */
    now = now_monotonic_us();
    if (self->last_read_us != 0U &&
        now - self->last_read_us < self->min_interval_us) {
        return 0;
    }
    self->last_read_us = now;

    if (mpu6050_read(self->imu, &raw) != 0) {
        self->errors++;
        return -1;
    }

    if (mpu6050_source_convert(&raw, self->accel_scale, self->gyro_scale,
                               out) != 0) {
        self->errors++;
        return -1;
    }

    self->samples++;
    return 1;
}

static void mpu6050_source_close_adapter(void *context)
{
    mpu6050_source_close(context);
}

static unsigned long mpu6050_source_error_adapter(void *context)
{
    struct mpu6050_sensor *self = context;
    return (self != NULL) ? self->errors : 0UL;
}

struct sensor_source mpu6050_sensor_source(struct mpu6050_sensor *sensor)
{
    struct sensor_source source;

    memset(&source, 0, sizeof(source));
    source.name = "mpu6050";
    source.read = mpu6050_source_read;
    source.close = mpu6050_source_close_adapter;
    source.read_errors = mpu6050_source_error_adapter;
    source.context = sensor;
    return source;
}

unsigned long mpu6050_source_samples(const struct mpu6050_sensor *sensor)
{
    return (sensor != NULL) ? sensor->samples : 0UL;
}

unsigned long mpu6050_source_errors(const struct mpu6050_sensor *sensor)
{
    return (sensor != NULL) ? sensor->errors : 0UL;
}

uint8_t mpu6050_source_who_am_i(const struct mpu6050_sensor *sensor)
{
    return (sensor != NULL) ? sensor->who_am_i : 0U;
}

#endif /* __linux__ */
