#ifndef MPU6050_GPIO_H
#define MPU6050_GPIO_H

#include <stdint.h>

/*
 * Minimal pin-level interface the bit-banged I2C master drives.
 *
 * The point of this indirection is testability. The I2C timing is the part
 * worth getting right, and the sysfs calls underneath it are the part that
 * cannot run on a development host. So the timing layer talks to this
 * interface, and the test suite plugs in a fake that records every edge
 * instead of touching a filesystem.
 *
 * Semantics: the bus is open drain, so there is no "drive high" operation.
 * A line is either pulled low or released and left to the external pull-up.
 *
 *   line_low()     drive the line low
 *   line_release() stop driving; the pull-up takes it high
 *   line_read()    sample the actual voltage on the wire
 *
 * line_read() must return what is physically on the pin, not what was last
 * written. A backend that reads back its own output latch would never see a
 * slave pulling the line down, which is how ACK is detected.
 */

enum i2c_line {
    I2C_LINE_SCL = 0,
    I2C_LINE_SDA = 1
};

struct i2c_gpio_ops {
    /* Drive the given line low. Returns 0 on success. */
    int (*line_low)(void *ctx, enum i2c_line line);

    /* Stop driving; the external pull-up pulls the line high. */
    int (*line_release)(void *ctx, enum i2c_line line);

    /*
     * Sample the line. Returns 1 for high, 0 for low, -1 on a read error.
     * A negative return is distinct from 0 on purpose: "the wire is low" and
     * "I could not look at the wire" must not be confused, because the first
     * is a valid ACK and the second is a driver failure.
     */
    int (*line_read)(void *ctx, enum i2c_line line);

    /* Sleep for the configured half period. Returns 0 on success. */
    int (*delay)(void *ctx);

    /* Release both lines. Called on close and on every error path. */
    void (*release_all)(void *ctx);
};

#endif /* MPU6050_GPIO_H */
