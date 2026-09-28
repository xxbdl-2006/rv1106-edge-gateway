#ifndef I2C_BITBANG_H
#define I2C_BITBANG_H

#include <stddef.h>
#include <stdint.h>

#include "mpu6050_gpio.h"

/*
 * Bit-banged I2C master.
 *
 * Why this exists at all: on the Luckfox Pico Max the i2c3 controller that
 * would serve header pins 24/14 is status = "disabled" in the device tree, so
 * no /dev/i2c-N ever appears. The runtime device-tree overlay route turned out
 * to be non-functional on this board (writing literally the string "GARBAGE"
 * as a dtbo returns success where a working kernel returns -EINVAL, and dmesg
 * shows no fragment activity at all), so we cannot just flip status at runtime.
 *
 * What saves us: a disabled controller never claims its pinctrl, so those pins
 * stay plain unclaimed GPIO. Driving them from userspace reproduces I2C well
 * enough for a sensor polled at 100 Hz.
 *
 * This file contains ONLY the timing. It never opens a file or sleeps: every
 * pin operation and every delay goes through struct i2c_gpio_ops, which the
 * host test replaces with a recorder. That keeps the part that is easy to get
 * wrong under unit test, and leaves the sysfs plumbing small enough to read.
 *
 * Speed: sysfs costs roughly 10-50 us per access, so the half period is
 * measured in tens of microseconds, not the 2.5 us of a 100 kHz bus. That is
 * fine. An I2C target has no minimum clock rate; the MPU6050 works correctly
 * at a few kHz and the 14 byte burst still completes in a few milliseconds.
 */

/* Result codes. Distinct so a caller can tell "absent" from "broken". */
#define I2C_BB_OK       0
#define I2C_BB_ENOACK  -1  /* a device did not acknowledge */
#define I2C_BB_EIO     -2  /* the gpio backend reported a failure */
#define I2C_BB_EINVAL  -3  /* bad argument */

struct i2c_bitbang {
    const struct i2c_gpio_ops *ops;
    void *ctx;
    /*
     * Counters, kept for the probe tool's diagnostics. A bus that reports
     * "no ACK" but also shows clock_stretch_timeouts climbing is telling a
     * very different story from one that is simply not connected.
     */
    uint32_t transactions;
    uint32_t ack_failures;
    uint32_t io_errors;
};

/* Bind the timing layer to a gpio backend. Returns 0 on success. */
int i2c_bb_init(struct i2c_bitbang *bus,
                const struct i2c_gpio_ops *ops,
                void *ctx);

/*
 * Leave the bus idle and release both lines.
 *
 * Must be called on every exit path, including errors. A transaction that
 * bails out holding SCL low leaves the target waiting for a clock edge that
 * never comes and wedges the whole bus until the target is power cycled.
 */
void i2c_bb_release(struct i2c_bitbang *bus);

/*
 * Clock out nine pulses and issue a STOP.
 *
 * Recovers a bus where a target was interrupted mid-byte and is still holding
 * SDA low. Cheap enough to run before a scan; without it a wedged slave makes
 * the whole bus look dead.
 */
int i2c_bb_bus_recover(struct i2c_bitbang *bus);

/*
 * Read `length` bytes starting at `reg`.
 *
 * The register pointer is written with the repeated-START form
 * (S, addr+W, reg, Sr, addr+R, data..., P) rather than a STOP between phases.
 * A STOP also happens to work on the MPU6050, but the repeated START is what
 * the datasheet specifies and it matters on parts that reset their pointer
 * when they see one.
 */
int i2c_bb_read_regs(struct i2c_bitbang *bus,
                     uint8_t address,
                     uint8_t reg,
                     uint8_t *out,
                     size_t length);

/* Write `length` bytes starting at `reg`. */
int i2c_bb_write_regs(struct i2c_bitbang *bus,
                      uint8_t address,
                      uint8_t reg,
                      const uint8_t *values,
                      size_t length);

/*
 * Address probe: returns 1 when the address acknowledges its address byte,
 * 0 when it does not, and a negative I2C_BB_* code on a transport failure.
 *
 * Only the address byte is sent, which is the most forgiving possible test:
 * some parts NAK a register access until they have been unlocked, but every
 * part answers its own address.
 */
int i2c_bb_probe_address(struct i2c_bitbang *bus, uint8_t address);

#endif /* I2C_BITBANG_H */
