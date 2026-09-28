/*
 * Host side self test for the bit-banged I2C timing.
 *
 * Built and run natively (no board, no sysfs):
 *
 *     make test-i2c-bitbang
 *
 * The gpio backend is replaced with a recorder that logs every transition
 * instead of touching a filesystem, and an emulated target answers on SDA.
 * That combination is what makes the interesting assertions possible: not just
 * "the call returned 0" but "the wire actually carried a START, the right
 * address with the right read bit, and a NACK on the final byte".
 *
 * These are the checks that matter. A bit-bang master that returns success
 * while producing the wrong edges looks perfectly healthy from the caller's
 * side and only fails against real silicon, usually intermittently.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i2c_bitbang.h"
#include "mpu6050_gpio.h"

static int g_checks;
static int g_failures;

#define CHECK(condition, format, ...)                                      \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(condition)) {                                                \
            g_failures++;                                                  \
            printf("FAIL line %d: " format "\n", __LINE__, ##__VA_ARGS__);  \
        }                                                                  \
    } while (0)

/* ------------------------------------------------------- edge recorder */

/*
 * Every mutation of the wire is appended here. A "release" is recorded as a
 * level change to 1 because that is what the pull-up does; the distinction
 * between "driven high" and "released" is not visible on the wire and the
 * protocol only ever depends on the resulting level.
 */
enum edge_kind {
    EDGE_LOW = 0,   /* driven low */
    EDGE_HIGH = 1,  /* released, pull-up takes it high */
    EDGE_SAMPLE = 2 /* a read of SDA, recorded so interleaving can be checked */
};

struct edge {
    enum i2c_line line;
    enum edge_kind kind;
    uint8_t sampled; /* only meaningful for EDGE_SAMPLE */
};

#define MAX_EDGES 4096

struct recorder {
    struct edge edges[MAX_EDGES];
    size_t count;
    int level[2];      /* current level per line, 1 = high */
    int overflow;
    int fail_next_read; /* force a backend read error */

    /*
     * Emulated target. When enabled it answers its address and NACKs data,
     * which is all the master logic needs to be exercised end to end.
     */
    int target_enabled;
    uint8_t target_address;
    int ack_next_byte; /* 1 = pull SDA low on the coming ACK slot */
};

static void rec_push(struct recorder *r, enum i2c_line line, enum edge_kind k)
{
    if (r->count >= MAX_EDGES) {
        r->overflow = 1;
        return;
    }
    r->edges[r->count].line = line;
    r->edges[r->count].kind = k;
    r->edges[r->count].sampled = 0;
    r->count++;
}

/*
 * Detect START and STOP by the definition itself: a transition on SDA while
 * SCL is high.
 *
 * Note this tracks the *level* of SCL rather than looking for a preceding SCL
 * edge. That distinction matters: the first START of a transaction happens
 * when SCL is already high from the idle state, so there is no preceding rise
 * to look back for. An edge-based detector silently undercounts by one, which
 * is a bug in the test rather than in the driver, and the kind that wastes an
 * afternoon because the failure looks like a driver fault.
 */
static int count_starts(const struct recorder *r)
{
    int count = 0;
    int scl_level = 1; /* idle bus starts high */
    int sda_level = 1;
    int prev_sda = 1;

    for (size_t i = 0; i < r->count; i++) {
        const struct edge *e = &r->edges[i];

        if (e->kind == EDGE_SAMPLE)
            continue;

        if (e->line == I2C_LINE_SCL) {
            scl_level = (e->kind == EDGE_LOW) ? 0 : 1;
        } else {
            prev_sda = sda_level;
            sda_level = (e->kind == EDGE_LOW) ? 0 : 1;
            if (scl_level == 1 && prev_sda == 1 && sda_level == 0)
                count++;
        }
    }
    return count;
}

