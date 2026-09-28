/*
 * Bit-banged I2C timing. See i2c_bitbang.h for why this is not /dev/i2c-N.
 *
 * Everything here goes through struct i2c_gpio_ops, so this file has no
 * dependency on Linux, sysfs, or time. That is what lets tests/test_i2c_bitbang.c
 * drive it with a recording fake and assert on the actual edge sequence.
 */

#include "i2c_bitbang.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Low level line handling                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Every backend call is funnelled through these two so the error accounting in
 * one place. Returning I2C_BB_EIO rather than 0 matters: a wire that reads low
 * because a target is acknowledging looks identical to a wire that reads low
 * because the file handle broke, and only the errno can tell them apart.
 */
static int bb_call(struct i2c_bitbang *bus, int rc)
{
    if (rc != 0) {
        bus->io_errors++;
        return I2C_BB_EIO;
    }
    return I2C_BB_OK;
}

static int scl_low(struct i2c_bitbang *bus)
{
    return bb_call(bus, bus->ops->line_low(bus->ctx, I2C_LINE_SCL));
}

static int scl_release(struct i2c_bitbang *bus)
{
    return bb_call(bus, bus->ops->line_release(bus->ctx, I2C_LINE_SCL));
}

static int sda_low(struct i2c_bitbang *bus)
{
    return bb_call(bus, bus->ops->line_low(bus->ctx, I2C_LINE_SDA));
}

static int sda_release(struct i2c_bitbang *bus)
{
    return bb_call(bus, bus->ops->line_release(bus->ctx, I2C_LINE_SDA));
}

/* Returns 1 high, 0 low, I2C_BB_EIO on a read error. */
static int sda_read(struct i2c_bitbang *bus)
{
    int v = bus->ops->line_read(bus->ctx, I2C_LINE_SDA);

    if (v < 0) {
        bus->io_errors++;
        return I2C_BB_EIO;
    }
    return v;
}

static int tick(struct i2c_bitbang *bus)
{
    return bb_call(bus, bus->ops->delay(bus->ctx));
}

/*
 * One clock pulse: raise SCL, hold, lower it.
 *
 * The hold time is what gives the target its window to do anything with the
 * bit, so this is where the half period is actually spent.
 */
