#ifndef GPIO_SYSFS_H
#define GPIO_SYSFS_H

#include "mpu6050_gpio.h"

/*
 * Linux sysfs GPIO backend for the bit-banged I2C master.
 *
 * Declared outside the __linux__ guard on purpose: the type is opaque, so a
 * caller can hold a pointer to one on any platform without seeing a definition
 * that does not exist. Only gpio_sysfs.c needs the layout, and that file is
 * Linux only.
 *
 * The functions themselves exist only on Linux. Anything that calls them must
 * be Linux only too, which is the case for every board-side user.
 */
struct sysfs_gpio;

/*
 * Export and configure both pins.
 *
 * Returns NULL on failure with errno set. half_period_us is the delay applied
 * after every level change; 25 us gives roughly a 20 kHz bus, which the
 * MPU6050 is perfectly happy with and which keeps a 14 byte burst in the low
 * milliseconds.
 */
struct sysfs_gpio *sysfs_gpio_open(unsigned scl, unsigned sda,
                                   unsigned half_period_us);

/* Release both lines back to inputs, unexport the pins, free the state. */
void sysfs_gpio_close(struct sysfs_gpio *gpio);

/* The ops table to hand to i2c_bb_init(). */
const struct i2c_gpio_ops *sysfs_gpio_ops(void);

#endif /* GPIO_SYSFS_H */