static int count_stops(const struct recorder *r)
{
    int count = 0;
    int scl_level = 1;
    int sda_level = 1;
    int prev_sda = 1;

    for (size_t i = 0; i < r->count; i++) {
        const struct edge *e = &r->edges[i];

        if (e->kind == EDGE_SAMPLE)
            continue;

        if (e->line == I2C_LINE_SCL) {
            scl_level = (e->kind == EDGE_LOW) ? 0 : 1;
        } else {
            prev_sda = sda_level;
            sda_level = (e->kind == EDGE_LOW) ? 0 : 1;
            if (scl_level == 1 && prev_sda == 0 && sda_level == 1)
                count++;
        }
    }
    return count;
}

/*
 * Decode the bytes the master shifted out.
 *
 * Bits are sampled on the SCL rising edge, which is where a real target looks
 * too, so this is a faithful decode rather than a reconstruction from the
 * master's intentions.
 */
/*
 * Decode the bytes the master shifted out.
 *
 * A real target samples SDA on the SCL rising edge, so this does too, with two
 * details that a naive version gets wrong and that are worth spelling out
 * because both produce plausible garbage rather than an obvious failure.
 *
 * First, the counter runs 0..8 per byte. The ninth clock is the ACK slot and
 * belongs to the receiver; treating it as data shifts every later byte by one
 * bit.
 *
 * Second, the clock activity inside START and STOP must not be counted at all.
 * A START is SDA falling while SCL is high, and it is followed by SCL going
 * low then high again - that first high is not a data bit, but a decoder that
 * only looks at SCL edges will happily record it as one. Detecting the frame
 * boundaries and resetting the bit counter there is what keeps the alignment.
 */
static size_t decode_written_bytes(const struct recorder *r,
                                   uint8_t *out, size_t max)
{
    size_t nbytes = 0;
    int tick = 0;       /* 0..7 data bits, 8 = ACK slot */
    uint8_t value = 0;
    int scl_level = 1;
    int sda_level = 1;
    int prev_sda = 1;
    int active = 1;     /* idle bus counts as active; a STOP ends the frame */

    for (size_t i = 0; i < r->count; i++) {
        const struct edge *e = &r->edges[i];

        if (e->kind == EDGE_SAMPLE)
            continue;

        if (e->line == I2C_LINE_SCL) {
            int level = (e->kind == EDGE_LOW) ? 0 : 1;

            if (level == 1 && scl_level == 0 && active && tick < 8) {
                value = (uint8_t)((value << 1) | (uint8_t)sda_level);
                tick++;
                if (tick == 8) {
                    if (nbytes < max)
                        out[nbytes] = value;
                    nbytes++;
                }
            } else if (level == 1 && scl_level == 0 && active && tick == 8) {
                /* ACK slot completed. */
                tick = 0;
                value = 0;
            }
            scl_level = level;
            continue;
        }

        /* SDA changed: look for a frame boundary at the current SCL level. */
        prev_sda = sda_level;
        sda_level = (e->kind == EDGE_LOW) ? 0 : 1;

        if (scl_level == 1 && prev_sda == 1 && sda_level == 0) {
            /* START: alignment restarts here. */
            active = 1;
            tick = 0;
            value = 0;
        } else if (scl_level == 1 && prev_sda == 0 && sda_level == 1) {
            /* STOP: nothing after this belongs to the frame. */
            active = 0;
            tick = 0;
            value = 0;
        }
    }
    return nbytes;
}

/*
 * Level of SDA during the last clock pulse, which is the final ACK/NACK slot
 * the master drove.
 *
 * Found by scanning backwards for the last SCL fall and reporting SDA as it
 * was just before that. Sampling "the last SDA value in the log" would be
 * wrong here, because the master releases SDA again right after the slot and
 * the release is exactly what we are trying to distinguish from a drive-low.
 */
static int last_ack_slot_sda(const struct recorder *r)
{
    int sda_level = 1;

    for (size_t i = r->count; i-- > 0;) {
        const struct edge *e = &r->edges[i];

        if (e->kind == EDGE_SAMPLE)
            continue;
        if (e->line == I2C_LINE_SCL && e->kind == EDGE_LOW) {
            /* SDA as it stood during the high phase just ended. */
            for (size_t j = i; j-- > 0;) {
                const struct edge *p = &r->edges[j];
                if (p->kind == EDGE_SAMPLE || p->line != I2C_LINE_SDA)
                    continue;
                return (p->kind == EDGE_LOW) ? 0 : 1;
            }
            return sda_level;
        }
        if (e->line == I2C_LINE_SDA && e->kind != EDGE_SAMPLE)
            sda_level = (e->kind == EDGE_LOW) ? 0 : 1;
    }
    return sda_level;
}