static int clock_pulse(struct i2c_bitbang *bus)
{
    int rc;

    if ((rc = scl_release(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = tick(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = scl_low(bus)) != I2C_BB_OK)
        return rc;
    return tick(bus);
}

/* ------------------------------------------------------------------------- */
/* Frame level primitives                                                     */
/* ------------------------------------------------------------------------- */

static int send_start(struct i2c_bitbang *bus)
{
    int rc;

    /*
     * START is defined as SDA falling while SCL is high. SDA is released
     * first so that the high-to-low transition is a real edge and not a no-op
     * on a line that was already low from the previous transaction.
     */
    if ((rc = sda_release(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = scl_release(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = tick(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = sda_low(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = tick(bus)) != I2C_BB_OK)
        return rc;
    return scl_low(bus);
}

static int send_stop(struct i2c_bitbang *bus)
{
    int rc;

    /* STOP is SDA rising while SCL is high: the mirror of START. */
    if ((rc = sda_low(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = tick(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = scl_release(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = tick(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = sda_release(bus)) != I2C_BB_OK)
        return rc;
    return tick(bus);
}

/*
 * Shift out one byte, most significant bit first, then read the ACK slot.
 *
 * Data is set up while SCL is low so it is stable when the target samples it
 * on the rising edge. Setting it after raising SCL would race the target.
 */
static int write_byte(struct i2c_bitbang *bus, uint8_t byte, int *acked)
{
    int rc;

    for (int i = 0; i < 8; i++) {
        if (byte & 0x80) {
            if ((rc = sda_release(bus)) != I2C_BB_OK)
                return rc;
        } else {
            if ((rc = sda_low(bus)) != I2C_BB_OK)
                return rc;
        }
        byte = (uint8_t)(byte << 1);

        if ((rc = clock_pulse(bus)) != I2C_BB_OK)
            return rc;
    }

    /*
     * The ninth clock belongs to the target: it pulls SDA low to acknowledge.
     * The master must release the line for that whole period, otherwise the
     * master's own low would look like an ACK it sent itself.
     */
    if ((rc = sda_release(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = scl_release(bus)) != I2C_BB_OK)
        return rc;
    if ((rc = tick(bus)) != I2C_BB_OK)
        return rc;

    {
        int v = sda_read(bus);

        if (v == I2C_BB_EIO)
            return I2C_BB_EIO;
        *acked = (v == 0);
    }

    if ((rc = scl_low(bus)) != I2C_BB_OK)
        return rc;
    return tick(bus);
}

/*
 * Shift in one byte, then drive the ACK slot ourselves.
 *
 * The caller passes ack=1 for all but the final byte. NACKing the last byte is
 * how the master tells the target to stop driving, and it is not optional:
 * a target that keeps sending after a NACK is undefined, and one that is
 * NACKed early may hold the bus.
 */
static int read_byte(struct i2c_bitbang *bus, int ack, uint8_t *out)
{
    uint8_t value = 0;
    int rc;

    for (int i = 0; i < 8; i++) {
        int v;

        value = (uint8_t)(value << 1);

        /*
         * The line is sampled while SCL is high, which is the window the
         * target guarantees the data is valid in. Sampling on the falling
         * edge instead would read the bit the target has already started to
         * replace, which works until the bus gets slow enough for the two to
         * overlap.
         */
        if ((rc = scl_release(bus)) != I2C_BB_OK)
            return rc;
        if ((rc = tick(bus)) != I2C_BB_OK)
            return rc;

        v = sda_read(bus);
        if (v == I2C_BB_EIO)
            return I2C_BB_EIO;
        if (v == 1)
            value |= 1;

        if ((rc = scl_low(bus)) != I2C_BB_OK)
            return rc;
        if ((rc = tick(bus)) != I2C_BB_OK)
            return rc;
    }

    *out = value;

    /*
     * The ninth clock is ours. Driving it low acknowledges, releasing it
     * NACKs. The release case especially must not skip the clock: a target
     * waiting on that edge to advance would be left mid-byte.
     */
    if (ack)
        rc = sda_low(bus);
    else
        rc = sda_release(bus);
    if (rc != I2C_BB_OK)
        return rc;

    if ((rc = clock_pulse(bus)) != I2C_BB_OK)
        return rc;

    /*
     * Release SDA afterwards even when we just drove it low for an ACK. The
     * next thing that touches the line is the target or the next START, and
     * both need it free.
     */
    return sda_release(bus);
}

static int write_address(struct i2c_bitbang *bus, uint8_t address, int read)
{
    uint8_t byte = (uint8_t)((address << 1) | (read ? 1 : 0));
    int acked = 0;
    int rc = write_byte(bus, byte, &acked);

    if (rc != I2C_BB_OK)
        return rc;
    if (!acked) {
        bus->ack_failures++;
        return I2C_BB_ENOACK;
    }
    return I2C_BB_OK;
}

/*
 * Send a byte and treat a missing ACK as a failure.
 *
 * Separate from write_byte because the ACK of an address byte and the ACK of a
 * data byte mean different things: the first says "a device is there", the
 * second says "the device accepted this value". Only the first is worth
 * counting as an ack_failure.
 */
static int write_byte_checked(struct i2c_bitbang *bus, uint8_t byte)
{
    int acked = 0;
    int rc = write_byte(bus, byte, &acked);

    if (rc != I2C_BB_OK)
        return rc;
    if (!acked) {
        bus->ack_failures++;
        return I2C_BB_ENOACK;
    }
    return I2C_BB_OK;
}

/* ------------------------------------------------------------------------- */
/* Public interface                                                           */
/* ------------------------------------------------------------------------- */

int i2c_bb_init(struct i2c_bitbang *bus,
                const struct i2c_gpio_ops *ops,
                void *ctx)
{
    if (bus == NULL || ops == NULL)
        return I2C_BB_EINVAL;

    memset(bus, 0, sizeof(*bus));
    bus->ops = ops;
    bus->ctx = ctx;
    return I2C_BB_OK;
}

void i2c_bb_release(struct i2c_bitbang *bus)
{
    if (bus == NULL || bus->ops == NULL)
        return;
    bus->ops->release_all(bus->ctx);
}

int i2c_bb_bus_recover(struct i2c_bitbang *bus)
{
    int rc;

    if (bus == NULL || bus->ops == NULL)
        return I2C_BB_EINVAL;

    /*
     * Up to nine clocks, because a target interrupted part way through a byte
     * may still be holding SDA low expecting the rest of it. Once it has seen
     * enough clocks it releases, and the STOP returns the bus to idle.
     */
    if ((rc = sda_release(bus)) != I2C_BB_OK)
        return rc;

    for (int i = 0; i < 9; i++) {
        if ((rc = clock_pulse(bus)) != I2C_BB_OK)
            return rc;
    }

    return send_stop(bus);
}

int i2c_bb_read_regs(struct i2c_bitbang *bus,
                     uint8_t address,
                     uint8_t reg,
                     uint8_t *out,
                     size_t length)
{
    int rc;

    if (bus == NULL || out == NULL || length == 0)
        return I2C_BB_EINVAL;

    bus->transactions++;

    if ((rc = send_start(bus)) != I2C_BB_OK)
        goto out;
    if ((rc = write_address(bus, address, 0)) != I2C_BB_OK)
        goto out;
    if ((rc = write_byte_checked(bus, reg)) != I2C_BB_OK)
        goto out;

    /* Repeated START, no STOP: some parts reset their pointer on a STOP. */
    if ((rc = send_start(bus)) != I2C_BB_OK)
        goto out;
    if ((rc = write_address(bus, address, 1)) != I2C_BB_OK)
        goto out;

    for (size_t i = 0; i < length; i++) {
        int last = (i + 1 == length);

        if ((rc = read_byte(bus, !last, &out[i])) != I2C_BB_OK)
            goto out;
    }

    rc = I2C_BB_OK;

out:
    /*
     * Always leave the bus idle, success or failure. Bailing out with SCL low
     * would leave the target waiting for a clock edge and wedge the bus for
     * every later access, turning one bad read into a permanent outage.
     */
    send_stop(bus);
    if (rc != I2C_BB_OK)
        i2c_bb_release(bus);
    return rc;
}

int i2c_bb_write_regs(struct i2c_bitbang *bus,
                      uint8_t address,
                      uint8_t reg,
                      const uint8_t *values,
                      size_t length)
{
    int rc;

    if (bus == NULL || values == NULL || length == 0)
        return I2C_BB_EINVAL;

    bus->transactions++;

    if ((rc = send_start(bus)) != I2C_BB_OK)
        goto out;
    if ((rc = write_address(bus, address, 0)) != I2C_BB_OK)
        goto out;
    if ((rc = write_byte_checked(bus, reg)) != I2C_BB_OK)
        goto out;

    for (size_t i = 0; i < length; i++) {
        if ((rc = write_byte_checked(bus, values[i])) != I2C_BB_OK)
            goto out;
    }

    rc = I2C_BB_OK;

out:
    send_stop(bus);
    if (rc != I2C_BB_OK)
        i2c_bb_release(bus);
    return rc;
}

int i2c_bb_probe_address(struct i2c_bitbang *bus, uint8_t address)
{
    int acked = 0;
    int rc;

    if (bus == NULL)
        return I2C_BB_EINVAL;

    bus->transactions++;

    if ((rc = send_start(bus)) != I2C_BB_OK)
        goto failed;
    rc = write_byte(bus, (uint8_t)(address << 1), &acked);
    if (rc != I2C_BB_OK)
        goto failed;

    send_stop(bus);
    return acked ? 1 : 0;

failed:
    send_stop(bus);
    i2c_bb_release(bus);
    return rc;
}
