#include "osd_feed.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OSD_FEED_DEFAULT_INTERVAL_US 20000ULL   /* 50 Hz */
#define OSD_FEED_DEFAULT_RATE_WINDOW 30U        /* one second at 30 fps */

/*
 * Largest rate window we will honour.
 *
 * The window is stored as a fixed ring so nothing allocates per frame, which
 * means the array size is a compile-time constant. A caller asking for more
 * than this is refused rather than silently clamped: a window that quietly
 * became 120 frames when 300 was requested would make the rate sluggish for
 * reasons nobody could see from the API.
 */
#define OSD_FEED_MAX_RATE_WINDOW 128U

struct osd_feed {
    struct sensor_source *source;
    uint64_t interval_us;

    /* Last time the sensor was polled, and the last good sample. */
    uint64_t last_poll_us;
    struct sensor_sample sample;
    bool have_sample;
    bool last_read_failed;
    unsigned long read_errors;

    /*
     * Frame timestamps for the rate window, in a fixed ring. The rate is
     * measured over the span between the oldest and newest entry rather than
     * over an assumed frame count, so a window that straddles a stall reports
     * the rate it actually achieved instead of 30.
     */
    uint64_t window[OSD_FEED_MAX_RATE_WINDOW];
    unsigned window_size;
    unsigned window_count;
    unsigned window_head;

    struct osd_telemetry_input input;
    struct osd_feed_stats stats;
};

int osd_feed_open(const struct osd_feed_config *config,
                  struct sensor_source *source,
                  struct osd_feed **feed)
{
    struct osd_feed *created;

    if (feed == NULL) {
        errno = EINVAL;
        return -1;
    }
    *feed = NULL;

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        perror("osd_feed_open: calloc");
        return -1;
    }

    created->source = source;
    created->interval_us = (config != NULL && config->interval_us != 0U)
                               ? config->interval_us
                               : OSD_FEED_DEFAULT_INTERVAL_US;

    created->window_size = (config != NULL && config->rate_window != 0U)
                               ? config->rate_window
                               : OSD_FEED_DEFAULT_RATE_WINDOW;

    if (created->window_size > OSD_FEED_MAX_RATE_WINDOW) {
        fprintf(stderr, "osd_feed_open: rate window %u exceeds the maximum %u\n",
                created->window_size, (unsigned)OSD_FEED_MAX_RATE_WINDOW);
        free(created);
        errno = EINVAL;
        return -1;
    }

    /*
     * The source name is published so the overlay can say which sensor the
     * numbers came from. A mock mistaken for hardware is a mistake this project
     * has already made once, and printing the name costs a pointer.
     */
    created->input.sensor.source_name =
        (source != NULL && source->name != NULL) ? source->name : "none";

    *feed = created;
    return 0;
}

void osd_feed_destroy(struct osd_feed *feed)
{
    if (feed == NULL) {
        return;
    }

    /*
     * The source is borrowed and is NOT closed here. Closing it would mean this
     * layer decided the lifetime of something it did not open, and a test that
     * stacked a feed on a stack-allocated fake would have it freed underneath.
     */
    free(feed);
}

/*
 * Poll the sensor if enough time has passed.
 *
 * The interval test is `>=` rather than `>` so that an interval of 0 means
 * "every call", which is what a test wants when it is driving the feed by hand
 * and has no real time to advance.
 */
static void poll_sensor(struct osd_feed *feed, uint64_t now_us)
{
    int result;

    if (feed->source == NULL || feed->source->read == NULL) {
        return;
    }

    if (feed->last_poll_us != 0U && now_us - feed->last_poll_us <
                                        feed->interval_us) {
        return;
    }
    feed->last_poll_us = now_us;

    feed->stats.sensor_polls++;

    result = feed->source->read(feed->source->context, &feed->sample);

    if (result == 1) {
        feed->have_sample = true;
        feed->last_read_failed = false;
        feed->stats.samples++;
        return;
    }

    if (result == 0) {
        /*
         * Merely idle, which the source interface defines as distinct from an
         * error. The previous sample and the previous failure flag are both
         * left alone: nothing happened, and inventing a state change for it
         * would make an idle sensor look like a flapping one.
         */
        feed->stats.no_sample++;
        return;
    }

    /*
     * A real error. The last good sample is deliberately kept so the display
     * freezes on a plausible reading with a stale marker rather than dropping
     * to "--", which would be indistinguishable from a board that never had a
     * sensor at all.
     */
    feed->last_read_failed = true;
    feed->stats.sensor_errors++;

    if (feed->source->read_errors != NULL) {
        feed->read_errors = feed->source->read_errors(feed->source->context);
    } else {
        feed->read_errors++;
    }
}

/* Push one frame timestamp into the ring and return the measured rate. */
static float update_rate(struct osd_feed *feed, uint64_t now_us)
{
    uint64_t oldest;
    uint64_t span_us;
    unsigned count;

    feed->window[feed->window_head] = now_us;
    feed->window_head = (feed->window_head + 1U) % feed->window_size;
    if (feed->window_count < feed->window_size) {
        feed->window_count++;
    }

    count = feed->window_count;
    if (count < 2U) {
        /*
         * A rate needs two timestamps. Reporting 0 for the first frame would
         * print "0.0 fps" on the very first frame of every run, which reads
         * like a fault; the caller's configured rate is the honest answer
         * until there is a measured one.
         */
        return 0.0f;
    }

    /*
     * The oldest entry is the one just before the head once the ring is full,
     * and index 0 before that. Computing it from head and count rather than
     * storing a separate tail keeps the two from drifting apart.
     */
    oldest = feed->window[(feed->window_head + feed->window_size - count) %
                          feed->window_size];

    span_us = now_us - oldest;
    if (span_us == 0U) {
        return 0.0f;
    }

    /*
     * count-1 intervals span the window: n timestamps bound n-1 frame periods.
     * Using count here would overstate the rate by one part in count, which at
     * 30 fps is a visible 30.0 reported as 31.0.
     */
    return (float)(count - 1U) * 1000000.0f / (float)span_us;
}

struct osd_telemetry_input *osd_feed_next(struct osd_feed *feed,
                                          uint64_t now_us,
                                          uint64_t frame_pts_us,
                                          uint64_t frame_count)
{
    if (feed == NULL) {
        return NULL;
    }

    poll_sensor(feed, now_us);

    feed->input.clock.now_us = now_us;
    feed->input.clock.frame_pts_us = frame_pts_us;
    feed->input.clock.frame_count = frame_count;
    feed->input.clock.frame_rate = update_rate(feed, now_us);

    feed->input.sensor.have_sample = feed->have_sample;
    feed->input.sensor.sample = feed->sample;
    feed->input.sensor.last_read_failed = feed->last_read_failed;
    feed->input.sensor.read_errors = feed->read_errors;
    /* source_name was set at open and never changes; copying it per frame
     * would be a string pointer assignment for no reason. */

    return &feed->input;
}

const struct osd_telemetry_input *osd_feed_input(const struct osd_feed *feed)
{
    return feed != NULL ? &feed->input : NULL;
}

void osd_feed_stats(const struct osd_feed *feed, struct osd_feed_stats *stats)
{
    if (feed == NULL || stats == NULL) {
        return;
    }

    *stats = feed->stats;
}
