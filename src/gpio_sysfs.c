/*
 * sysfs GPIO backend for the bit-banged I2C master. Linux only.
 *
 * This is the only file in the sensor path that touches the filesystem. It
 * implements struct i2c_gpio_ops on top of /sys/class/gpio, so the timing
 * logic in i2c_bitbang.c stays host testable and this file stays small enough
 * to review by reading it once.
 *
 * On Windows this compiles to nothing, which is what lets the Makefile list it
 * unconditionally without breaking `make test`.
 */

#include "mpu6050_gpio.h"

#ifdef __linux__

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * One pin, held as pre-opened file descriptors.
 *
 * The naive implementation writes the path with snprintf and reopens it on
 * every single edge. A 14 byte read is over 200 edges, so that turns into
 * thousands of open/close pairs per sample and dominates the timing budget.
 * Instead each pin keeps two fds: one for `direction` and one for `value`,
 * both opened once at setup.
 *
 * The direction fd is lseek'ed back to 0 before each write, because a sysfs
 * attribute write consumes the offset and a second write without rewinding
 * does nothing at all. That failure is silent and looks exactly like "the
 * slave never drives the line", which is a memorable way to lose an afternoon.
 */
struct gpio_pin {
    int dir_fd;   /* /sys/class/gpio/gpioN/direction */
    int val_fd;   /* /sys/class/gpio/gpioN/value, opened O_RDWR */
    int exported; /* 1 when we exported it and must unexport on close */
    unsigned number;
};

struct sysfs_gpio {
    struct gpio_pin scl;
    struct gpio_pin sda;
    struct timespec half_period;
    char dir_buf[8];
};

/* ------------------------------------------------------------------------- */
/* Pin plumbing                                                               */
/* ------------------------------------------------------------------------- */

static int write_attr(struct gpio_pin *pin, int fd, const char *text)
{
    size_t len = strlen(text);

    /*
     * Rewind first: sysfs attributes are single-shot, and a written-at-offset-1
     * write is silently discarded.
     */
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -1;
    if (write(fd, text, len) != (ssize_t)len)
        return -1;
    (void)pin;
    return 0;
}

static int pin_set_direction(struct gpio_pin *pin, const char *dir)
{
    return write_attr(pin, pin->dir_fd, dir);
}

static int pin_export(unsigned number, int *exported)
{
    char path[64];
    int fd;

    *exported = 0;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u", number);
    if (access(path, F_OK) == 0)
        return 0; /* somebody already exported it; leave it alone on close */

    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0)
        return -1;

    snprintf(path, sizeof(path), "%u", number);
    if (write(fd, path, strlen(path)) != (ssize_t)strlen(path)) {
        close(fd);
        return -1;
    }
    close(fd);
    *exported = 1;
    return 0;
}

