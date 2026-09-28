/*
 * Host tests for the MPU6050 decoding layer.
 *
 * Runs on Windows against src/mpu6050.c, which is why that file has no Linux
 * dependency. The transport half (src/mpu6050_i2c.c) is not covered here --
 * it cannot be, without a bus -- so the split is what makes these tests
 * meaningful rather than decorative.
 *
 * A real 14 byte burst captured from a stationary part is used as the anchor
 * case, so a scaling regression shows up as a physically wrong number rather
 * than as a changed constant.
 */

#include "mpu6050.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* Float compare with a tolerance, because these are scaled sensor values. */
static int close_to(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

static void put_be16(uint8_t *p, int16_t value)
{
    uint16_t u = (uint16_t)value;
    p[0] = (uint8_t)(u >> 8);
    p[1] = (uint8_t)(u & 0xFF);
}

/* Build a burst from raw counts, honouring the register order. */
static void make_burst(uint8_t *burst,
                       int16_t ax, int16_t ay, int16_t az,
                       int16_t temp,
                       int16_t gx, int16_t gy, int16_t gz)
{
    put_be16(&burst[0], ax);
    put_be16(&burst[2], ay);
    put_be16(&burst[4], az);
    put_be16(&burst[6], temp);
    put_be16(&burst[8], gx);
    put_be16(&burst[10], gy);
    put_be16(&burst[12], gz);
}

/* ------------------------------------------------------------------------ */

static void test_scales(void)
{
    printf("scales\n");

    /* Datasheet sensitivity values, expressed as g-per-LSB. */
    CHECK(close_to(mpu6050_accel_scale(MPU6050_FSR_ACCEL_2G),
                   1.0f / 16384.0f, 1e-9f), "+/-2g scale");
    CHECK(close_to(mpu6050_accel_scale(MPU6050_FSR_ACCEL_4G),
                   1.0f / 8192.0f, 1e-9f), "+/-4g scale");
    CHECK(close_to(mpu6050_accel_scale(MPU6050_FSR_ACCEL_8G),
                   1.0f / 4096.0f, 1e-9f), "+/-8g scale");
    CHECK(close_to(mpu6050_accel_scale(MPU6050_FSR_ACCEL_16G),
                   1.0f / 2048.0f, 1e-9f), "+/-16g scale");

    CHECK(close_to(mpu6050_gyro_scale(MPU6050_FSR_GYRO_250),
                   1.0f / 131.0f, 1e-9f), "+/-250 dps scale");
    CHECK(close_to(mpu6050_gyro_scale(MPU6050_FSR_GYRO_500),
                   1.0f / 65.5f, 1e-9f), "+/-500 dps scale");
    CHECK(close_to(mpu6050_gyro_scale(MPU6050_FSR_GYRO_1000),
                   1.0f / 32.8f, 1e-9f), "+/-1000 dps scale");
    CHECK(close_to(mpu6050_gyro_scale(MPU6050_FSR_GYRO_2000),
                   1.0f / 16.4f, 1e-9f), "+/-2000 dps scale");

    /* Out of range must be rejected, not silently clamped. */
    CHECK(mpu6050_accel_scale(-1) == 0.0f, "bad accel fsr -> 0");
    CHECK(mpu6050_accel_scale(4) == 0.0f, "accel fsr 4 -> 0");
    CHECK(mpu6050_gyro_scale(-1) == 0.0f, "bad gyro fsr -> 0");
    CHECK(mpu6050_gyro_scale(4) == 0.0f, "gyro fsr 4 -> 0");
}

static void test_fsr_bits(void)
{
    printf("fsr register bits\n");

    /* AFS_SEL / FS_SEL live in bits 4:3. */
    CHECK(mpu6050_accel_fsr_bits(MPU6050_FSR_ACCEL_2G) == 0x00, "2g bits");
    CHECK(mpu6050_accel_fsr_bits(MPU6050_FSR_ACCEL_4G) == 0x08, "4g bits");
    CHECK(mpu6050_accel_fsr_bits(MPU6050_FSR_ACCEL_8G) == 0x10, "8g bits");
    CHECK(mpu6050_accel_fsr_bits(MPU6050_FSR_ACCEL_16G) == 0x18, "16g bits");

    CHECK(mpu6050_gyro_fsr_bits(MPU6050_FSR_GYRO_250) == 0x00, "250 bits");
    CHECK(mpu6050_gyro_fsr_bits(MPU6050_FSR_GYRO_500) == 0x08, "500 bits");
    CHECK(mpu6050_gyro_fsr_bits(MPU6050_FSR_GYRO_1000) == 0x10, "1000 bits");
    CHECK(mpu6050_gyro_fsr_bits(MPU6050_FSR_GYRO_2000) == 0x18, "2000 bits");

    CHECK(mpu6050_accel_fsr_bits(-1) == -1, "accel -1 rejected");
    CHECK(mpu6050_accel_fsr_bits(4) == -1, "accel 4 rejected");
    CHECK(mpu6050_gyro_fsr_bits(-1) == -1, "gyro -1 rejected");
    CHECK(mpu6050_gyro_fsr_bits(4) == -1, "gyro 4 rejected");
}

static void test_decode_flat(void)
{
    printf("decode: flat on the table\n");

    /*
     * A part sitting flat, +Z up. At +/-2g and +/-250 dps the raw values are
     * 16384 on Z and zero elsewhere. This is the case a user meets first, and
     * the numbers are physically checkable: 1.000 g on Z, 0 dps everywhere.
     */
    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;

    make_burst(burst, 0, 0, 16384, 0, 0, 0, 0);
    CHECK(mpu6050_decode_burst(burst, sizeof(burst),
                               mpu6050_accel_scale(MPU6050_FSR_ACCEL_2G),
                               mpu6050_gyro_scale(MPU6050_FSR_GYRO_250),
                               &s) == 0, "decode returns 0");

    CHECK(s.accel_raw[0] == 0 && s.accel_raw[1] == 0,
          "accel x/y raw zero");
    CHECK(s.accel_raw[2] == 16384, "accel z raw 16384, got %d", s.accel_raw[2]);
    CHECK(close_to(s.accel_g[2], 1.0f, 1e-4f),
          "accel z = 1.000 g, got %f", (double)s.accel_g[2]);
    CHECK(close_to(s.accel_g[0], 0.0f, 1e-6f), "accel x = 0 g");
    CHECK(close_to(s.gyro_dps[0], 0.0f, 1e-6f), "gyro x = 0 dps");
}

static void test_decode_signs(void)
{
    printf("decode: negative values\n");

    /*
     * Negative counts must survive the big-endian read. A uint16 read that
     * forgets the sign produces 32767-ish here instead of -1, which is the
     * classic version of this bug.
     */
    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;

    make_burst(burst, -1, -16384, -32768, -100, -131, -65, -16400);
    CHECK(mpu6050_decode_burst(burst, sizeof(burst),
                               mpu6050_accel_scale(MPU6050_FSR_ACCEL_2G),
                               mpu6050_gyro_scale(MPU6050_FSR_GYRO_250),
                               &s) == 0, "decode returns 0");

    CHECK(s.accel_raw[0] == -1, "accel x raw -1, got %d", s.accel_raw[0]);
    CHECK(s.accel_raw[1] == -16384, "accel y raw -16384, got %d",
          s.accel_raw[1]);
    CHECK(s.accel_raw[2] == -32768, "accel z raw -32768, got %d",
          s.accel_raw[2]);
    CHECK(s.gyro_raw[0] == -131, "gyro x raw -131, got %d", s.gyro_raw[0]);

    /* -16384 / 16384 = exactly -1.0 g. */
    CHECK(close_to(s.accel_g[1], -1.0f, 1e-4f),
          "accel y = -1.000 g, got %f", (double)s.accel_g[1]);
    /* -131 / 131 = exactly -1.0 dps. */
    CHECK(close_to(s.gyro_dps[0], -1.0f, 1e-4f),
          "gyro x = -1.000 dps, got %f", (double)s.gyro_dps[0]);
}

static void test_decode_temperature(void)
{
    printf("decode: temperature\n");

    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;

    /*
     * The register map gives T[C] = raw / 340 + 36.53. Room temperature is
     * therefore raw ~= 0 for 36.53 C and raw = 340 for 37.53 C.
     */
    make_burst(burst, 0, 0, 0, 0, 0, 0, 0);
    CHECK(mpu6050_decode_burst(burst, sizeof(burst), 1.0f, 1.0f, &s) == 0,
          "decode returns 0");
    CHECK(close_to(s.temp_c, 36.53f, 1e-3f),
          "raw 0 -> 36.53 C, got %f", (double)s.temp_c);

    make_burst(burst, 0, 0, 0, 340, 0, 0, 0);
    CHECK(mpu6050_decode_burst(burst, sizeof(burst), 1.0f, 1.0f, &s) == 0,
          "decode returns 0");
    CHECK(close_to(s.temp_c, 37.53f, 1e-3f),
          "raw 340 -> 37.53 C, got %f", (double)s.temp_c);

    /* Negative raw, i.e. a cold part. */
    make_burst(burst, 0, 0, 0, -340, 0, 0, 0);
    CHECK(mpu6050_decode_burst(burst, sizeof(burst), 1.0f, 1.0f, &s) == 0,
          "decode returns 0");
    CHECK(close_to(s.temp_c, 35.53f, 1e-3f),
          "raw -340 -> 35.53 C, got %f", (double)s.temp_c);
}

static void test_axis_order(void)
{
    printf("decode: register order\n");

    /*
     * Guards the byte offsets themselves. Each axis is a distinct power of
     * two so a swap or an off-by-one slot shows up immediately.
     */
    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;

    make_burst(burst, 1, 2, 4, 8, 16, 32, 64);
    CHECK(mpu6050_decode_burst(burst, sizeof(burst), 1.0f, 1.0f, &s) == 0,
          "decode returns 0");

    CHECK(s.accel_raw[0] == 1, "accel x offset");
    CHECK(s.accel_raw[1] == 2, "accel y offset");
    CHECK(s.accel_raw[2] == 4, "accel z offset");
    CHECK(s.temp_raw == 8, "temp offset, got %d", s.temp_raw);
    CHECK(s.gyro_raw[0] == 16, "gyro x offset, got %d", s.gyro_raw[0]);
    CHECK(s.gyro_raw[1] == 32, "gyro y offset");
    CHECK(s.gyro_raw[2] == 64, "gyro z offset");
}

static void test_full_scale_saturation(void)
{
    printf("decode: saturation and range changes\n");

    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;

    /*
     * At +/-16g the same raw count means eight times less acceleration than
     * at +/-2g. Getting this backwards is easy and silently produces a
     * physically plausible but wrong number.
     */
    make_burst(burst, 2048, 0, 0, 0, 0, 0, 0);

    CHECK(mpu6050_decode_burst(burst, sizeof(burst),
                               mpu6050_accel_scale(MPU6050_FSR_ACCEL_16G),
                               1.0f, &s) == 0, "decode 16g");
    CHECK(close_to(s.accel_g[0], 1.0f, 1e-4f),
          "2048 counts at +/-16g = 1.000 g, got %f", (double)s.accel_g[0]);

    CHECK(mpu6050_decode_burst(burst, sizeof(burst),
                               mpu6050_accel_scale(MPU6050_FSR_ACCEL_2G),
                               1.0f, &s) == 0, "decode 2g");
    CHECK(close_to(s.accel_g[0], 0.125f, 1e-4f),
          "2048 counts at +/-2g = 0.125 g, got %f", (double)s.accel_g[0]);

    /* Gyro at +/-2000 dps: 16400 counts is ~1000 dps. */
    make_burst(burst, 0, 0, 0, 0, 16400, 0, 0);
    CHECK(mpu6050_decode_burst(burst, sizeof(burst), 1.0f,
                               mpu6050_gyro_scale(MPU6050_FSR_GYRO_2000),
                               &s) == 0, "decode 2000dps");
    CHECK(close_to(s.gyro_dps[0], 1000.0f, 0.1f),
          "16400 counts at +/-2000dps = 1000 dps, got %f",
          (double)s.gyro_dps[0]);
}

static void test_rejects_short_burst(void)
{
    printf("decode: bad input\n");

    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;

    make_burst(burst, 0, 0, 16384, 0, 0, 0, 0);

    /* A short read means the transfer was truncated; refuse rather than
     * decode whatever happened to be in the buffer. */
    CHECK(mpu6050_decode_burst(burst, MPU6050_BURST_LEN - 1, 1.0f, 1.0f,
                               &s) == -1, "13 bytes rejected");
    CHECK(mpu6050_decode_burst(burst, 0, 1.0f, 1.0f, &s) == -1,
          "0 bytes rejected");
    CHECK(mpu6050_decode_burst(NULL, sizeof(burst), 1.0f, 1.0f, &s) == -1,
          "NULL burst rejected");
    CHECK(mpu6050_decode_burst(burst, sizeof(burst), 1.0f, 1.0f, NULL) == -1,
          "NULL sample rejected");

    /* The buffer is accepted once it is long enough again. */
    CHECK(mpu6050_decode_burst(burst, sizeof(burst), 1.0f, 1.0f, &s) == 0,
          "14 bytes accepted");
}

static void test_zero_sample_on_failure(void)
{
    printf("decode: failure clears the sample\n");

    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;

    make_burst(burst, 123, 456, 789, 101, 112, 131, 415);

    /* Poison the struct, then force a failure: it must come back zeroed so a
     * caller that ignores the return value cannot read stale values. */
    s.accel_raw[0] = 9999;
    s.temp_c = 99.0f;
    CHECK(mpu6050_decode_burst(burst, 3, 1.0f, 1.0f, &s) == -1,
          "short burst rejected");
    CHECK(s.accel_raw[0] == 0, "sample zeroed on failure, got %d",
          s.accel_raw[0]);
    CHECK(close_to(s.temp_c, 0.0f, 1e-9f), "temperature zeroed on failure");
}

static void test_giant_values(void)
{
    printf("decode: extreme scale factors\n");

    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;

    /* A zero scale is what an invalid FSR produces. It must not be silently
     * treated as "fine", so the decode still reports what it read while the
     * scaled values collapse to zero. */
    make_burst(burst, 1000, 2000, 3000, 400, 5000, 6000, 7000);
    CHECK(mpu6050_decode_burst(burst, sizeof(burst), 0.0f, 0.0f, &s) == 0,
          "zero scale still decodes");
    CHECK(s.accel_raw[0] == 1000, "raw survives zero scale");
    CHECK(close_to(s.accel_g[0], 0.0f, 1e-9f), "zero scale -> 0 g");
}

/* ------------------------------------------------------------------------ */
/* Calibration                                                               */
/* ------------------------------------------------------------------------ */

/*
 * Build a sample for a level part: the given acceleration along Z, nothing on
 * X and Y.
 *
 * This exists because writing it inline three times got it wrong three times.
 * "No acceleration on X" is not a raw count of zero: the part outputs its
 * offset when at rest, so a level axis reads MPU6050_ACCEL_BIAS_X counts. A
 * zero here would mean +0.16 g of real motion on this particular sensor, and
 * the resulting magnitude assertions would fail for a reason that has nothing
 * to do with the code under test.
 */
static void make_level_sample(uint8_t *burst, float z_g)
{
    int16_t ax = (int16_t)(MPU6050_ACCEL_BIAS_X + (MPU6050_ACCEL_BIAS_X < 0 ? -0.5f : 0.5f));
    int16_t ay = (int16_t)(MPU6050_ACCEL_BIAS_Y + (MPU6050_ACCEL_BIAS_Y < 0 ? -0.5f : 0.5f));
    int16_t az = (int16_t)(z_g * 16384.0f + MPU6050_ACCEL_BIAS_Z + 0.5f);

    make_burst(burst, ax, ay, az, 3882, 0, 0, 0);
}

/*
 * The anchor case: the exact 100-sample mean the committed biases came from,
 * fed back through the correction. A correct implementation must land on
 * zero for every axis of the gyro, because a part at rest has no rotation and
 * the whole mean was therefore offset.
 */
static void test_calibration_removes_gyro_bias(void)
{
    printf("calib: gyro at rest cancels\n");

    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;
    struct mpu6050_calibrated c;

    make_burst(burst, -2639, -67, 17195, 3882, 553, -612, -240);
    CHECK(mpu6050_decode_burst(burst, sizeof(burst),
                               mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                               mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR),
                               &s) == 0, "decode ok");
    CHECK(mpu6050_apply_calibration(&s,
                                    mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                                    mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR),
                                    &c) == 0, "apply ok");

    /* Raw counts were integers rounded from the mean, so allow the LSB of
     * that rounding: 0.36 counts and a fraction of a dps. */
    CHECK(close_to(c.gyro_dps[0], 0.0f, 0.01f), "gyro x -> 0, got %f",
          c.gyro_dps[0]);
    CHECK(close_to(c.gyro_dps[1], 0.0f, 0.01f), "gyro y -> 0, got %f",
          c.gyro_dps[1]);
    CHECK(close_to(c.gyro_dps[2], 0.0f, 0.01f), "gyro z -> 0, got %f",
          c.gyro_dps[2]);
}

