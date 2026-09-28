/*
 * MPU6050 register decoding.
 *
 * This file deliberately includes nothing but the C standard library. That is
 * what lets tests/test_mpu6050.c build and run on the Windows host, where the
 * real bus obviously does not exist. See mpu6050.h for the layering rationale.
 */

#include "mpu6050.h"

#include <string.h>

/* Sensitivity per the MPU6050 register map, table "Accelerometer Specifications". */
#define MPU6050_ACCEL_LSB_PER_G_2G  16384.0f
#define MPU6050_ACCEL_LSB_PER_G_4G  8192.0f
#define MPU6050_ACCEL_LSB_PER_G_8G  4096.0f
#define MPU6050_ACCEL_LSB_PER_G_16G 2048.0f

/* Gyroscope sensitivity, table "Gyroscope Specifications". */
#define MPU6050_GYRO_LSB_PER_DPS_250  131.0f
#define MPU6050_GYRO_LSB_PER_DPS_500  65.5f
#define MPU6050_GYRO_LSB_PER_DPS_1000 32.8f
#define MPU6050_GYRO_LSB_PER_DPS_2000 16.4f

/* Temperature: T[C] = raw / 340 + 36.53, per the register map. */
#define MPU6050_TEMP_LSB_PER_C  340.0f
#define MPU6050_TEMP_OFFSET_C   36.53f

/*
 * The bus delivers the high byte first. Doing this by hand rather than by
 * casting the buffer to int16_t keeps the code correct on any host endianness,
 * which matters because the unit test runs on x86 while the target is ARM.
 */