static int pin_open(struct gpio_pin *pin, unsigned number)
{
    char path[64];
    int i;

    memset(pin, 0, sizeof(*pin));
    pin->number = number;
    pin->dir_fd = -1;
    pin->val_fd = -1;

    if (pin_export(number, &pin->exported) != 0)
        return -1;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/direction", number);
    /*
     * udev can lag the export by a few milliseconds on a busy boot. Retrying
     * briefly is kinder than failing a sensor open because the reader raced a
     * device manager that had not got round to it yet.
     */
    for (i = 0; i < 100; i++) {
        pin->dir_fd = open(path, O_WRONLY);
        if (pin->dir_fd >= 0)
            break;
        {
            struct timespec ts = { 0, 5 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    if (pin->dir_fd < 0)
        goto fail;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", number);
    /*
     * O_RDWR, not O_RDONLY: the same descriptor is used to sample the line and
     * to drive it, and the line must be readable *while* configured as an
     * input, which is how the ACK slot is detected.
     */
    pin->val_fd = open(path, O_RDWR);
    if (pin->val_fd < 0)
        goto fail;

    return 0;

fail:
    {
        int saved = errno;

        if (pin->dir_fd >= 0)
            close(pin->dir_fd);
        if (pin->val_fd >= 0)
            close(pin->val_fd);
        pin->dir_fd = -1;
        pin->val_fd = -1;
        if (pin->exported) {
            int fd = open("/sys/class/gpio/unexport", O_WRONLY);
            if (fd >= 0) {
                char num[16];
                snprintf(num, sizeof(num), "%u", number);
                if (write(fd, num, strlen(num)) < 0) {
                    /* nothing useful to do; the pin is already unusable */
                }
                close(fd);
            }
        }
        errno = saved;
    }
    return -1;
}

static void pin_close(struct gpio_pin *pin)
{
    if (pin->dir_fd >= 0) {
        /* Leave the pin as an input so it does not drive the bus when idle. */
        if (write_attr(pin, pin->dir_fd, "in") != 0) {
            /* best effort during teardown */
        }
        close(pin->dir_fd);
        pin->dir_fd = -1;
    }
    if (pin->val_fd >= 0) {
        close(pin->val_fd);
        pin->val_fd = -1;
    }
    if (pin->exported) {
        int fd = open("/sys/class/gpio/unexport", O_WRONLY);
        if (fd >= 0) {
            char num[16];
            snprintf(num, sizeof(num), "%u", pin->number);
            if (write(fd, num, strlen(num)) < 0) {
                /* best effort; a later boot clears it anyway */
            }
            close(fd);
        }
        pin->exported = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* struct i2c_gpio_ops implementation                                         */
/* ------------------------------------------------------------------------- */

static struct gpio_pin *pin_for(struct sysfs_gpio *gpio, enum i2c_line line)
{
    return (line == I2C_LINE_SCL) ? &gpio->scl : &gpio->sda;
}

static int sysfs_line_low(void *ctx, enum i2c_line line)
{
    struct sysfs_gpio *gpio = ctx;
    struct gpio_pin *pin = pin_for(gpio, line);

    /*
     * Direction first, then the value. This is forced by the kernel, and it is
     * the opposite of what a bare-metal GPIO driver would do.
     *
     * Writing value while the pin is an input fails on Linux with EPERM
     * ("write error: Operation not permitted" from the shell). The natural
     * order for open-drain - set the latch to 0, then flip to output, so the
     * pin never drives the stale latch value even for a moment - simply cannot
     * be expressed here: the write fails, line_low returns -1, and the bus
     * never comes up.
     *
     * That is exactly what happened on hardware. The symptom was 225 io errors
     * and every address reporting "transport error" while the Python
     * implementation, on the same pins and the same part, found 0x68 without
     * complaint. The Python code writes direction first; that difference was
     * the whole bug, and no compiler warning fires because there is no warning
     * for "your GPIO write silently returned EPERM".
     *
     * The cost of the correct order is one transient: between direction=out
     * and value=0 the pin drives whatever the latch last held. The kernel
     * clears the output latch when a pin is set to input, and sysfs_gpio_open
     * leaves both lines as inputs before the first START, so the stale value
     * is low rather than high. Low is what a released I2C line looks like
     * anyway, so the transient is harmless here - but it is worth knowing it
     * exists, because on a pin whose latch could hold 1 it would be a narrow
     * high pulse on the bus.
     */
    if (pin_set_direction(pin, "out") != 0)
        return -1;
    if (write_attr(pin, pin->val_fd, "0") != 0)
        return -1;
    return 0;
}

static int sysfs_line_release(void *ctx, enum i2c_line line)
{
    struct sysfs_gpio *gpio = ctx;
    struct gpio_pin *pin = pin_for(gpio, line);

    /* Open drain: "high" means "stop driving" and let the pull-up do it. */
    return pin_set_direction(pin, "in");
}

static int sysfs_line_read(void *ctx, enum i2c_line line)
{
    struct sysfs_gpio *gpio = ctx;
    struct gpio_pin *pin = pin_for(gpio, line);
    char buf[8];
    ssize_t n;

    /*
     * Reading a pin that is still configured as an output returns the latch we
     * last wrote, not the voltage on the wire - the kernel does not sample the
     * pad for us. Every ACK would then look like whatever we happened to send,
     * which is a protocol bug that presents as a target that always replies
     * "yes".
     *
     * The timing layer happens to release SDA before each read in read_byte,
     * but that is a property of the current call sequence, not something this
     * function can rely on. Asserting it here costs one redundant direction
     * write on the paths where it is already an input - a few microseconds
     * against a 25 us half period that the bus does not need anyway - and
     * turns a subtle, timing-dependent corruption into a guaranteed-correct
     * read.
     */
    if (pin_set_direction(pin, "in") != 0)
        return -1;

    if (lseek(pin->val_fd, 0, SEEK_SET) < 0)
        return -1;
    n = read(pin->val_fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return -1;
    buf[n] = '\0';

    return (buf[0] == '1') ? 1 : 0;
}

static int sysfs_delay(void *ctx)
{
    struct sysfs_gpio *gpio = ctx;

    while (nanosleep(&gpio->half_period, &gpio->half_period) < 0) {
        if (errno != EINTR) {
            /* Restore the period so the next call is not a zero-length sleep. */
            return -1;
        }
    }
    return 0;
}

static void sysfs_release_all(void *ctx)
{
    struct sysfs_gpio *gpio = ctx;

    (void)pin_set_direction(&gpio->scl, "in");
    (void)pin_set_direction(&gpio->sda, "in");
}

static const struct i2c_gpio_ops sysfs_ops = {
    .line_low = sysfs_line_low,
    .line_release = sysfs_line_release,
    .line_read = sysfs_line_read,
    .delay = sysfs_delay,
    .release_all = sysfs_release_all,
};

/* ------------------------------------------------------------------------- */
/* Public interface                                                           */
/* ------------------------------------------------------------------------- */

struct sysfs_gpio *sysfs_gpio_open(unsigned scl, unsigned sda,
                                   unsigned half_period_us);

void sysfs_gpio_close(struct sysfs_gpio *gpio);

const struct i2c_gpio_ops *sysfs_gpio_ops(void);

struct sysfs_gpio *sysfs_gpio_open(unsigned scl, unsigned sda,
                                   unsigned half_period_us)
{
    struct sysfs_gpio *gpio = calloc(1, sizeof(*gpio));

    if (gpio == NULL)
        return NULL;

    gpio->scl.dir_fd = -1;
    gpio->scl.val_fd = -1;
    gpio->sda.dir_fd = -1;
    gpio->sda.val_fd = -1;
    gpio->half_period.tv_sec = half_period_us / 1000000u;
    gpio->half_period.tv_nsec = (long)(half_period_us % 1000000u) * 1000L;

    if (pin_open(&gpio->scl, scl) != 0)
        goto fail;
    if (pin_open(&gpio->sda, sda) != 0)
        goto fail;

    /*
     * Both lines idle high (released) before the first START. Leaving them
     * driven low from a previous run would look like a busy bus to any other
     * master and to the target itself.
     */
    if (pin_set_direction(&gpio->scl, "in") != 0)
        goto fail;
    if (pin_set_direction(&gpio->sda, "in") != 0)
        goto fail;

    return gpio;

fail:
    {
        int saved = errno;

        pin_close(&gpio->scl);
        pin_close(&gpio->sda);
        free(gpio);
        errno = saved;
    }
    return NULL;
}

void sysfs_gpio_close(struct sysfs_gpio *gpio)
{
    if (gpio == NULL)
        return;
    (void)pin_set_direction(&gpio->scl, "in");
    (void)pin_set_direction(&gpio->sda, "in");
    pin_close(&gpio->scl);
    pin_close(&gpio->sda);
    free(gpio);
}

const struct i2c_gpio_ops *sysfs_gpio_ops(void)
{
    return &sysfs_ops;
}

#endif /* __linux__ */