/*
 * The mistake this test exists to catch, and it is a subtle one because it
 * passes every other check: subtracting the raw accelerometer mean instead of
 * the mean minus gravity. The gyro case above cannot see it - gyro really does
 * read zero at rest - and the magnitude check would still be near 1.0 if only
 * one axis were wrong in a compensating way.
 *
 * The input has to be what the part actually reports at rest, not a round
 * number: a level part reads 16384 counts of gravity *plus* its own offset, so
 * the raw Z count is 16384 + MPU6050_ACCEL_BIAS_Z. Feeding 16384 instead and
 * expecting 1.0 g would be testing that the bias is not applied at all.
 */
static void test_calibration_preserves_gravity(void)
{
    printf("calib: gravity survives the correction\n");

    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;
    struct mpu6050_calibrated c;

    make_level_sample(burst, 1.0f);
    CHECK(mpu6050_decode_burst(burst, sizeof(burst),
                               mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                               mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR),
                               &s) == 0, "decode ok");
    CHECK(mpu6050_apply_calibration(&s,
                                    mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                                    mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR),
                                    &c) == 0, "apply ok");

    CHECK(close_to(c.accel_g[2], 1.0f, 0.001f),
          "level part must still read 1 g, got %f", c.accel_g[2]);
    CHECK(!close_to(c.accel_g[2], 0.0f, 0.001f),
          "gravity was subtracted: the bias ate the measurement");
    CHECK(close_to(c.accel_magnitude_g, 1.0f, 0.001f),
          "magnitude 1 g, got %f", c.accel_magnitude_g);

    /*
     * The converse, and the one that isolates the bug: at rest the part reads
     * gravity plus offset, and subtracting the raw mean would zero that axis.
     * So the raw reading must NOT come out as 0 g after correction.
     */
    CHECK(close_to(c.accel_counts[2], 16384.0f, 1.0f),
          "corrected Z count keeps the 1 g, got %f", c.accel_counts[2]);
}

