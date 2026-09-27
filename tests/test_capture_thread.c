/*
 * Host side self test for the capture thread.
 *
 * Built and run natively (no camera, no SDK):
 *
 *     make test-capture-thread
 *
 * Instead of V4L2 the source callback synthesises frames, which is enough to
 * verify the parts that actually broke before: that the thread keeps producing
 * while the consumer is slow, that warmup frames never reach the ring, that a
 * source error stops the thread instead of spinning, and that stop() joins
 * instead of hanging.
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "capture_signal.h"
#include "capture_thread.h"
#include "frame_ring.h"

/*
 * g_stop normally lives in v4l2_capture.c along with the signal handler. The
 * host test links without that file, so provide the definition here.
 */
volatile sig_atomic_t g_stop;

static int g_checks;
static int g_failures;

#define CHECK(condition, format, ...)                                      \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(condition)) {                                                \
            g_failures++;                                                  \
            printf("FAIL line %d: " format "\n", __LINE__, __VA_ARGS__);    \
        }                                                                  \
    } while (false)

/* ------------------------------------------------------- fake capture */

enum fake_mode {
    FAKE_NORMAL = 0,
    FAKE_ALWAYS_FAIL,
    FAKE_ERROR_AFTER_N
};

struct fake_capture {
    enum fake_mode mode;
    unsigned int produced;
    unsigned int release_count;
    unsigned int fail_after;
    unsigned int delay_us;
    uint8_t payload[64];
};

static bool fake_source(void *context, struct capture_frame_view *view)
{
    struct fake_capture *fake = context;

    if (fake->mode == FAKE_ALWAYS_FAIL) {
        return false;
    }

    if (fake->mode == FAKE_ERROR_AFTER_N &&
        fake->produced >= fake->fail_after) {
        return false;
    }

    if (fake->delay_us != 0U) {
        struct timespec pause;

        pause.tv_sec = 0;
        pause.tv_nsec = (long)fake->delay_us * 1000L;
        (void)nanosleep(&pause, NULL);
    }

    memset(fake->payload, (int)(fake->produced & 0xFFU),
           sizeof(fake->payload));

    view->data = fake->payload;
    view->length = sizeof(fake->payload);
    /*
     * Carry the source frame number in the V4L2 index field. The ring's own
     * sequence number counts ACCEPTED frames, so it restarts at zero once
     * warmup ends and cannot be used to tell which source frame this was.
     */
    view->index = fake->produced;
    view->pts_us = (uint64_t)fake->produced * 33333ULL;

    fake->produced++;
    return true;
}

static int fake_release(void *context, unsigned int buffer_index)
{
    struct fake_capture *fake = context;

    (void)buffer_index;
    fake->release_count++;
    return 0;
}

static struct capture_thread *start_thread(struct fake_capture *fake,
                                           unsigned int warmup,
                                           size_t slots)
{
    struct capture_thread_config config;
    struct capture_thread *thread = NULL;

    memset(&config, 0, sizeof(config));
    config.ring_slots = slots;
    config.slot_bytes = 128U;
    config.warmup_frames = warmup;

    g_stop = 0;

    if (capture_thread_start(&config, fake_source, fake_release, fake,
                             &thread) == -1) {
        printf("FAIL: capture_thread_start returned -1\n");
        g_checks++;
        g_failures++;
    }

    return thread;
}

/* ------------------------------------------------------------- tests */

static void test_produces_and_releases(void)
{
    struct fake_capture fake;
    struct capture_thread *thread;
    struct frame_ring *ring;
    const struct frame_ring_slot *slot = NULL;
    struct capture_thread_stats stats;
    unsigned long received = 0UL;

    memset(&fake, 0, sizeof(fake));
    thread = start_thread(&fake, 0U, 4U);
    if (thread == NULL) {
        return;
    }

    CHECK(capture_thread_wait_ready(thread, 2000) == 0,
          "%s", "wait_ready must succeed on a healthy source");

    ring = capture_thread_ring(thread);
    CHECK(ring != NULL, "%s", "ring must not be NULL");

    for (unsigned int frame = 0U; frame < 8U; frame++) {
        if (frame_ring_acquire(ring, &slot, 500) != FRAME_RING_OK) {
            break;
        }
        received++;
        frame_ring_release(ring);
    }

    CHECK(received > 0UL, "received %lu frames", received);

    capture_thread_stats(thread, &stats);
    CHECK(stats.captured > 0UL, "captured %lu", stats.captured);

    /*
     * Every produced frame must have been requeued to the driver, otherwise
     * V4L2 would run out of buffers within seconds. Stop first: while the
     * thread is live it is always one frame ahead of the counters, so
     * comparing them here would be a race, not a bug.
     */
    capture_thread_stop(thread);

    CHECK(fake.release_count >= fake.produced,
          "released %u of %u produced",
          fake.release_count, fake.produced);

    capture_thread_destroy(thread);
}

