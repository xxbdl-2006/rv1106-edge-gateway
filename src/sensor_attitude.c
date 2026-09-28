#include "sensor_attitude.h"

#include "sensor_math.h"

/*
 * Radians to degrees as a multiply rather than a library call. 180/pi is not
 * exactly representable, so the constant is written to enough digits that the
 * float rounds to the nearest representable value - which is the best any
 * implementation can do, libm included.
 */
#define RAD_TO_DEG 57.2957795131f

/*
 * Below this magnitude we do not trust the direction of the vector.
 *
 * 0.05 g is far below the 1 g of a sensor sitting still and far above the noise
 * floor of a part that is merely quiet, so it separates "something is wrong or
 * this is in free fall" from "this is a real reading". The exact number matters
 * less than having one: without a floor, atan2 of two near-zero numbers still
 * returns a full-scale angle, so a disconnected part would report a confident
 * 90 degrees instead of "no idea".
 */
#define ATTITUDE_MIN_MAGNITUDE_G 0.05f

bool sensor_attitude_from_accel(const float accel_g[3],
                                float *pitch_deg,
                                float *roll_deg)
{
    float yz;
    float magnitude;

    if (accel_g == 0 || pitch_deg == 0 || roll_deg == 0)
        return false;

    magnitude = sensor_magnitude3(accel_g[0], accel_g[1], accel_g[2]);
    if (magnitude < ATTITUDE_MIN_MAGNITUDE_G)
        return false;

    /*
     * The YZ magnitude, not accel_z, for the pitch denominator. See the header:
     * this is what keeps pitch finite as the sensor goes vertical.
     */
    yz = sensor_magnitude3(0.0f, accel_g[1], accel_g[2]);
    if (yz < ATTITUDE_MIN_MAGNITUDE_G) {
        /*
         * The board is on its side: X carries essentially all of gravity. The
         * atan2 below would be atan2(~1, ~0), which is well defined (+-90), so
         * this is not strictly needed for pitch - but roll's atan2(accel_y,
         * accel_z) would be atan2(~0, ~0) and return 0, naming an arbitrary
         * direction. Returning false is more honest than a made-up roll.
         */
        return false;
    }

    *pitch_deg = sensor_atan2(-accel_g[0], yz) * RAD_TO_DEG;
    *roll_deg = sensor_atan2(accel_g[1], accel_g[2]) * RAD_TO_DEG;

    return true;
}

bool sensor_attitude_fill(struct sensor_sample *sample)
{
    float pitch = 0.0f;
    float roll = 0.0f;

    if (sample == 0)
        return false;

    sample->accel_magnitude_g =
        sensor_magnitude3(sample->accel_g[0],
                          sample->accel_g[1],
                          sample->accel_g[2]);

    if (!sensor_attitude_from_accel(sample->accel_g, &pitch, &roll)) {
        sample->pitch_deg = 0.0f;
        sample->roll_deg = 0.0f;
        sample->has_attitude = false;
        return false;
    }

    sample->pitch_deg = pitch;
    sample->roll_deg = roll;
    sample->has_attitude = true;
    return true;
}
