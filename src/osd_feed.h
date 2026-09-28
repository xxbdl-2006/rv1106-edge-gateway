#ifndef OSD_FEED_H
#define OSD_FEED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "osd_telemetry.h"
#include "sensor_source.h"

/*
 * Keeps an osd_telemetry_input current as the pipeline runs.
 *
 * WHY THIS EXISTS SEPARATELY FROM THE TELEMETRY LAYER. The telemetry layer
 * turns a snapshot of numbers into text; it has no opinion about where those
 * numbers come from or when they were sampled. This layer owns that: it polls
 * the sensor source on its own cadence, keeps the most recent usable sample
 * even across failed reads, and computes the frame rate over a window rather
 * than reporting an instantaneous value.
 *
 * THE SAMPLING CADENCE IS NOT THE FRAME CADENCE, and that is deliberate. The
 * MPU6050 can produce a sample every 10 ms and the display runs at 33 ms, so
 * polling it once per frame would work - but it would also couple the sensor
 * read to the encode loop, so a slow or hung I2C transaction would show up as a
 * dropped video frame. Instead the feed is polled with a timestamp and reads
 * the sensor only when `interval_us` has elapsed, so the cost per frame is one
 * comparison on the common path.
 *
 * THE LAST GOOD SAMPLE IS KEPT, NOT DISCARDED. When a read fails the display
 * keeps showing the previous reading and the input's `last_read_failed` flag
 * tells the overlay to mark it stale. Clearing the sample instead would make a
 * sensor glitch look like a board that had gone level, which is the exact
 * confusion the health line exists to prevent.
 *
 * THE RATE IS WINDOWED. An instantaneous 1/delta rate is unusable on an
 * overlay: it swings by tens of a percent frame to frame and is unreadable as
 * text. The window is a fixed count of frames, so the memory is a scalar and
 * the update is O(1).
 */

struct osd_feed;

struct osd_feed_config {
    /*
     * Minimum microseconds between sensor reads. 0 uses 20000 (50 Hz), which
     * is comfortably faster than the display needs and comfortably slower than
     * the part's maximum output rate.
     */
    uint64_t interval_us;
    /*
     * Frames in the rate window. 0 uses 30, i.e. one second at 30 fps: long
     * enough to be steady, short enough that a real rate change is visible
     * within a second.
     */
    unsigned rate_window;
};

/*
 * Create a feed over `source`.
 *
 * The source is borrowed, not owned: the caller opened it and the caller closes
 * it, which keeps the ownership in one place and lets a test hand in a fake
 * without the feed freeing something it did not allocate.
 *
 * `source` may be NULL, which makes every read report "no sample" - useful for
 * running the overlay on pipeline counters alone while a sensor is being
 * brought up, and for a test that wants the no-sample path without a stub.
 *
 * Returns 0 on success, -1 with errno set.
 */
int osd_feed_open(const struct osd_feed_config *config,
                  struct sensor_source *source,
                  struct osd_feed **feed);

void osd_feed_destroy(struct osd_feed *feed);

/*
 * Advance the feed and fill `input`.
 *
 * `now_us` is a monotonic timestamp; the frame's own capture timestamp goes in
 * via `frame_pts_us`, and `frame_count` is the caller's running count. Passing
 * the timestamps in rather than reading a clock here keeps this file
 * deterministic and therefore testable - the same reason the mock sensor's
 * waveforms are functions of the sample index rather than of the wall clock.
 *
 * Returns a pointer to the filled input, which the feed owns and which stays
 * valid until the next call. Never returns NULL.
 */
struct osd_telemetry_input *osd_feed_next(struct osd_feed *feed,
                                          uint64_t now_us,
                                          uint64_t frame_pts_us,
                                          uint64_t frame_count);

/* The most recent input, without advancing anything. */
const struct osd_telemetry_input *osd_feed_input(const struct osd_feed *feed);

struct osd_feed_stats {
    /* Calls that performed a sensor read, and reads that failed. */
    unsigned long sensor_polls;
    unsigned long sensor_errors;
    /* Samples accepted and samples refused by the source (returned 0). */
    unsigned long samples;
    unsigned long no_sample;
};

void osd_feed_stats(const struct osd_feed *feed, struct osd_feed_stats *stats);

#endif /* OSD_FEED_H */