static int16_t read_be16(const uint8_t *p)
{
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/*
 * WHO_AM_I values this driver can decode.
 *
 * All of these share the output register map the decoder assumes: 0x3B accel,
 * 0x41 temperature, 0x43 gyro, 16 bit big endian, and the same 16384 LSB/g at
 * +/-2g. They differ in id, in some control bit masks, and in what else is on
 * the die (the 9250 has a magnetometer behind its own bypass).
 *
 * The distinction that matters in practice: the module on this bench reports
 * 0x70, an MPU6500, and a driver that only accepted 0x68 would have refused to
 * open a sensor that reads back perfectly good physics. The list is here so
 * that decision is explicit rather than accidental.
 */
int mpu6050_who_am_i_supported(uint8_t who)
{
    switch (who) {
    case 0x68: /* MPU6050 */
    case 0x70: /* MPU6500 */
    case 0x71: /* MPU9250 */
    case 0x72: /* MPU6515 */
    case 0x73: /* MPU9255 */
    case 0x98: /* ICM series and various clones */
        return 1;
    default:
        return 0;
    }
}

const char *mpu6050_who_am_i_name(uint8_t who)
{
    switch (who) {
    case 0x68:
        return "MPU6050";
    case 0x70:
        return "MPU6500";
    case 0x71:
        return "MPU9250";
    case 0x72:
        return "MPU6515";
    case 0x73:
        return "MPU9255";
    case 0x98:
        return "ICM-series or clone";
    default:
        return NULL;
    }
}

float mpu6050_accel_scale(int accel_fsr)
{
    switch (accel_fsr) {
    case MPU6050_FSR_ACCEL_2G:
        return 1.0f / MPU6050_ACCEL_LSB_PER_G_2G;
    case MPU6050_FSR_ACCEL_4G:
        return 1.0f / MPU6050_ACCEL_LSB_PER_G_4G;
    case MPU6050_FSR_ACCEL_8G:
        return 1.0f / MPU6050_ACCEL_LSB_PER_G_8G;
    case MPU6050_FSR_ACCEL_16G:
        return 1.0f / MPU6050_ACCEL_LSB_PER_G_16G;
    default:
        return 0.0f;
    }
}

float mpu6050_gyro_scale(int gyro_fsr)
{
    switch (gyro_fsr) {
    case MPU6050_FSR_GYRO_250:
        return 1.0f / MPU6050_GYRO_LSB_PER_DPS_250;
    case MPU6050_FSR_GYRO_500:
        return 1.0f / MPU6050_GYRO_LSB_PER_DPS_500;
    case MPU6050_FSR_GYRO_1000:
        return 1.0f / MPU6050_GYRO_LSB_PER_DPS_1000;
    case MPU6050_FSR_GYRO_2000:
        return 1.0f / MPU6050_GYRO_LSB_PER_DPS_2000;
    default:
        return 0.0f;
    }
}

int mpu6050_accel_fsr_bits(int accel_fsr)
{
    if (accel_fsr < MPU6050_FSR_ACCEL_2G || accel_fsr > MPU6050_FSR_ACCEL_16G)
        return -1;
    /* AFS_SEL occupies bits 4:3 of ACCEL_CONFIG; the value is the enum. */
    return (accel_fsr & 0x3) << 3;
}

int mpu6050_gyro_fsr_bits(int gyro_fsr)
{
    if (gyro_fsr < MPU6050_FSR_GYRO_250 || gyro_fsr > MPU6050_FSR_GYRO_2000)
        return -1;
    /* FS_SEL occupies bits 4:3 of GYRO_CONFIG. */
    return (gyro_fsr & 0x3) << 3;
}

int mpu6050_decode_burst(const uint8_t *burst,
                         size_t length,
                         float accel_scale,
                         float gyro_scale,
                         struct mpu6050_sample *sample)
{
    if (sample == NULL)
        return -1;

    /*
     * Zero before the other checks, not after: the point of zeroing is to
     * protect a caller that ignores the return value, and such a caller is
     * exactly the one that hits the early exits below.
     */
    memset(sample, 0, sizeof(*sample));

    if (burst == NULL)
        return -1;
    if (length < MPU6050_BURST_LEN)
        return -1;

    sample->accel_raw[0] = read_be16(&burst[0]);
    sample->accel_raw[1] = read_be16(&burst[2]);
    sample->accel_raw[2] = read_be16(&burst[4]);
    sample->temp_raw = read_be16(&burst[6]);
    sample->gyro_raw[0] = read_be16(&burst[8]);
    sample->gyro_raw[1] = read_be16(&burst[10]);
    sample->gyro_raw[2] = read_be16(&burst[12]);

    for (int i = 0; i < 3; i++) {
        sample->accel_g[i] = (float)sample->accel_raw[i] * accel_scale;
        sample->gyro_dps[i] = (float)sample->gyro_raw[i] * gyro_scale;
    }
    sample->temp_c = (float)sample->temp_raw / MPU6050_TEMP_LSB_PER_C
                     + MPU6050_TEMP_OFFSET_C;

    return 0;
}

/*
 * Guard against a caller that hands us scales from a different range than the
 * biases were measured at. Comparing floats for exact equality is fine here
 * and nowhere else: both sides are the same expression evaluated from the
 * same constants, so either they are bit-identical or the caller really did
 * configure another range.
 */
static int scale_matches(float scale, int fsr, int accel)
{
    float expected = accel ? mpu6050_accel_scale(fsr) : mpu6050_gyro_scale(fsr);

    return scale == expected;
}

/*
 * sqrt without libm.
 *
 * The target rootfs ships libc only, and one sqrt() drags in the whole of
 * libm just to turn a check value into a number. Newton's method from a fixed
 * seed converges to full float precision in well under twenty iterations for
 * the range here (0 to a few g), and unlike a magic-constant bit hack it is
 * obvious to a reader what it computes.
 *
 * Seeding from x0 = v is exact for v == 1 and converges for any v > 0; the
 * guard matters because a zero or negative input would otherwise divide by
 * zero on the first step.
 */
static float sqrt_newton(float v)
{
    float x;
    int i;

    if (v <= 0.0f)
        return 0.0f;

    x = v;
    for (i = 0; i < 20; i++)
        x = 0.5f * (x + v / x);

    return x;
}

int mpu6050_apply_calibration(const struct mpu6050_sample *in,
                              float accel_scale,
                              float gyro_scale,
                              struct mpu6050_calibrated *out)
{
    static const float accel_bias[3] = {
        MPU6050_ACCEL_BIAS_X,
        MPU6050_ACCEL_BIAS_Y,
        MPU6050_ACCEL_BIAS_Z,
    };
    static const float gyro_bias[3] = {
        MPU6050_GYRO_BIAS_X,
        MPU6050_GYRO_BIAS_Y,
        MPU6050_GYRO_BIAS_Z,
    };
    float sum_squares = 0.0f;
    int i;

    if (in == NULL || out == NULL)
        return -1;
    /*
     * The structs are different types, so overlap has to be judged by address
     * range rather than by pointer equality. Comparing in->accel_raw with
     * out->accel_counts would be worse than useless: the array-to-pointer
     * decay makes them different types, the comparison compiles, and it is
     * always false.
     */
    if ((const void *)in == (const void *)out)
        return -1;

    if (!scale_matches(accel_scale, MPU6050_CAL_ACCEL_FSR, 1))
        return -1;
    if (!scale_matches(gyro_scale, MPU6050_CAL_GYRO_FSR, 0))
        return -1;

    for (i = 0; i < 3; i++) {
        out->accel_counts[i] = (float)in->accel_raw[i] - accel_bias[i];
        out->gyro_counts[i] = (float)in->gyro_raw[i] - gyro_bias[i];
        out->accel_g[i] = out->accel_counts[i] * accel_scale;
        out->gyro_dps[i] = out->gyro_counts[i] * gyro_scale;
        sum_squares += out->accel_g[i] * out->accel_g[i];
    }

    out->accel_magnitude_g = sqrt_newton(sum_squares);
    out->temp_c = in->temp_c;
    out->timestamp_us = in->timestamp_us;

    return 0;
}
