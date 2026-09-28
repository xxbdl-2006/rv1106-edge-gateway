#ifndef SENSOR_ATTITUDE_H
#define SENSOR_ATTITUDE_H

#include <stdbool.h>

#include "sensor_source.h"

/*
 * Turn an accelerometer reading into a pitch and a roll.
 *
 * This is the "which way is up" calculation, and it is worth being explicit
 * about what it can and cannot do, because the limitations are the reason the
 * struct carries a has_attitude flag rather than just two floats.
 *
 * What it does: with the part at rest, gravity points along whichever body axis
 * is currently vertical, and the direction of that 1 g vector, decomposed
 * against the other two axes, gives the tilt. Concretely, using the MPU6050
 * convention where Z is normal to the board:
 *
 *   pitch = atan2(-accel_x, sqrt(accel_y^2 + accel_z^2))
 *   roll  = atan2( accel_y, accel_z)
 *
 * The sqrt in pitch is what keeps it well behaved at the poles: feeding the
 * combined YZ magnitude instead of just accel_z means the argument never blows
 * up as the sensor approaches vertical, so pitch stays finite all the way to
 * +-90 rather than jumping through infinity.
 *
 * What it does NOT do:
 *
 *   - It cannot separate tilt from linear acceleration. A sensor on a table
 *     and a sensor being shoved sideways read similarly, because an
 *     accelerometer measures proper acceleration and cannot tell gravity from
 *     the push. Fixing that needs a complementary filter or an attitude
 *     estimator blending the gyro in, which is a later step. Until then this
 *     is correct for "at rest, how level is it", which is what the level
 *     display asks.
 *
 *   - It has no heading. Yaw is unobservable from an accelerometer alone; a
 *     single IMU without a magnetometer cannot know which way is north. That is
 *     physics, not a missing feature, so there is no yaw field to fill.
 *
 *   - It needs a trustworthy magnitude. A near-zero acceleration vector means
 *     free fall or a dead part, and atan2 of noise against noise would report a
 *     confident angle that means nothing. That case returns false and leaves
 *     the caller's sample alone rather than inventing a direction.
 *
 * accel_g is the calibrated three vector (bias removed, gravity preserved).
 * Returns true and fills pitch_deg / roll_deg / has_attitude on success, false
 * when the reading is not usable.
 */
bool sensor_attitude_from_accel(const float accel_g[3],
                                float *pitch_deg,
                                float *roll_deg);

/*
 * Fill pitch / roll / magnitude / has_attitude on a sample in place, using the
 * accelerometer reading already in it.
 *
 * Separate from the function above so the maths can be unit tested on bare
 * arrays without constructing a whole sample, and so a caller that has already
 * validated its inputs does not pay for the check twice. Returns true when an
 * attitude was produced.
 */
bool sensor_attitude_fill(struct sensor_sample *sample);

#endif /* SENSOR_ATTITUDE_H */