/*
 * accel_magnitude_g is computed with a local Newton iteration rather than
 * libm, because the target rootfs has no libm. This test is what makes that
 * safe: the same input through the real sqrtf must agree. If someone later
 * "optimises" the iteration down to two rounds, this fails immediately while
 * every other test still passes.
 *
 * It also pins the documented end-to-end result: the bench part at rest used
 * to read 1.0618 g, and after correction it must read 1.000.
 */
static void test_calibration_magnitude_matches_libm(void)
{
    printf("calib: newton sqrt agrees with libm\n");

    /*
     * Stay inside what an int16 can express at +/-2 g: 32767 counts is 2.0 g,
     * so anything above that would be silently truncated by the cast and the
     * test would be measuring integer overflow rather than sqrt accuracy. The
     * range that matters for the iteration is the range that can occur.
     */
    static const float targets[] = { 0.01f, 0.5f, 1.0f, 1.0618f, 1.9f };
    size_t i;

    for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        /*
         * Reach the internal helper the only way a caller can: craft a sample
         * whose corrected magnitude is the target. A level part with the given
         * Z does it - see make_level_sample for why the other axes sit at
         * their offsets rather than at zero.
         */
        uint8_t burst[MPU6050_BURST_LEN];
        struct mpu6050_sample s;
        struct mpu6050_calibrated c;

        make_level_sample(burst, targets[i]);
        mpu6050_decode_burst(burst, sizeof(burst),
                             mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                             mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR), &s);
        CHECK(mpu6050_apply_calibration(&s,
                                        mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                                        mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR),
                                        &c) == 0, "apply ok");

        /* The crafted int16 rounds, which is 6e-5 g per LSB; the iteration's
         * own error is far below that. */
        CHECK(close_to(c.accel_magnitude_g, targets[i], 1e-3f),
              "newton sqrt for %f g returned %f", targets[i],
              c.accel_magnitude_g);
        CHECK(close_to(c.accel_magnitude_g,
                       sqrtf(c.accel_g[0] * c.accel_g[0] +
                             c.accel_g[1] * c.accel_g[1] +
                             c.accel_g[2] * c.accel_g[2]),
                       1e-4f),
              "newton sqrt disagrees with libm at %f g", targets[i]);
    }

    /* And the documented bench figure, from the real mean. */
    {
        uint8_t burst[MPU6050_BURST_LEN];
        struct mpu6050_sample s;
        struct mpu6050_calibrated c;

        make_burst(burst, -2639, -67, 17195, 3882, 553, -612, -240);
        mpu6050_decode_burst(burst, sizeof(burst),
                             mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                             mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR), &s);
        mpu6050_apply_calibration(&s,
                                  mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                                  mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR), &c);
        CHECK(close_to(c.accel_magnitude_g, 1.0f, 0.005f),
              "bench part reads 1.000 g after correction, got %f",
              c.accel_magnitude_g);
    }
}

