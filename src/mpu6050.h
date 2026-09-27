#ifndef MPU6050_H
#define MPU6050_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * MPU6050 six-axis IMU over I2C.
 *
 * Layering, following the same rule the RTSP code follows:
 *
 *   mpu6050.c     protocol + register decoding. Pure buffer maths, no ioctl,
 *                 no socket, no Linux headers. Builds and tests on Windows.
 *   mpu6050_i2c.c the ONLY file that talks to /dev/i2c-N. One open, two ioctls.
 *
 * The split exists because the interesting part of a sensor driver is the
 * decoding, and the decoding is exactly the part that is painful to test on
 * hardware. Keeping it transport-free means the scaling maths, the sign
 * handling and the big-endian reads are all covered by host unit tests, and
 * the untestable remainder is small enough to eyeball.
 *
 * The part is read as one 14 byte burst starting at 0x3B:
 *
 *   0x3B ACCEL_XOUT_H   int16 big endian
 *   0x3D ACCEL_YOUT_H
 *   0x3F ACCEL_ZOUT_H
 *   0x41 TEMP_OUT_H     int16, T[C] = raw / 340.0 + 36.53
 *   0x43 GYRO_XOUT_H    int16 big endian
 *   0x45 GYRO_YOUT_H
 *   0x47 GYRO_ZOUT_H
 *
 * Bursting matters: six separate reads would sample axes from six different
 * moments, which corrupts any attitude estimate. One transaction gives one
 * consistent sample.
 */

#define MPU6050_I2C_DEV_DEFAULT "/dev/i2c-4"

/* 7-bit address. AD0 low = 0x68, AD0 high = 0x69. */
#define MPU6050_ADDR_AD0_LOW  0x68
#define MPU6050_ADDR_AD0_HIGH 0x69

#define MPU6050_REG_SMPLRT_DIV   0x19
#define MPU6050_REG_CONFIG       0x1A
#define MPU6050_REG_GYRO_CONFIG  0x1B
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_INT_PIN_CFG  0x37
#define MPU6050_REG_ACCEL_XOUT_H 0x3B
#define MPU6050_REG_PWR_MGMT_1   0x6B
#define MPU6050_REG_WHO_AM_I     0x75

/* WHO_AM_I is the only way to tell a real part from a floating bus. */
#define MPU6050_WHO_AM_I_VALUE 0x68

#define MPU6050_BURST_START MPU6050_REG_ACCEL_XOUT_H
#define MPU6050_BURST_LEN   14

struct mpu6050_config {
    /* Path to the i2c adapter, e.g. "/dev/i2c-4". NULL uses the default. */
    const char *i2c_dev;
    /* 7-bit address, MPU6050_ADDR_AD0_LOW or _HIGH. 0 uses the default. */
    uint8_t address;
    /* Full scale range, one of the MPU6050_FSR_* constants. 0 uses +/-2g. */
    int accel_fsr;
    int gyro_fsr;
    /*
     * Sample rate divider. 0 = 8 kHz, 7 = 1 kHz, 9 = 800 Hz.
     * Applied on top of the 1 kHz gyro output rate: rate = 1000/(1+div).
     */
    uint8_t sample_rate_div;
    /* DLPF bandwidth, one of the MPU6050_DLPF_* constants. 0 uses 44 Hz. */
    uint8_t dlpf;
    /*
     * 0 = skip the WHO_AM_I check (useful for clones that misreport it),
     * non-zero = refuse to open when the part does not answer 0x68.
     */
    int verify_who_am_i;
};

/* Accelerometer full scale, in g. */
#define MPU6050_FSR_ACCEL_2G  0
#define MPU6050_FSR_ACCEL_4G  1
#define MPU6050_FSR_ACCEL_8G  2
#define MPU6050_FSR_ACCEL_16G 3