/* ------------------------------------------------------- ops implementation */

static int rec_line_low(void *ctx, enum i2c_line line)
{
    struct recorder *r = ctx;
    r->level[line] = 0;
    rec_push(r, line, EDGE_LOW);
    return 0;
}

static int rec_line_release(void *ctx, enum i2c_line line)
{
    struct recorder *r = ctx;
    r->level[line] = 1;
    rec_push(r, line, EDGE_HIGH);
    return 0;
}

/*
 * Read SDA.
 *
 * This is where several real bugs would hide: if the master fails to release
 * SDA before the ACK slot, its own low would be read back and it would think
 * every byte was acknowledged. The emulation below makes that observable by
 * only ever pulling SDA low when the master has released it.
 */
static int rec_line_read(void *ctx, enum i2c_line line)
{
    struct recorder *r = ctx;
    int value;

    if (r->fail_next_read) {
        r->fail_next_read = 0;
        return -1;
    }

    if (line == I2C_LINE_SDA) {
        if (r->target_enabled && r->ack_next_byte) {
            /*
             * A real target can only pull low, so it can only ACK when the
             * master has let go. Modelling that constraint is the point: it
             * turns "master forgot to release" into a visible NACK.
             */
            value = r->level[I2C_LINE_SDA] ? 0 : 1;
        } else {
            value = r->level[I2C_LINE_SDA];
        }
    } else {
        value = r->level[line];
    }

    /* Record the sample so tests can see what was read and when. */
    if (r->count < MAX_EDGES) {
        r->edges[r->count].line = line;
        r->edges[r->count].kind = EDGE_SAMPLE;
        r->edges[r->count].sampled = (uint8_t)value;
        r->count++;
    }
    return value;
}

static int rec_delay(void *ctx)
{
    struct recorder *r = ctx;
    (void)r;
    return 0; /* the host test does not need to wait */
}

static void rec_release_all(void *ctx)
{
    struct recorder *r = ctx;
    r->level[I2C_LINE_SCL] = 1;
    r->level[I2C_LINE_SDA] = 1;
}

static const struct i2c_gpio_ops rec_ops = {
    .line_low = rec_line_low,
    .line_release = rec_line_release,
    .line_read = rec_line_read,
    .delay = rec_delay,
    .release_all = rec_release_all,
};

static void rec_reset(struct recorder *r)
{
    memset(r, 0, sizeof(*r));
    r->level[I2C_LINE_SCL] = 1;
    r->level[I2C_LINE_SDA] = 1;
}

/* ------------------------------------------------------- tests */

static void test_start_and_stop_shape(void)
{
    struct recorder r;
    struct i2c_bitbang bus;

    /*
     * An absent target must abort after the address byte rather than plowing
     * on. That means one START (not two) and still exactly one STOP: a
     * transaction that gives up must release the bus, otherwise the next
     * caller inherits a wedged line.
     */
    rec_reset(&r);
    CHECK(i2c_bb_init(&bus, &rec_ops, &r) == I2C_BB_OK, "init failed");

    uint8_t buf[1] = { 0 };
    int rc = i2c_bb_read_regs(&bus, 0x68, 0x75, buf, 1);

    CHECK(rc == I2C_BB_ENOACK, "absent target should be ENOACK, got %d", rc);
    CHECK(count_starts(&r) == 1,
          "absent target should abort after 1 START, got %d", count_starts(&r));
    CHECK(count_stops(&r) == 1,
          "even an aborted transaction must emit 1 STOP, got %d",
          count_stops(&r));

    /*
     * A present target runs both phases, joined by a repeated START rather
     * than a STOP. That is what the MPU6050 datasheet specifies, and it
     * matters on parts that reset their register pointer when they see a STOP.
     */
    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);
    r.target_enabled = 1;
    r.ack_next_byte = 1;

    rc = i2c_bb_read_regs(&bus, 0x68, 0x75, buf, 1);
    CHECK(rc == I2C_BB_OK, "present target should succeed, got %d", rc);
    CHECK(count_starts(&r) == 2,
          "a successful read needs 2 STARTs, got %d", count_starts(&r));
    CHECK(count_stops(&r) == 1,
          "a read is one transaction and ends with 1 STOP, got %d",
          count_stops(&r));
}