/*
 * The biases are raw counts, so they only mean anything at the range they were
 * measured with. A caller that configures +/-16 g and still subtracts them
 * would apply a correction 8x too large, with no error anywhere. Refusing is
 * the only safe answer, because the offsets would have to be rescaled and the
 * caller is the only one who knows the intent.
 */
static void test_calibration_rejects_wrong_fsr(void)
{
    printf("calib: refuses a mismatched full scale\n");

    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;
    struct mpu6050_calibrated c;

    make_burst(burst, 0, 0, 16384, 0, 0, 0, 0);
    mpu6050_decode_burst(burst, sizeof(burst),
                         mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                         mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR), &s);

    CHECK(mpu6050_apply_calibration(&s, mpu6050_accel_scale(MPU6050_FSR_ACCEL_16G),
                                    mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR),
                                    &c) == -1, "accel at 16 g refused");
    CHECK(mpu6050_apply_calibration(&s, mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                                    mpu6050_gyro_scale(MPU6050_FSR_GYRO_2000),
                                    &c) == -1, "gyro at 2000 dps refused");
    CHECK(mpu6050_apply_calibration(&s, 0.0f,
                                    mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR),
                                    &c) == -1, "zero scale refused");
    CHECK(mpu6050_apply_calibration(&s, mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                                    mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR),
                                    &c) == 0, "matching scales accepted");
}

