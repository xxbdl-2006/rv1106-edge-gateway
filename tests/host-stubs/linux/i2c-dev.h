#ifndef LINUX_I2C_DEV_H
#define LINUX_I2C_DEV_H

/*
 * Stub of <linux/i2c-dev.h> for `make host-syntax` on Windows.
 *
 * MinGW has no i2c-dev.h, so src/mpu6050_i2c.c cannot be checked here without
 * one. This stub carries only the constants the driver uses, with the real
 * values for the ioctl request numbers, so a typo in a name is caught even
 * though the numbers themselves do not matter on the host.
 *
 * It is deliberately NOT a full implementation: nothing here is executed,
 * the file is only ever compiled with -fsyntax-only.
 */

#include <sys/ioctl.h>

/* ioctl request numbers from <linux/i2c-dev.h>. */
#define I2C_SLAVE       0x0703
#define I2C_SLAVE_FORCE 0x0706
#define I2C_FUNCS       0x0705

/* Capability bits from <linux/i2c.h>. */
#define I2C_FUNC_I2C                0x00000001
#define I2C_FUNC_SMBUS_WRITE_BYTE   0x00020000

#endif
