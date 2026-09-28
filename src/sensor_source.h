#ifndef SENSOR_SOURCE_H
#define SENSOR_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * One sensor reading, in engineering units, from whichever source produced it.
 *
 * This is the seam between "where the numbers come from" and "what we do with
 * them". The OSD, the alarm logic and the logger all consume this and none of
 * them care whether the numbers arrived over bit-banged I2C or out of a
 * waveform generator. That is the whole point of the interface: the mock source
 * exists so the entire data plane above the sensor can be built and tested
 * before the hardware is even powered, and so a regression in the OSD can be
 * told apart from a regression in the driver.
 *
 * Units follow the roadmap and the MPU6050 driver:
 *   accel in g, gyro in deg/s, angles in degrees.
 *
 * pitch / roll are filled by the attitude layer, not by the source. A source
 * that cannot compute them (the mock's raw modes, a future magnetometer-less
 * setup) leaves them at 0 and sets has_attitude = false, so a consumer can tell
 * "flat and level" apart from "not measured" instead of displaying 0.0 for both.
 */
struct sensor_sample {
    uint64_t timestamp_us;
    float accel_g[3];
    float gyro_dps[3];
    float temperature_c;
    /* Rotation about the sensor X axis (nose up/down), degrees. */
    float pitch_deg;
    /* Rotation about the sensor Y axis (left/right roll), degrees. */
    float roll_deg;
    /* True when pitch_deg / roll_deg were computed rather than left at zero. */
    bool has_attitude;
    /*
     * Magnitude of the acceleration vector. At rest this is 1 g; a value far
     * from it means the part is moving, or the scale is wrong, or the
     * calibration ate the gravity. Kept in the sample because both the OSD and
     * the alarm path need it and recomputing it in each is how the two drift.
     */
    float accel_magnitude_g;
};

/*
 * A sensor data source.
 *
 * Deliberately tiny: open once, read samples, close. There is no "configure"
 * or "set rate" here because every implementation needs different knobs, and
 * a lowest-common-denominator config struct would be all the wrong fields for
 * whoever ended up reading it. Configuration belongs to the concrete open()
 * (see mock_sensor_open, mpu6050_source_open).
 *
 * read() returns the number of samples written (0 or 1 today, but the
 * signature allows a source that batches), or -1 on error. A source that is
 * merely idle - no new sample yet - returns 0 and is not an error: the caller
 * polls again. This is the distinction that keeps a slow sensor from looking
 * like a broken one.
 */
struct sensor_source {
    /* Human readable name for logs, e.g. "mock" or "mpu6050". */
    const char *name;

    /*
     * Fill `out`. Returns 1 when a sample was produced, 0 when none is ready
     * yet, -1 on error (with errno set; the caller owns the message, this
     * layer stays free of printf like the rest of the driver code).
     * `out` is untouched when the return value is not 1.
     */
    int (*read)(void *context, struct sensor_sample *out);

    /* Release resources. Safe to call on a NULL context. */
    void (*close)(void *context);

    /*
     * How many samples this source failed to produce, for diagnostics.
     *
     * A source whose read_errors climbs while samples stay flat cannot talk to
     * its sensor; one with samples climbing and errors flat is merely quiet.
     * Those two look identical to a caller that only counts samples, which is
     * exactly the confusion these counters exist to remove.
     */
    unsigned long (*read_errors)(void *context);

    void *context;
};

#endif /* SENSOR_SOURCE_H */
