/*
 * Host side self test for the real IMU data source.
 *
 * Built and run natively (no board, no SDK):
 *
 *     make test-mpu6050-source
 *
 * Only the conversion half is covered here. The other half - open, read, close
 * - calls mpu6050_open/read, which exist only on Linux, so it is verified on
 * the bench instead. That is the same split the driver itself uses and for the
 * same reason: the transport is a dozen lines over an already verified bus,
 * while the conversion carries the one claim about this sensor that is cheap
 * to get wrong and impossible to see when it is wrong.
 *
 * The claim is this: at rest, after calibration, the acceleration vector is
 * 1 g long. Not 0 g - that is the classic blunder, subtracting the raw mean
 * and cancelling gravity along with the offset - and not 1.06 g, which is what
 * the uncalibrated part reads because the module is leaning on its header
 * pins. Both wrong answers come out of the same function, both look plausible,
 * and neither is visible in a running system: the OSD would show a confident
 * "0.0 g" or a confident "1.06 g" and nothing would complain. So they are
 * pinned here, on every build, with no board in the loop.
 *
 * The biases are not repeated in this file. They come from mpu6050.h, because
 * a copy in a test would keep compiling long after it stopped matching the
 * constants it is supposed to be checking.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mpu6050.h"
#include "mpu6050_source.h"
#include "sensor_source.h"

static int g_checks;
static int g_failures;

#define CHECK(condition, ...)                                              \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(condition)) {                                                \
            g_failures++;                                                  \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

static int close_to(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

/*
 * The scales the real source configures. Written as calls rather than literals
 * so that changing the calibration range in mpu6050.h changes this test too -
 * a test that hardcodes 1/16384 would keep passing against a part configured
 * for 4 g, which is exactly the mismatch the range check exists to catch.
 */
#define CAL_ACCEL_SCALE mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR)
#define CAL_GYRO_SCALE mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR)

/*
 * Build a decoded sample from counts as the part would report them: the
 * argument is the *calibrated* count the test wants to end up with, and the
 * frozen bias is added back on the way in, so a test thinks in terms of the
 * physics it wants rather than in terms of raw register values.
 */
static void make_sample(const float counts[3],
                        const float gyro_counts[3],
                        int16_t temp_raw,
                        uint64_t timestamp_us,
                        struct mpu6050_sample *out)
{
    uint8_t burst[MPU6050_BURST_LEN];
    size_t at = 0;
    int16_t words[7];

    words[0] = (int16_t)(counts[0] + MPU6050_ACCEL_BIAS_X);
    words[1] = (int16_t)(counts[1] + MPU6050_ACCEL_BIAS_Y);
    words[2] = (int16_t)(counts[2] + MPU6050_ACCEL_BIAS_Z);
    words[3] = temp_raw;
    words[4] = (int16_t)(gyro_counts[0] + MPU6050_GYRO_BIAS_X);
    words[5] = (int16_t)(gyro_counts[1] + MPU6050_GYRO_BIAS_Y);
    words[6] = (int16_t)(gyro_counts[2] + MPU6050_GYRO_BIAS_Z);

    /*
     * Big endian, high byte first, which is how the part answers and the only
     * sane way to check the decoder on a little endian host.
     */
    for (int i = 0; i < 7; i++) {
        uint16_t u = (uint16_t)words[i];

        burst[at++] = (uint8_t)(u >> 8);
        burst[at++] = (uint8_t)(u & 0xFFu);
    }

    if (mpu6050_decode_burst(burst, sizeof(burst), CAL_ACCEL_SCALE,
                             CAL_GYRO_SCALE, out) != 0) {
        printf("  FAIL decode_burst returned -1\n");
        g_failures++;
    }
    out->timestamp_us = timestamp_us;
}

/* ------------------------------------------------------------------------ */

/*
 * The bench case: the part sitting still in the orientation the biases were
 * measured in, reporting the means that produced them.
 */
