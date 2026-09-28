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
 *   i2c_bitbang.c I2C timing. No I/O at all: every pin access and every delay
 *                 goes through struct i2c_gpio_ops, so the host test drives it
 *                 with a recorder and asserts on the real edge sequence.
 *   gpio_sysfs.c  the only file that touches /sys/class/gpio. Linux only.
 *   mpu6050_i2c.c register policy on top of all three. Linux only.
 *
 * The split exists because the interesting parts of a sensor driver are the
 * decoding and the timing, and those are exactly the parts that are painful to
 * test on hardware. Keeping both transport-free means the scaling maths, the
 * sign handling, the big-endian reads and the I2C edge ordering are all
 * covered by host unit tests, and the untestable remainder is small enough to
 * eyeball.
 *
 * Why not /dev/i2c-N: on the Luckfox Pico Max the i2c3 controller that serves
 * header pins 24/14 is status = "disabled" in the device tree, so no adapter
 * node is ever created. The runtime overlay that would flip it does not work
 * on this board either - see dts/i2c3-enable.dts for the evidence. A disabled
 * controller never claims its pinctrl, which leaves those pins as ordinary
 * GPIO and makes userspace bit-banging the practical route.
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

/* Default GPIO numbers for the bit-banged bus (header pin 24 SCL / pin 14 SDA). */
#define MPU6050_SCL_GPIO_DEFAULT 70
#define MPU6050_SDA_GPIO_DEFAULT 71

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

/*
 * WHO_AM_I values that share the MPU6050 output register layout.
 *
 * 0x68 is the original MPU6050, but modules ship with whatever the assembler
 * could get. The one on this bench answers 0x70, an MPU6500: identical output
 * registers, different id. Treating only 0x68 as valid would reject it, so the
 * whole family is accepted and the exact part is reported rather than enforced.
 */
#define MPU6050_WHO_AM_I_VALUE 0x68

/* Non-zero when `who` is a part this driver can decode. */
int mpu6050_who_am_i_supported(uint8_t who);

/* Human readable model name for a WHO_AM_I value, or NULL if unrecognised. */
const char *mpu6050_who_am_i_name(uint8_t who);

#define MPU6050_BURST_START MPU6050_REG_ACCEL_XOUT_H
#define MPU6050_BURST_LEN   14

struct mpu6050_config {
    /*
     * GPIO numbers of the two bus lines. 0 uses MPU6050_SCL_GPIO_DEFAULT /
     * MPU6050_SDA_GPIO_DEFAULT, which are header pin 24 and pin 14.
     */
    unsigned scl_gpio;
    unsigned sda_gpio;
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
     * 0 = skip the WHO_AM_I check, non-zero = refuse to open when the part is
     * not a recognised member of the family. Note this accepts 0x70 and
     * friends, not just 0x68; see mpu6050_who_am_i_supported().
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
/* Transport: only implemented on Linux, lives in mpu6050_i2c.c.             */
/* ------------------------------------------------------------------------ */

struct mpu6050_imu;

/*
 * Open the pins, unwedge the bus, wake the part, apply the configured ranges
 * and sample rate.
 *
 * Returns 0 on success. On failure returns -1 with errno set; the message is
 * left to the caller's log so this layer stays free of printf.
 *
 * Fails with ENODEV when verify_who_am_i is set and the part does not answer
 * with a recognised id, which is the difference between "wrong address" and
 * "nothing on the bus". Fails with ENOMEM when the pins cannot be exported,
 * usually because something else already claimed them.
 */
int mpu6050_open(const struct mpu6050_config *config, struct mpu6050_imu **imu);

void mpu6050_close(struct mpu6050_imu *imu);

/* Read one sample. timestamp_us is filled from CLOCK_MONOTONIC. */
int mpu6050_read(struct mpu6050_imu *imu, struct mpu6050_sample *sample);

/* Read WHO_AM_I without disturbing the configuration. */
int mpu6050_read_who_am_i(struct mpu6050_imu *imu, uint8_t *value);

/*
 * Transport counters, for diagnostics.
 *
 * These distinguish failure modes that otherwise look identical from the
 * caller's side: a bus with transactions climbing and ack_failures flat is
 * talking to something; one with io_errors climbing cannot even drive the
 * pins, which points at permissions or a pin already claimed by a driver.
 */
unsigned mpu6050_transactions(const struct mpu6050_imu *imu);
unsigned mpu6050_ack_failures(const struct mpu6050_imu *imu);
unsigned mpu6050_io_errors(const struct mpu6050_imu *imu);

#endif
