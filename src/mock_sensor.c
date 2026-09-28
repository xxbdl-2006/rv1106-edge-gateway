#include "mock_sensor.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sensor_attitude.h"
#include "sensor_math.h"

#define MOCK_DEFAULT_STEP_US 10000U    /* 100 Hz */
#define MOCK_DEFAULT_PERIOD 300U       /* 3 s at 100 Hz */
#define MOCK_DEFAULT_AMPLITUDE 30.0f   /* degrees */

#define DEG_TO_RAD 0.0174532925199f

struct mock_sensor {
    enum mock_sensor_mode mode;
    float amplitude_deg;
    unsigned period_samples;
    uint64_t step_us;

    unsigned fail_after_samples;
    unsigned fail_duration_samples;
    bool repeat_failures;

    /* Phase counter: how many samples have been produced. */
    uint64_t index;
    /* How many of the current failure window have been consumed. */
    unsigned failing_remaining;

    unsigned long samples;
    unsigned long errors;
};

static int mock_read(void *context, struct sensor_sample *out);
static void mock_close_adapter(void *context);
static unsigned long mock_read_errors(void *context);

int mock_sensor_open(const struct mock_sensor_config *config,
                     struct mock_sensor **sensor)
{
    struct mock_sensor *created;

    if (sensor == NULL) {
        errno = EINVAL;
        return -1;
    }
    *sensor = NULL;

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        errno = ENOMEM;
        return -1;
    }

    if (config != NULL) {
        created->mode = config->mode;
        created->amplitude_deg = config->amplitude_deg != 0.0f
                                     ? config->amplitude_deg
                                     : MOCK_DEFAULT_AMPLITUDE;
        created->period_samples = config->period_samples != 0U
                                      ? config->period_samples
                                      : MOCK_DEFAULT_PERIOD;
        created->step_us = config->step_us != 0U ? config->step_us
                                                 : MOCK_DEFAULT_STEP_US;
        created->fail_after_samples = config->fail_after_samples;
        created->fail_duration_samples = config->fail_duration_samples;
        created->repeat_failures = config->repeat_failures;
    } else {
        created->mode = MOCK_SENSOR_LEVEL;
        created->amplitude_deg = MOCK_DEFAULT_AMPLITUDE;
        created->period_samples = MOCK_DEFAULT_PERIOD;
        created->step_us = MOCK_DEFAULT_STEP_US;
    }

    *sensor = created;
    return 0;
}

struct sensor_source mock_sensor_source(struct mock_sensor *sensor)
{
    struct sensor_source source;

    memset(&source, 0, sizeof(source));
    source.name = "mock";
    source.read = mock_read;
    source.close = mock_close_adapter;
    source.read_errors = mock_read_errors;
    source.context = sensor;
    return source;
}

void mock_close(struct mock_sensor *sensor)
{
    free(sensor);
}

/*
 * The interface's close takes a void context, so it needs a shim. Keeping the
 * typed mock_close() as the real entry point means a caller that skipped the
 * interface is not forced to cast.
 */
static void mock_close_adapter(void *context)
{
    mock_close(context);
}

unsigned long mock_sensor_samples(const struct mock_sensor *sensor)
{
    return sensor != NULL ? sensor->samples : 0UL;
}

unsigned long mock_sensor_errors(const struct mock_sensor *sensor)
{
    return sensor != NULL ? sensor->errors : 0UL;
}

/*
 * Should this read fail?
 *
 * fail_after_samples == 0 disables injection entirely, so the default
 * configuration is a sensor that always works and every fault in a test is
 * something the test explicitly asked for. A mock that fails by default would
 * make every other test's failure ambiguous.
 *
 * Once the window opens it stays open for fail_duration_samples reads. With
 * repeat_failures the schedule re-arms and fires again after the same count of
 * good samples, which is how a test gets "flaky sensor" behaviour - the case
 * that matters most, because recovery after a clean stop is the easy half and
 * recovery from a part that keeps dropping out is the real one.
 */
static bool should_fail(struct mock_sensor *sensor)
{
    uint64_t fire_at;

    if (sensor->fail_duration_samples == 0U)
        return false;

    if (sensor->failing_remaining > 0U) {
        sensor->failing_remaining--;
        return true;
    }

    if (sensor->fail_after_samples == 0U)
        return false;

    fire_at = sensor->fail_after_samples;
    if (sensor->repeat_failures) {
        /*
         * Period of the schedule is the good-run length plus the failure
         * window. index is the count of reads attempted so far, so the
         * schedule repeats cleanly instead of drifting.
         */
        uint64_t period = (uint64_t)sensor->fail_after_samples +
                          sensor->fail_duration_samples;
        if (period == 0U)
            return false;

        if ((sensor->index % period) == fire_at) {
            sensor->failing_remaining = sensor->fail_duration_samples - 1U;
            return true;
        }
        return false;
    }

    if (sensor->index == fire_at) {
        sensor->failing_remaining = sensor->fail_duration_samples - 1U;
        return true;
    }
    return false;
}