/* Gyroscope full scale, in degrees per second. */
#define MPU6050_FSR_GYRO_250  0
#define MPU6050_FSR_GYRO_500  1
#define MPU6050_FSR_GYRO_1000 2
#define MPU6050_FSR_GYRO_2000 3

/* DLPF_CFG values. Higher numbers mean lower bandwidth and lower noise. */
#define MPU6050_DLPF_260HZ 0
#define MPU6050_DLPF_184HZ 1
#define MPU6050_DLPF_94HZ  2
#define MPU6050_DLPF_44HZ  3
#define MPU6050_DLPF_21HZ  4
#define MPU6050_DLPF_10HZ  5
#define MPU6050_DLPF_5HZ   6

/*
 * One decoded sample. Raw counts are kept alongside the scaled values so a
 * caller can log exactly what the part said, and so a regression shows up as
 * a changed raw value rather than a rounding difference in the scaled one.
 */
struct mpu6050_sample {
    /* Raw signed counts, straight off the bus. */
    int16_t accel_raw[3];
    int16_t gyro_raw[3];
    int16_t temp_raw;

    /* Scaled: accel in g, gyro in deg/s, temp in degrees Celsius. */
    float accel_g[3];
    float gyro_dps[3];
    float temp_c;

    uint64_t timestamp_us;
};

/* ------------------------------------------------------------------------ */
/* Decoding layer: no I/O, no Linux headers, fully host testable.            */
/* ------------------------------------------------------------------------ */

/*
 * Decode one 14 byte burst as read from MPU6050_BURST_START.
 *
 * accel_scale is g per LSB and gyro_scale is deg/s per LSB; use
 * mpu6050_accel_scale() / mpu6050_gyro_scale() to derive them from the FSR
 * the part was configured with. Returns 0 on success, -1 on bad arguments.
 */
int mpu6050_decode_burst(const uint8_t *burst,
                         size_t length,
                         float accel_scale,
                         float gyro_scale,
                         struct mpu6050_sample *sample);

/* g per LSB for a MPU6050_FSR_ACCEL_* value. */
float mpu6050_accel_scale(int accel_fsr);

/* deg/s per LSB for a MPU6050_FSR_GYRO_* value. */
float mpu6050_gyro_scale(int gyro_fsr);

/* Register value for a MPU6050_FSR_ACCEL_* constant, or -1 if out of range. */
int mpu6050_accel_fsr_bits(int accel_fsr);

/* Register value for a MPU6050_FSR_GYRO_* constant, or -1 if out of range. */
int mpu6050_gyro_fsr_bits(int gyro_fsr);

/* ------------------------------------------------------------------------ */
/* I2C transport: only implemented on Linux, lives in mpu6050_i2c.c.         */
/* ------------------------------------------------------------------------ */

struct mpu6050_imu;

/*
 * Open the bus, wake the part, apply the configured ranges and sample rate.
 *
 * Returns 0 on success. On failure returns -1 with errno set; the message is
 * left to the caller's log so this layer stays free of printf.
 *
 * Fails with ENODEV when verify_who_am_i is set and the part does not answer,
 * which is the difference between "wrong address" and "nothing on the bus".
 */
int mpu6050_open(const struct mpu6050_config *config, struct mpu6050_imu **imu);

void mpu6050_close(struct mpu6050_imu *imu);

/* Read one sample. timestamp_us is filled from CLOCK_MONOTONIC. */
int mpu6050_read(struct mpu6050_imu *imu, struct mpu6050_sample *sample);

/* Read WHO_AM_I without disturbing the configuration. */
int mpu6050_read_who_am_i(struct mpu6050_imu *imu, uint8_t *value);

/*
 * Probe a 7-bit address: returns 1 when the address acknowledges, 0 when it
 * does not, -1 on a transport error. Used by the scanner to find the part.
 */
int mpu6050_probe_address(int fd, uint8_t address);

/* The raw adapter fd, for the scanner in tools/. */
int mpu6050_fd(const struct mpu6050_imu *imu);

#endif