static void test_convert_at_rest(void)
{
    const float counts[3] = {0.0f, 0.0f, 16384.0f};
    const float gyro[3] = {0.0f, 0.0f, 0.0f};
    struct mpu6050_sample raw;
    struct sensor_sample out;

    make_sample(counts, gyro, 3882, 1234567ULL, &raw);

    printf("convert: at rest, raw |a| = %.4f g (uncalibrated, the module leans)\n",
           (double)sqrtf(raw.accel_g[0] * raw.accel_g[0] +
                         raw.accel_g[1] * raw.accel_g[1] +
                         raw.accel_g[2] * raw.accel_g[2]));

    CHECK(mpu6050_source_convert(&raw, CAL_ACCEL_SCALE, CAL_GYRO_SCALE,
                                 &out) == 0,
          "convert rejected a valid sample");

    printf("convert: at rest, calibrated |a| = %.4f g, pitch %.3f roll %.3f\n",
           (double)out.accel_magnitude_g, (double)out.pitch_deg,
           (double)out.roll_deg);

    /*
     * The whole point of the file. 1.000 plus or minus the noise, and
     * deliberately not 0.000: an accelerometer at rest is measuring gravity,
     * and a calibration that removes it has removed the measurement.
     */
    CHECK(close_to(out.accel_magnitude_g, 1.0f, 0.02f),
          "|a| = %.4f g, expected 1.000 +/- 0.02", (double)out.accel_magnitude_g);
    CHECK(out.accel_magnitude_g > 0.5f,
          "|a| = %.4f g: the bias ate the gravity", (double)out.accel_magnitude_g);

    /*
     * Level, because the biases were measured in this orientation. This is
     * zero-by-construction rather than a coincidence of the constants, and it
     * is worth asserting: a bias set that left a residual tilt would show up
     * here as a confident non-zero angle on a part nobody moved.
     */
    CHECK(close_to(out.pitch_deg, 0.0f, 0.5f), "pitch = %.3f deg, expected 0",
          (double)out.pitch_deg);
    CHECK(close_to(out.roll_deg, 0.0f, 0.5f), "roll = %.3f deg, expected 0",
          (double)out.roll_deg);
    CHECK(out.has_attitude, "has_attitude is false for a 1 g reading");

    /*
     * A gyro at rest reads zero, so its calibrated output must be zero. Not
     * merely small: the biases are its raw mean, so anything left over is a
     * rounding error, and a large residual means the wrong constant is being
     * subtracted.
     */
    for (int i = 0; i < 3; i++) {
        CHECK(fabsf(out.gyro_dps[i]) < 0.1f, "gyro[%d] = %.4f dps at rest",
              i, (double)out.gyro_dps[i]);
    }

    /* Temperature passes through untouched: it has no at-rest offset. */
    CHECK(close_to(out.temperature_c, 47.95f, 1.0f),
          "temperature = %.2f C, expected about 47.95", (double)out.temperature_c);

    /* The timestamp is the sample's, not a fresh one. */
    CHECK(out.timestamp_us == 1234567ULL, "timestamp = %llu, expected 1234567",
          (unsigned long long)out.timestamp_us);
}

/*
 * A known tilt. 30 degrees is far enough from zero that a sign error or a
 * swapped axis cannot hide inside the tolerance, and the magnitude is still
 * exactly 1 g, which separates "the angles are right" from "the vector is the
 * right length".
 */
static void test_convert_known_tilt(void)
{
    const float counts[3] = {-8192.0f, 0.0f, 14189.0f}; /* -0.5 g, 0, +0.866 g */
    const float gyro[3] = {0.0f, 0.0f, 0.0f};
    struct mpu6050_sample raw;
    struct sensor_sample out;

    make_sample(counts, gyro, 3882, 0ULL, &raw);

    CHECK(mpu6050_source_convert(&raw, CAL_ACCEL_SCALE, CAL_GYRO_SCALE,
                                 &out) == 0,
          "convert rejected a tilted sample");

    printf("convert: 30 deg tilt -> pitch %.3f roll %.3f |a| %.4f\n",
           (double)out.pitch_deg, (double)out.roll_deg,
           (double)out.accel_magnitude_g);

    CHECK(close_to(out.pitch_deg, 30.0f, 0.5f), "pitch = %.3f deg, expected 30",
          (double)out.pitch_deg);
    CHECK(close_to(out.roll_deg, 0.0f, 0.5f), "roll = %.3f deg, expected 0",
          (double)out.roll_deg);
    CHECK(close_to(out.accel_magnitude_g, 1.0f, 0.02f),
          "|a| = %.4f g, expected 1.000", (double)out.accel_magnitude_g);
    CHECK(out.has_attitude, "has_attitude is false for a tilted 1 g reading");
}

/*
 * The uncalibrated magnitude, printed and bounded.
 *
 * It is about 1.062 g, and that is not an error: the breakout leans by
 * asin(2639/16384) = 9.3 degrees on the header pins, and a single-position
 * calibration cannot tell a lean from an offset. Recording the number here
 * means a future change to the constants that shrank this to 1.000 would be
 * visible as a failing test instead of as a slightly different angle nobody
 * noticed - the calibration has to leave the lean alone, or the part stops
 * agreeing with itself when it is moved.
 */
