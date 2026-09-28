#ifndef MOCK_SENSOR_H
#define MOCK_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "sensor_source.h"

/*
 * A sensor source that produces synthetic samples, so the entire data plane
 * above the sensor can be built, tested and demoed with no hardware attached.
 *
 * The roadmap asks for this before the real part for two reasons, and the
 * second is the one that matters:
 *
 *   1. It makes the OSD and the alarm path developable on a desk.
 *   2. It is the only way to test failure handling. Real hardware fails
 *      rarely and at inconvenient moments; a mock that can be told to stall,
 *      to return garbage, or to die mid-stream turns "we hope the timeout
 *      works" into a test that runs in milliseconds. Hardware can demonstrate
 *      the happy path, but it cannot conveniently demonstrate a sensor that
 *      goes quiet for two seconds and then recovers.
 *
 * The waveforms are deterministic functions of the sample index, not of the
 * wall clock. That is deliberate: a test can assert exact values, and two runs
 * of the same test produce the same bytes. A mock driven by clock_gettime
 * would be untestable in the only way that matters, because its output would
 * depend on how fast the machine happened to be.
 */

enum mock_sensor_mode {
    /*
     * Flat and level: 1 g straight down the Z axis, no rotation. This is the
     * "everything is fine" reference, and the mode that checks the attitude
     * maths produces 0/0 rather than a small non-zero from accumulated error.
     */
    MOCK_SENSOR_LEVEL = 0,

    /*
     * Slow sinusoidal tilt in pitch and roll, out of phase so the two traces
     * do not overlap and a frozen channel is obvious. Amplitudes and period
     * are configurable. Useful for watching the OSD move and for spotting a
     * swapped axis, which a level signal can never reveal.
     */
    MOCK_SENSOR_WAVE,

    /*
     * A monotonic ramp on all axes, wrapping. Stays inside real sensor limits
     * and never repeats within a short run, so a consumer that drops or
     * duplicates samples is caught - with a periodic waveform a dropped sample
     * looks like a phase shift and can go unnoticed.
     */
    MOCK_SENSOR_RAMP,
};

struct mock_sensor_config {
    enum mock_sensor_mode mode;
    /* Wave amplitude in degrees, for MOCK_SENSOR_WAVE. 0 uses 30. */
    float amplitude_deg;
    /* Samples per full cycle, for MOCK_SENSOR_WAVE. 0 uses 300 (3 s at 100 Hz). */
    unsigned period_samples;
    /* Timestamp step in microseconds. 0 uses 10000 (100 Hz). */
    uint64_t step_us;
    /* Injected by the fault schedule below. */
    unsigned fail_after_samples;
    unsigned fail_duration_samples;
    bool repeat_failures;
};

struct mock_sensor;

/*
 * Create a mock source. mode 0 (LEVEL) and all zero fields give a sane
 * default: level, 100 Hz, no injected faults.
 *
 * Returns 0 on success. On failure returns -1 with errno set.
 */
int mock_sensor_open(const struct mock_sensor_config *config,
                     struct mock_sensor **sensor);

/* The source interface, for handing to a consumer. */
struct sensor_source mock_sensor_source(struct mock_sensor *sensor);

/*
 * Direct sample access, bypassing the struct sensor_source indirection.
 *
 * The source struct exists so consumers are polymorphic; tests and tools that
 * already know they hold a mock should not have to go through it to check
 * behaviour, and the extra hop obscures what a failing assertion is testing.
 * Same contract as the interface method: 1 produced, 0 not ready, -1 error.
 */
int mock_sensor_read(struct mock_sensor *sensor, struct sensor_sample *out);

/* Samples produced so far, counting only successful reads. */
unsigned long mock_sensor_samples(const struct mock_sensor *sensor);

/* Errors injected so far, i.e. reads that returned -1. */
unsigned long mock_sensor_errors(const struct mock_sensor *sensor);

/*
 * Release the source. Declared here and not only reachable through the
 * interface's close(), because a caller that opened the mock directly and
 * never wrapped it in a struct sensor_source still has to be able to free it.
 */
void mock_close(struct mock_sensor *sensor);

#endif /* MOCK_SENSOR_H */
