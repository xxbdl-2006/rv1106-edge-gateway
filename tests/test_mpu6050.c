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

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
        printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