static void test_address_and_read_bit(void)
{
    struct recorder r;
    struct i2c_bitbang bus;
    uint8_t written[8];
    size_t got;

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);

    /* Target present so both phases run to completion. */
    r.target_enabled = 1;
    r.target_address = 0x68;
    r.ack_next_byte = 1;

    /*
     * Emulate a target that acknowledges everything. The point of this test is
     * the shifted-out bytes, so the read data itself is not checked here.
     */
    uint8_t buf[2] = { 0 };
    (void)i2c_bb_read_regs(&bus, 0x68, 0x3B, buf, 2);

    got = decode_written_bytes(&r, written, sizeof(written));

    /*
     * Expected sequence: 0xD0 (0x68 << 1 | write), 0x3B (register),
     * 0xD1 (0x68 << 1 | read). The read bit is the difference between the
     * first and third byte and is the single most common thing to get wrong.
     */
    CHECK(got >= 3, "expected at least 3 written bytes, got %d", (int)got);
    if (got >= 3) {
        CHECK(written[0] == 0xD0, "address+W should be 0xD0, got 0x%02X",
              written[0]);
        CHECK(written[1] == 0x3B, "register byte should be 0x3B, got 0x%02X",
              written[1]);
        CHECK(written[2] == 0xD1, "address+R should be 0xD1, got 0x%02X",
              written[2]);
    }
}

static void test_nack_on_final_byte(void)
{
    struct recorder r;
    struct i2c_bitbang bus;
    uint8_t buf[4] = { 0 };

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);

    r.target_enabled = 1;
    r.ack_next_byte = 1;

    (void)i2c_bb_read_regs(&bus, 0x68, 0x3B, buf, 4);

    /*
     * After the last byte the master must NACK: leave SDA released so the
     * target knows to stop driving. If the master drove SDA low there, a
     * target that keeps counting clocks would keep streaming data and the
     * transaction would never end cleanly.
     *
     * Checked by looking at the very last SDA level the master established:
     * it must be high (released), not low.
     */
    int final_sda = last_ack_slot_sda(&r);
    CHECK(final_sda == 1,
          "SDA should be left released (high) after the final byte, got %d",
          final_sda);
}

static void test_write_sequence(void)
{
    struct recorder r;
    struct i2c_bitbang bus;
    uint8_t written[8];
    size_t got;

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);

    r.target_enabled = 1;
    r.ack_next_byte = 1;

    static const uint8_t values[2] = { 0x01, 0x00 };
    (void)i2c_bb_write_regs(&bus, 0x68, 0x6B, values, 2);

    got = decode_written_bytes(&r, written, sizeof(written));

    CHECK(got >= 4, "expected 4 written bytes, got %d", (int)got);
    if (got >= 4) {
        CHECK(written[0] == 0xD0, "address+W should be 0xD0, got 0x%02X",
              written[0]);
        CHECK(written[1] == 0x6B, "register should be 0x6B, got 0x%02X",
              written[1]);
        CHECK(written[2] == 0x01, "value 0 should be 0x01, got 0x%02X",
              written[2]);
        CHECK(written[3] == 0x00, "value 1 should be 0x00, got 0x%02X",
              written[3]);
    }

    /* A write is one frame: exactly one START and one STOP. */
    CHECK(count_starts(&r) == 1, "write should have 1 START, got %d",
          count_starts(&r));
}