/*
 * Build the acceleration vector for a given pitch and roll.
 *
 * The inverse of what sensor_attitude does: instead of reading a direction out
 * of the vector, put a 1 g vector at that direction. Written from the same
 * equations rearranged, so a mock at pitch P reads back as pitch P.
 *
 *   accel_x = -sin(pitch)
 *   accel_y =  sin(roll) * cos(pitch)
 *   accel_z =  cos(roll) * cos(pitch)
 *
 * The first version multiplied the x term by cos(roll) as well:
 *
 *   accel_x = -sin(pitch) * cos(roll)   <-- wrong
 *
 * which is the textbook formula for a different rotation order (roll applied
 * about the world Z axis, then pitch about the body Y axis). It looks
 * plausible, it matches several references, and it is not this solver's
 * convention. With roll applied about the X axis first, tilting in roll does
 * not shorten the projection of gravity onto the body X axis at all - the X
 * axis is the roll axis, so it is a fixed point of that rotation. The extra
 * cos(roll) therefore shrank accel_x for no reason.
 *
 * The unit magnitude is what exposed it: with the spurious factor the vector
 * is short by cos(roll) (0.991 at the 26.7 degrees the wave test happens to
 * visit, 1.0 at roll = 90 degrees), and the test that asserts |a| == 1 g
 * caught it. What made it hard to see is that the mock then reported both a
 * wrong pitch AND a short vector, while the trig, the sqrt and the attitude
 * solver were all individually correct - the error had to be in the algebra
 * relating the two. Reproducing the chain by hand and comparing against libm
 * is what separated them: libm gave the same 0.994375 for the same vector, so
 * the vector was wrong, not the square root.
 *
 * The roll angle is read back from atan2(y, z), so only its sign and the sign
 * of cos(roll) matter; the cos(pitch) factor cancels in that ratio and the
 * round trip stays exact.
 */
static void fill_accel_from_attitude(float pitch_deg, float roll_deg,
                                     float accel_g[3])
{
    float p = pitch_deg * DEG_TO_RAD;
    float r = roll_deg * DEG_TO_RAD;
    float cp = sensor_cos(p);
    float sp = sensor_sin(p);
    float cr = sensor_cos(r);
    float sr = sensor_sin(r);

    accel_g[0] = -sp;
    accel_g[1] = sr * cp;
    accel_g[2] = cr * cp;
}

/*
 * Produce the sample at the current index and advance.
 *
 * Split out from read() so the fault schedule can refuse to call it without
 * the waveform state advancing: a failed read must not consume a waveform
 * position, otherwise an injected failure would show up as a phase jump in
 * the signal and a test could not tell the two apart.
 */
static void produce(struct mock_sensor *sensor, struct sensor_sample *out)
{
    float pitch = 0.0f;
    float roll = 0.0f;

    memset(out, 0, sizeof(*out));

    switch (sensor->mode) {
    case MOCK_SENSOR_WAVE: {
        /*
         * Sinusoidal tilt, phase-shifted between the two axes so a dead or
         * frozen channel cannot hide behind the other one's motion.
         */
        float phase = (float)sensor->index / (float)sensor->period_samples;
        float two_pi = 6.28318530718f;
        pitch = sensor->amplitude_deg * sensor_sin(phase * two_pi);
        roll = sensor->amplitude_deg * sensor_sin(phase * two_pi + 1.57079632679f);
        break;
    }
    case MOCK_SENSOR_RAMP: {
        /*
         * A slow ramp that wraps through the full range. Wrapping rather than
         * clamping keeps the value moving forever, so a long-running consumer
         * is never accidentally fed a constant.
         */
        float span = sensor->amplitude_deg * 2.0f;
        float pos = (float)(sensor->index % (uint64_t)(span != 0.0f ? (unsigned)span : 1U));
        pitch = pos - sensor->amplitude_deg;
        roll = pitch;
        break;
    }
    case MOCK_SENSOR_LEVEL:
    default:
        /* 1 g straight down Z. The reference case. */
        break;
    }

    fill_accel_from_attitude(pitch, roll, out->accel_g);

    /* A part at rest reports no angular rate. */
    out->gyro_dps[0] = 0.0f;
    out->gyro_dps[1] = 0.0f;
    out->gyro_dps[2] = 0.0f;

    /* Temperature is a constant, not a waveform: nothing tests temperature. */
    out->temperature_c = 25.0f;

    out->timestamp_us = sensor->index * sensor->step_us;

    /* Derive magnitude and attitude exactly as the real path would. */
    sensor_attitude_fill(out);
}

int mock_sensor_read(struct mock_sensor *sensor, struct sensor_sample *out)
{
    if (sensor == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (should_fail(sensor)) {
        sensor->errors++;
        sensor->index++;
        errno = EIO;
        return -1;
    }

    produce(sensor, out);
    sensor->index++;
    sensor->samples++;
    return 1;
}

/*
 * The interface adapter. Its only job is the void* cast, so the public
 * entry point stays typed and a direct caller needs no cast of its own.
 */
static int mock_read(void *context, struct sensor_sample *out)
{
    return mock_sensor_read(context, out);
}

static unsigned long mock_read_errors(void *context)
{
    struct mock_sensor *sensor = context;
    return sensor != NULL ? sensor->errors : 0UL;
}
