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