static void test_msb_first(void)
{
    struct recorder r;
    struct i2c_bitbang bus;
    uint8_t written[8];
    size_t got;

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);
    r.target_enabled = 1;
    r.ack_next_byte = 1;

    /* 0x81 has the top and bottom bits set, so bit order is unambiguous. */
    static const uint8_t values[1] = { 0x81 };
    (void)i2c_bb_write_regs(&bus, 0x68, 0x19, values, 1);

    got = decode_written_bytes(&r, written, sizeof(written));
    CHECK(got >= 3, "expected 3 bytes, got %d", (int)got);
    if (got >= 3)
        CHECK(written[2] == 0x81, "byte should survive as 0x81, got 0x%02X",
              written[2]);
}

static void test_bus_recover_pulses_nine_clocks(void)
{
    struct recorder r;
    struct i2c_bitbang bus;

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);

    CHECK(i2c_bb_bus_recover(&bus) == I2C_BB_OK, "recover failed");

    /*
     * Nine clocks because a target stuck mid-byte needs up to eight to finish
     * the byte it thinks it is in, plus one to see the STOP condition.
     * Fewer than nine leaves some targets still holding SDA.
     */
    int rises = 0;
    for (size_t i = 0; i < r.count; i++) {
        if (r.edges[i].line == I2C_LINE_SCL && r.edges[i].kind == EDGE_HIGH)
            rises++;
    }
    CHECK(rises >= 9, "recover should clock at least 9 times, got %d", rises);
    CHECK(count_stops(&r) >= 1, "recover should end with a STOP");
}

static void test_bus_released_on_error(void)
{
    struct recorder r;
    struct i2c_bitbang bus;

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);

    /* Prime the next SDA read to fail, then attempt a transaction. */
    r.fail_next_read = 1;

    uint8_t buf[1] = { 0 };
    int rc = i2c_bb_read_regs(&bus, 0x68, 0x75, buf, 1);

    CHECK(rc == I2C_BB_EIO, "backend read failure should surface as EIO, got %d",
          rc);

    /*
     * The critical property: both lines must end released. A transaction that
     * bails out holding SCL low leaves the target waiting for a clock edge
     * forever, which turns one transient error into a permanently dead bus.
     */
    CHECK(r.level[I2C_LINE_SCL] == 1, "SCL must be released after an error");
    CHECK(r.level[I2C_LINE_SDA] == 1, "SDA must be released after an error");
}

static void test_probe_reports_absent(void)
{
    struct recorder r;
    struct i2c_bitbang bus;

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);

    /* Nothing on the bus: SDA stays high during the ACK slot. */
    CHECK(i2c_bb_probe_address(&bus, 0x68) == 0, "absent address should be 0");
    CHECK(count_stops(&r) >= 1, "probe should still emit a STOP");
}

static void test_probe_reports_present(void)
{
    struct recorder r;
    struct i2c_bitbang bus;

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);

    r.target_enabled = 1;
    r.ack_next_byte = 1;

    CHECK(i2c_bb_probe_address(&bus, 0x68) == 1, "present address should be 1");
}

static void test_counters_track_failures(void)
{
    struct recorder r;
    struct i2c_bitbang bus;

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);

    uint8_t buf[1] = { 0 };
    (void)i2c_bb_read_regs(&bus, 0x68, 0x75, buf, 1);
    (void)i2c_bb_read_regs(&bus, 0x69, 0x75, buf, 1);

    CHECK(bus.transactions == 2, "expected 2 transactions, got %u",
          bus.transactions);
    /*
     * The ACK failures are the diagnostic that separates "wrong address" from
     * "no bus at all". They must be counted, not swallowed.
     */
    CHECK(bus.ack_failures >= 1, "ack_failures should be counted");
    CHECK(bus.io_errors == 0, "a NACK is not an io error, got %u",
          bus.io_errors);
}

static void test_argument_validation(void)
{
    struct recorder r;
    struct i2c_bitbang bus;
    uint8_t buf[4] = { 0 };

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);

    CHECK(i2c_bb_init(NULL, &rec_ops, &r) == I2C_BB_EINVAL,
          "init should reject a NULL bus");
    CHECK(i2c_bb_init(&bus, NULL, &r) == I2C_BB_EINVAL,
          "init should reject NULL ops");
    CHECK(i2c_bb_read_regs(&bus, 0x68, 0x00, NULL, 1) == I2C_BB_EINVAL,
          "read should reject a NULL buffer");
    CHECK(i2c_bb_read_regs(&bus, 0x68, 0x00, buf, 0) == I2C_BB_EINVAL,
          "read should reject zero length");
    CHECK(i2c_bb_write_regs(&bus, 0x68, 0x00, NULL, 1) == I2C_BB_EINVAL,
          "write should reject a NULL buffer");
}