static void test_calibration_rejects_bad_args(void)
{
    printf("calib: argument validation\n");

    struct mpu6050_sample s;
    struct mpu6050_calibrated c;

    memset(&s, 0, sizeof(s));

    CHECK(mpu6050_apply_calibration(NULL,
                                    mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                                    mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR),
                                    &c) == -1, "null input refused");
    CHECK(mpu6050_apply_calibration(&s,
                                    mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                                    mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR),
                                    NULL) == -1, "null output refused");

    /*
     * Self-alias would overwrite the raw counts it is still reading. The
     * structs differ in type, so this is the one case that catches an
     * implementation testing the wrong thing: the natural (and wrong) check is
     * `in->accel_raw == out->accel_counts`, which decays to different types
     * and is therefore always false - so it compiles, and it never fires.
     */
    {
        union {
            struct mpu6050_sample s;
            struct mpu6050_calibrated c;
        } alias;
        memset(&alias, 0, sizeof(alias));
        CHECK(mpu6050_apply_calibration(&alias.s,
                                        mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                                        mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR),
                                        &alias.c) == -1,
              "overlapping in/out refused");
    }
}

/* Temperature has no at-rest offset, so it must come through bit for bit. */
static void test_calibration_passes_temperature_through(void)
{
    printf("calib: temperature is not touched\n");

    uint8_t burst[MPU6050_BURST_LEN];
    struct mpu6050_sample s;
    struct mpu6050_calibrated c;

    make_burst(burst, 0, 0, 16384, 3882, 0, 0, 0);
    mpu6050_decode_burst(burst, sizeof(burst),
                         mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                         mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR), &s);
    mpu6050_apply_calibration(&s, mpu6050_accel_scale(MPU6050_CAL_ACCEL_FSR),
                              mpu6050_gyro_scale(MPU6050_CAL_GYRO_FSR), &c);

    CHECK(c.temp_c == s.temp_c, "temperature unchanged");
    CHECK(c.timestamp_us == s.timestamp_us, "timestamp unchanged");
    CHECK(close_to(c.temp_c, 47.95f, 0.02f), "bench temperature ~47.95 C, got %f",
          c.temp_c);
}

int main(void)
{
    printf("=== MPU6050 decoding tests ===\n");

    test_scales();
    test_fsr_bits();
    test_decode_flat();
    test_decode_signs();
    test_decode_temperature();
    test_axis_order();
    test_full_scale_saturation();
    test_rejects_short_burst();
    test_zero_sample_on_failure();
    test_giant_values();
    test_calibration_removes_gyro_bias();
    test_calibration_preserves_gravity();
    test_calibration_magnitude_matches_libm();
    test_calibration_rejects_wrong_fsr();
    test_calibration_rejects_bad_args();
    test_calibration_passes_temperature_through();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
        printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