static void test_uncalibrated_magnitude_is_the_lean(void)
{
    const float counts[3] = {0.0f, 0.0f, 16384.0f};
    const float gyro[3] = {0.0f, 0.0f, 0.0f};
    struct mpu6050_sample raw;
    float magnitude;

    make_sample(counts, gyro, 3882, 0ULL, &raw);
    magnitude = sqrtf(raw.accel_g[0] * raw.accel_g[0] +
                      raw.accel_g[1] * raw.accel_g[1] +
                      raw.accel_g[2] * raw.accel_g[2]);

    printf("convert: uncalibrated |a| = %.4f g (bounded, not asserted exact)\n",
           (double)magnitude);

    CHECK(magnitude > 1.03f && magnitude < 1.09f,
          "uncalibrated |a| = %.4f g, expected about 1.062", (double)magnitude);
    CHECK(magnitude > 1.02f,
          "uncalibrated |a| = %.4f g: below 1.02 would mean the raw means "
          "already had gravity removed", (double)magnitude);
}

/*
 * A dead or free-falling part: no magnitude, so no attitude.
 *
 * This is the case the has_attitude flag exists for. atan2 of two near-zero
 * numbers still returns a confident angle, so without the check a disconnected
 * sensor would display a steady 90 degrees - indistinguishable, on screen,
 * from a board that was genuinely on its side.
 */
static void test_no_attitude_for_a_dead_part(void)
{
    const float counts[3] = {0.0f, 0.0f, 0.0f};
    const float gyro[3] = {0.0f, 0.0f, 0.0f};
    struct mpu6050_sample raw;
    struct sensor_sample out;

    make_sample(counts, gyro, 3882, 0ULL, &raw);

    CHECK(mpu6050_source_convert(&raw, CAL_ACCEL_SCALE, CAL_GYRO_SCALE,
                                 &out) == 0,
          "convert rejected a zero reading");

    printf("convert: zero vector -> |a| %.4f has_attitude %d\n",
           (double)out.accel_magnitude_g, (int)out.has_attitude);

    CHECK(!out.has_attitude, "has_attitude is true for a zero vector");
    CHECK(out.pitch_deg == 0.0f, "pitch = %.3f on a zero vector",
          (double)out.pitch_deg);
    CHECK(out.roll_deg == 0.0f, "roll = %.3f on a zero vector",
          (double)out.roll_deg);
}

/*
 * A range the biases were not measured at must be refused, not quietly
 * applied. The failure has to be loud because the alternative is a plausible
 * number: subtracting a 2 g bias from a 4 g reading gives 0.98 g, which sits
 * inside every tolerance above and is wrong.
 */
static void test_wrong_range_is_refused(void)
{
    const float counts[3] = {0.0f, 0.0f, 16384.0f};
    const float gyro[3] = {0.0f, 0.0f, 0.0f};
    struct mpu6050_sample raw;
    struct sensor_sample out;

    make_sample(counts, gyro, 3882, 0ULL, &raw);

    memset(&out, 0x5A, sizeof(out));
    CHECK(mpu6050_source_convert(&raw, mpu6050_accel_scale(MPU6050_FSR_ACCEL_4G),
                                 CAL_GYRO_SCALE, &out) == -1,
          "convert accepted a 4 g scale against 2 g biases");
    CHECK(mpu6050_source_convert(&raw, CAL_ACCEL_SCALE,
                                 mpu6050_gyro_scale(MPU6050_FSR_GYRO_500),
                                 &out) == -1,
          "convert accepted a 500 dps scale against 250 dps biases");

    /*
     * Refused means untouched - and specifically not left holding the previous
     * sample. The sentinel above was 0x5A5A5A5A, so a zero here is proof the
     * field was cleared rather than preserved.
     */
    CHECK(out.accel_g[0] == 0.0f,
          "out was modified on a refused conversion: accel_g[0] = %f",
          (double)out.accel_g[0]);
    CHECK(out.timestamp_us == 0U, "out was modified on a refused conversion");
}

static void test_argument_checks(void)
{
    const float counts[3] = {0.0f, 0.0f, 16384.0f};
    const float gyro[3] = {0.0f, 0.0f, 0.0f};
    struct mpu6050_sample raw;
    struct sensor_sample out;

    make_sample(counts, gyro, 3882, 0ULL, &raw);

    CHECK(mpu6050_source_convert(NULL, CAL_ACCEL_SCALE, CAL_GYRO_SCALE,
                                 &out) == -1,
          "convert accepted a NULL sample");
    CHECK(mpu6050_source_convert(&raw, CAL_ACCEL_SCALE, CAL_GYRO_SCALE,
                                 NULL) == -1,
          "convert accepted a NULL output");
}

int main(void)
{
    printf("mpu6050 source: conversion and calibration\n");

    test_convert_at_rest();
    test_convert_known_tilt();
    test_uncalibrated_magnitude_is_the_lean();
    test_no_attitude_for_a_dead_part();
    test_wrong_range_is_refused();
    test_argument_checks();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