static void test_return_codes_distinguish_causes(void)
{
    struct recorder r;
    struct i2c_bitbang bus;
    uint8_t buf[1] = { 0 };

    /*
     * ENOACK and EIO must not be collapsed into one generic failure. The first
     * means "the wiring or the address is wrong", the second means "the driver
     * cannot drive the pins". They lead to completely different fixes, and a
     * caller that cannot tell them apart has to guess.
     */
    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);
    CHECK(i2c_bb_read_regs(&bus, 0x68, 0x75, buf, 1) == I2C_BB_ENOACK,
          "silent bus should be ENOACK");

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);
    r.fail_next_read = 1;
    CHECK(i2c_bb_read_regs(&bus, 0x68, 0x75, buf, 1) == I2C_BB_EIO,
          "broken backend should be EIO");
}

static void test_overflow_guard_does_not_crash(void)
{
    struct recorder r;
    struct i2c_bitbang bus;

    rec_reset(&r);
    i2c_bb_init(&bus, &rec_ops, &r);
    /*
     * A present target so each transaction runs to completion instead of
     * bailing out at the first NACK. An absent bus stops after one byte and
     * never produces enough edges to reach the limit, which would make this
     * test pass without exercising anything.
     */
    r.target_enabled = 1;
    r.ack_next_byte = 1;

    uint8_t buf[16] = { 0 };
    for (int i = 0; i < 40; i++)
        (void)i2c_bb_read_regs(&bus, 0x68, 0x3B, buf, sizeof(buf));

    CHECK(r.overflow == 1, "recorder should have reported overflow");
    CHECK(r.count <= MAX_EDGES, "recorder must not write past its buffer");
}

/* ------------------------------------------------------- mpu6050 identity */

/*
 * Included here rather than in test_mpu6050.c because it is about a policy
 * decision the transport depends on, not about decoding maths.
 */
int mpu6050_who_am_i_supported(uint8_t who);
const char *mpu6050_who_am_i_name(uint8_t who);

static void test_who_am_i_family()
{
    /*
     * The module on the bench answers 0x70. This test exists so that a future
     * change cannot quietly re-tighten the check to 0x68 only, which would
     * reject a sensor that reads back perfect physics.
     */
    CHECK(mpu6050_who_am_i_supported(0x68), "MPU6050 should be supported");
    CHECK(mpu6050_who_am_i_supported(0x70), "MPU6500 should be supported");
    CHECK(mpu6050_who_am_i_supported(0x71), "MPU9250 should be supported");
    CHECK(mpu6050_who_am_i_supported(0x73), "MPU9255 should be supported");
    CHECK(!mpu6050_who_am_i_supported(0x00), "0x00 must not be accepted");
    CHECK(!mpu6050_who_am_i_supported(0xFF), "0xFF must not be accepted");

    CHECK(mpu6050_who_am_i_name(0x68) != NULL, "0x68 should have a name");
    CHECK(mpu6050_who_am_i_name(0x70) != NULL, "0x70 should have a name");
    CHECK(mpu6050_who_am_i_name(0x12) == NULL, "unknown id should have no name");
}

/* ------------------------------------------------------- main */

int main(void)
{
    printf("i2c bitbang timing tests\n");

    test_start_and_stop_shape();
    test_address_and_read_bit();
    test_nack_on_final_byte();
    test_write_sequence();
    test_msb_first();
    test_bus_recover_pulses_nine_clocks();
    test_bus_released_on_error();
    test_probe_reports_absent();
    test_probe_reports_present();
    test_counters_track_failures();
    test_argument_validation();
    test_return_codes_distinguish_causes();
    test_overflow_guard_does_not_crash();
    test_who_am_i_family();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