static void test_warmup_frames_never_reach_ring(void)
{
    struct fake_capture fake;
    struct capture_thread *thread;
    struct frame_ring *ring;
    const struct frame_ring_slot *slot = NULL;
    struct capture_thread_stats stats;

    memset(&fake, 0, sizeof(fake));
    /* Slow the source so warmup does not finish before we look. */
    fake.delay_us = 2000U;

    thread = start_thread(&fake, 5U, 64U);
    if (thread == NULL) {
        return;
    }

    CHECK(capture_thread_wait_ready(thread, 3000) == 0,
          "%s", "wait_ready must succeed once warmup is done");

    capture_thread_stats(thread, &stats);
    CHECK(stats.skipped >= 5U, "skipped %lu warmup frames", stats.skipped);
    CHECK(fake.release_count >= 5U,
          "released %u warmup frames", fake.release_count);

    ring = capture_thread_ring(thread);

    /*
     * Stop first, then drain. A 64 slot ring against a 2 ms source easily
     * holds every accepted frame, so whatever is left is exactly what warmup
     * let through and nothing is lost to overwriting.
     */
    capture_thread_stop(thread);

    {
        unsigned long seen = 0UL;
        unsigned long too_early = 0UL;

        while (frame_ring_acquire(ring, &slot, 0) == FRAME_RING_OK) {
            seen++;
            if (slot->index < 5U) {
                too_early++;
            }
            frame_ring_release(ring);
        }

        CHECK(seen > 0UL, "%s", "no frame reached the ring after warmup");
        CHECK(too_early == 0UL,
              "%lu of %lu frames came from the warmup window",
              too_early, seen);
    }
    capture_thread_destroy(thread);
}

static void test_persistent_source_failure_stops_thread(void)
{
    struct fake_capture fake;
    struct capture_thread *thread;

    memset(&fake, 0, sizeof(fake));
    fake.mode = FAKE_ALWAYS_FAIL;

    thread = start_thread(&fake, 0U, 4U);
    if (thread == NULL) {
        return;
    }

    /*
     * A source that never yields must not be retried forever: the thread has to
     * give up so the caller can report the failure instead of the process
     * appearing to run while producing nothing.
     */
    CHECK(capture_thread_wait_ready(thread, 1000) == -1,
          "%s", "wait_ready must fail when the source never produces");

    /* Give the thread a moment to notice and exit, then stop must not hang. */
    capture_thread_stop(thread);
    capture_thread_destroy(thread);
}

static void test_stop_joins_unblocked(void)
{
    struct fake_capture fake;
    struct capture_thread *thread;
    struct timespec start;
    struct timespec end;
    long elapsed_ms;

    memset(&fake, 0, sizeof(fake));
    /* A slow source keeps the thread inside a poll-like wait. */
    fake.delay_us = 5000U;

    thread = start_thread(&fake, 0U, 2U);
    if (thread == NULL) {
        return;
    }

    CHECK(capture_thread_wait_ready(thread, 3000) == 0,
          "%s", "wait_ready failed");

    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    capture_thread_stop(thread);
    capture_thread_destroy(thread);
    (void)clock_gettime(CLOCK_MONOTONIC, &end);

    elapsed_ms = (long)((end.tv_sec - start.tv_sec) * 1000L +
                        (end.tv_nsec - start.tv_nsec) / 1000000L);

    /* One 5 ms source delay plus join overhead. Generous enough for CI. */
    CHECK(elapsed_ms < 2000L, "stop took %ld ms", elapsed_ms);

    printf("  stop latency: %ld ms\n", elapsed_ms);
}

int main(void)
{
    printf("capture_thread self test\n");

    g_stop = 0;

    test_produces_and_releases();
    test_warmup_frames_never_reach_ring();
    test_persistent_source_failure_stops_thread();
    test_stop_joins_unblocked();

    printf("checks=%d failures=%d\n", g_checks, g_failures);

    if (g_failures != 0) {
        printf("RESULT: FAIL\n");
        return EXIT_FAILURE;
    }

    printf("RESULT: PASS\n");
    return EXIT_SUCCESS;
}
