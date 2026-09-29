#ifndef MPU6050_SOURCE_H
#define MPU6050_SOURCE_H

#include <stdint.h>

#include "mpu6050.h"
#include "sensor_source.h"

/*
 * The real IMU as a sensor_source.
 *
 * This is the piece that was missing: everything below it (bit-banged I2C,
 * register decoding, the frozen calibration constants) and everything above it
 * (the ring, the attitude solver, the OSD) already existed, and nothing joined
 * them. The join is deliberately thin - read a burst, calibrate it, fill in the
 * attitude - because every interesting decision is already made somewhere else
 * and a second place that made them would eventually disagree.
 *
 * What this layer owns, and why each one belongs here rather than above:
 *
 *  - The full scale range. It is not configurable. The calibration constants
 *    are raw counts and were measured at +/-2 g and +/-250 dps, so a source
 *    that could be pointed at another range would silently apply the right
 *    number of the wrong unit. Refusing the configuration is the only safe
 *    answer; mpu6050_apply_calibration() checks the same thing from the other
 *    end.
 *
 *  - The polling rate. One 14 byte burst takes about 28 ms of real time on
 *    this SoC - the sysfs GPIO bus toggles at a few kHz - and all of it is
 *    spent in the thread that also feeds the encoder. Measured on the board:
 *    the same 300 frame 720p encode ran at 30.001 fps with no overlay and at
 *    21.359 fps with this source polled at 100 Hz, dropping 122 frames busy.
 *    So the source limits how often it will touch the bus and answers
 *    "nothing new yet" in between. That is what the interface's return value
 *    of 0 is for: idle is not an error, and only this layer knows the
 *    difference for this device.
 *
 *  - The error count. Samples climbing while errors stay flat means the part
 *    is quiet; errors climbing means the bus is not working. The OSD prints
 *    the same number either way, so the distinction has to be made here.
 *
 * Everything below the #ifdef is Linux only (it calls mpu6050_open/read, which
 * are implemented in mpu6050_i2c.c and exist only there). The conversion is
 * not, which is why it is a separate function and why it can be unit tested on
 * the host: it is pure arithmetic over a decoded sample.
 */

struct mpu6050_sensor;

/*
 * Configuration for the source.
 *
 * Deliberately narrower than struct mpu6050_config: the pins because another
 * board may wire differently, the address because AD0 is a strap, the sample
 * rate and filter because they are genuine choices, and nothing else. In
 * particular there is no field for the measurement ranges - see above.
 */
struct mpu6050_source_config {
    /* GPIO numbers of the two bus lines. 0 uses the header 24/14 defaults. */
    unsigned scl_gpio;
    unsigned sda_gpio;
    /* 7-bit address. 0 uses MPU6050_ADDR_AD0_LOW. */
    uint8_t address;
    /*
     * Sample rate divider written to the part. 0 uses
     * MPU6050_SOURCE_DEFAULT_RATE_DIV. The rate is 1000/(1+div), so 9 is the
     * 100 Hz the sensor was characterised at.
     */
    uint8_t sample_rate_div;
    /* DLPF bandwidth, one of the MPU6050_DLPF_* constants. 0 uses 44 Hz. */
    uint8_t dlpf;
    /*
     * Non-zero (the default) refuses to open a part that does not answer with
     * a recognised id. Zero is for a board with nothing attached, where a
     * caller would rather have a source that reports errors than no source.
     */
    int verify_who_am_i;
    /*
     * Minimum time between bus transactions. 0 uses
     * MPU6050_SOURCE_DEFAULT_MIN_INTERVAL_US. Reads that arrive sooner return
     * 0 - idle, not error - without touching the bus.
     */
    uint64_t min_interval_us;
};

#define MPU6050_SOURCE_DEFAULT_RATE_DIV 9U      /* 100 Hz */

/*
 * 10 Hz, and deliberately not the 100 Hz the part itself samples at: the limit
 * is the bus, not the sensor. See the polling rate note at the top of this
 * file for the measurement that set it. A number drawn on a video frame does
 * not need to change faster than this anyway.
 */
#define MPU6050_SOURCE_DEFAULT_MIN_INTERVAL_US 100000ULL

/*
 * Open the part and wrap it in a source.
 *
 * Returns 0 on success, -1 with errno set on failure. ENODEV means the part did
 * not answer with a recognised WHO_AM_I; EPERM or EBUSY usually means the GPIO
 * pins could not be exported because something else holds them. The message is
 * left to the caller so this layer stays free of printf, like the rest of the
 * driver.
 *
 * config may be NULL, which is the bench configuration: default pins, 100 Hz,
 * 44 Hz filter, id verified.
 */
int mpu6050_source_open(const struct mpu6050_source_config *config,
                        struct mpu6050_sensor **sensor);

/* Release the part and the source. Safe on NULL. */
void mpu6050_source_close(struct mpu6050_sensor *sensor);

/* The interface view of an open sensor. */
struct sensor_source mpu6050_sensor_source(struct mpu6050_sensor *sensor);

/* Diagnostics: how many samples were produced, how many reads failed. */
unsigned long mpu6050_source_samples(const struct mpu6050_sensor *sensor);
unsigned long mpu6050_source_errors(const struct mpu6050_sensor *sensor);

/*
 * WHO_AM_I as read at open time, 0 if it could not be read. Reported rather
 * than enforced because the family shares one register layout: the part on this
 * bench answers 0x70 and reads perfectly good physics.
 */
uint8_t mpu6050_source_who_am_i(const struct mpu6050_sensor *sensor);

/*
 * Turn one decoded burst into a sensor_sample.
 *
 * Pure: no I/O, no Linux headers, host testable, which is the only way the
 * calibration's central claim - that |a| is 1.000 at rest and not 0.000 - gets
 * checked on every build instead of every few months when somebody puts the
 * board on a bench.
 *
 * accel_scale / gyro_scale must be the scales the sample was decoded with and
 * are checked against the range the biases were measured at. Returns 0 on
 * success, -1 on bad arguments or a range mismatch, leaving out untouched.
 *
 * pitch / roll come from the accelerometer through sensor_attitude_fill() and
 * are therefore relative to the orientation the part was in when the biases
 * were measured, not to world level. That is a property of a three-axis
 * accelerometer with no magnetometer and a single-position calibration, not a
 * rounding detail: a proper six-face tumble would separate tilt from offset,
 * and is not worth doing until the mount is final.
 */
int mpu6050_source_convert(const struct mpu6050_sample *raw,
                           float accel_scale,
                           float gyro_scale,
                           struct sensor_sample *out);

#endif /* MPU6050_SOURCE_H */
