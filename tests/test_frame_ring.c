/*
 * Host side self test for the frame ring.
 *
 * Built and run natively (no SDK required):
 *
 *     make test-frame-ring
 *
 * It covers the invariants that the capture thread relies on: FIFO ordering,
 * zero copy borrowing, "a borrowed frame is never overwritten", the
 * overwrite-the-oldest overflow policy, and the stop/drain behaviour.
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "frame_ring.h"

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

static struct frame_ring *open_ring(size_t slots, size_t bytes)
{
    struct frame_ring_config config;
    struct frame_ring *ring = NULL;

    memset(&config, 0, sizeof(config));
    config.slot_count = slots;
    config.slot_bytes = bytes;

    if (frame_ring_open(&config, &ring) == -1) {
        printf("FAIL: frame_ring_open returned -1\n");
        g_checks++;
        g_failures++;
    }

    return ring;
}

static void fill(uint8_t *buffer, size_t length, uint8_t seed)
{
    for (size_t index = 0U; index < length; index++) {
        buffer[index] = (uint8_t)(seed + index);
    }
}

static void test_ordering_and_content(void)
{
    struct frame_ring *ring = open_ring(4U, 64U);
    uint8_t payload[16];
    const struct frame_ring_slot *slot = NULL;

    if (ring == NULL) {
        return;
    }

    for (uint8_t frame = 0U; frame < 3U; frame++) {
        fill(payload, sizeof(payload), (uint8_t)(frame * 16U));
        CHECK(frame_ring_push(ring, payload, sizeof(payload),
                              (uint64_t)frame * 1000ULL, frame) == 0,
              "push %u failed", frame);
    }

    for (uint64_t frame = 0U; frame < 3U; frame++) {
        CHECK(frame_ring_acquire(ring, &slot, 0) == FRAME_RING_OK,
              "acquire %" PRIu64 " failed", frame);
        CHECK(slot->sequence == frame,
              "expected sequence %" PRIu64 ", got %" PRIu64,
              frame, slot->sequence);
        CHECK(slot->length == sizeof(payload), "length %zu", slot->length);
        CHECK(slot->pts_us == frame * 1000ULL,
              "pts %" PRIu64, slot->pts_us);
        CHECK(slot->index == (unsigned)frame, "index %u", slot->index);
        CHECK(slot->data[0] == (uint8_t)(frame * 16U),
              "payload[0] %u", slot->data[0]);
        frame_ring_release(ring);
    }

    CHECK(frame_ring_acquire(ring, &slot, 0) == FRAME_RING_TIMEOUT,
          "%s", "empty ring must time out");

    frame_ring_destroy(ring);
}

static void test_borrowed_frame_is_stable(void)
{
    struct frame_ring *ring = open_ring(3U, 32U);
    uint8_t payload[8];
    const struct frame_ring_slot *slot = NULL;
    uint8_t snapshot[8];

    if (ring == NULL) {
        return;
    }

    fill(payload, sizeof(payload), 0xA0U);
    (void)frame_ring_push(ring, payload, sizeof(payload), 1U, 0U);

    CHECK(frame_ring_acquire(ring, &slot, 0) == FRAME_RING_OK,
          "%s", "acquire failed");

    memcpy(snapshot, slot->data, sizeof(snapshot));

    /*
     * Push enough frames to wrap the whole ring while the consumer is still
     * holding its slot. The borrowed bytes must survive untouched.
     */
    for (uint8_t frame = 0U; frame < 6U; frame++) {
        fill(payload, sizeof(payload), (uint8_t)(0x10U + frame));
        (void)frame_ring_push(ring, payload, sizeof(payload),
                              10U + frame, frame);
    }

    CHECK(memcmp(snapshot, slot->data, sizeof(snapshot)) == 0,
          "%s", "borrowed frame was overwritten");

    frame_ring_release(ring);
    frame_ring_destroy(ring);
}

static void test_overflow_overwrites_oldest(void)
{
    struct frame_ring *ring = open_ring(3U, 32U);
    uint8_t payload[8];
    const struct frame_ring_slot *slot = NULL;
    struct frame_ring_stats stats;

    if (ring == NULL) {
        return;
    }

    for (uint8_t frame = 0U; frame < 5U; frame++) {
        fill(payload, sizeof(payload), frame);
        (void)frame_ring_push(ring, payload, sizeof(payload), frame, frame);
    }

    frame_ring_stats(ring, &stats);
    CHECK(stats.pushed == 5U, "pushed %" PRIu64, stats.pushed);
    CHECK(stats.depth == 3U, "depth %zu", stats.depth);
    CHECK(stats.peak_depth == 3U, "peak %zu", stats.peak_depth);

    /*
     * Frames 0 and 1 were overwritten, so the consumer sees 2, 3, 4: the
     * freshest data and a bounded delay, which is the whole point.
     */
    for (uint64_t expected = 2U; expected < 5U; expected++) {
        CHECK(frame_ring_acquire(ring, &slot, 0) == FRAME_RING_OK,
              "acquire %" PRIu64, expected);
        CHECK(slot->sequence == expected,
              "expected sequence %" PRIu64 ", got %" PRIu64,
              expected, slot->sequence);
        CHECK(slot->data[0] == (uint8_t)expected,
              "payload[0] %u", slot->data[0]);
        frame_ring_release(ring);
    }

    frame_ring_destroy(ring);
}

static void test_oversize_is_rejected(void)
{
    struct frame_ring *ring = open_ring(2U, 16U);
    uint8_t payload[64];
    const struct frame_ring_slot *slot = NULL;
    struct frame_ring_stats stats;

    if (ring == NULL) {
        return;
    }

    fill(payload, sizeof(payload), 1U);
    CHECK(frame_ring_push(ring, payload, sizeof(payload), 1U, 0U) == 0,
          "%s", "oversize push must not report an error");

    frame_ring_stats(ring, &stats);
    CHECK(stats.dropped_oversize == 1U,
          "dropped_oversize %" PRIu64, stats.dropped_oversize);
    CHECK(stats.depth == 0U, "depth %zu", stats.depth);
    CHECK(frame_ring_acquire(ring, &slot, 0) == FRAME_RING_TIMEOUT,
          "%s", "ring must stay empty");

    frame_ring_destroy(ring);
}

static void test_stop_drains_then_stops(void)
{
    struct frame_ring *ring = open_ring(4U, 32U);
    uint8_t payload[8];
    const struct frame_ring_slot *slot = NULL;

    if (ring == NULL) {
        return;
    }

    for (uint8_t frame = 0U; frame < 2U; frame++) {
        fill(payload, sizeof(payload), frame);
        (void)frame_ring_push(ring, payload, sizeof(payload), frame, frame);
    }

    frame_ring_stop(ring);

    /* Pending frames stay readable after stop. */
    CHECK(frame_ring_acquire(ring, &slot, 0) == FRAME_RING_OK,
          "%s", "first frame after stop must still be readable");
    frame_ring_release(ring);
    CHECK(frame_ring_acquire(ring, &slot, 0) == FRAME_RING_OK,
          "%s", "second frame after stop must still be readable");
    frame_ring_release(ring);

    CHECK(frame_ring_acquire(ring, &slot, 0) == FRAME_RING_STOPPED,
          "%s", "drained ring must report stopped");

    /* A push after stop is still accepted; the consumer drains it. */
    (void)frame_ring_push(ring, payload, sizeof(payload), 9U, 9U);
    CHECK(frame_ring_acquire(ring, &slot, 0) == FRAME_RING_OK,
          "%s", "frame pushed after stop must be readable");

    frame_ring_release(ring);
    frame_ring_destroy(ring);
}

/* ---------------------------------------------------- concurrency */

struct producer_args {
    struct frame_ring *ring;
    unsigned int frames;
    unsigned int length;
    /* Delay between pushes, so the consumer really gets to interleave. */
    unsigned int delay_us;
};

static void *producer_main(void *argument)
{
    struct producer_args *args = argument;
    uint8_t payload[256];

    if (args->length > sizeof(payload)) {
        return NULL;
    }

    for (unsigned int frame = 0U; frame < args->frames; frame++) {
        memset(payload, (int)(frame & 0xFFU), args->length);
        (void)frame_ring_push(args->ring, payload, args->length,
                              (uint64_t)frame, frame);

        if (args->delay_us != 0U) {
            struct timespec pause;

            pause.tv_sec = 0;
            pause.tv_nsec = (long)args->delay_us * 1000L;
            (void)nanosleep(&pause, NULL);
        }
    }

    return NULL;
}

/*
 * The producer is faster than the consumer and the ring is deliberately tiny,
 * so eviction happens constantly. What must hold is: sequences arrive strictly
 * increasing and every delivered frame matches the byte pattern of its own
 * sequence number.
 */
static void test_producer_consumer(void)
{
    struct frame_ring *ring = open_ring(3U, 256U);
    struct producer_args args;
    const struct frame_ring_slot *slot = NULL;
    pthread_t producer;
    struct frame_ring_stats stats;
    uint64_t last_sequence = 0U;
    unsigned long received = 0UL;
    bool first = true;
    int result;

    if (ring == NULL) {
        return;
    }

    args.ring = ring;
    args.frames = 400U;
    args.length = 200U;
    /*
     * The producer runs unpaced while the consumer pauses 2 ms per frame. The
     * 3 slot ring therefore overflows heavily, which is exactly the condition
     * this test needs. Relying on a paced producer instead would make the
     * outcome depend on the host timer granularity.
     */
    args.delay_us = 0U;

    result = pthread_create(&producer, NULL, producer_main, &args);
    CHECK(result == 0, "pthread_create failed: %d", result);
    if (result != 0) {
        frame_ring_destroy(ring);
        return;
    }

    for (;;) {
        uint8_t expected;

        result = frame_ring_acquire(ring, &slot, 50);
        if (result == FRAME_RING_TIMEOUT) {
            /* Give the producer a chance to finish before giving up. */
            if (last_sequence + 1U >= args.frames) {
                break;
            }
            continue;
        }
        if (result == FRAME_RING_STOPPED) {
            break;
        }

        CHECK(slot->sequence >= last_sequence,
              "sequence went backwards: %" PRIu64 " after %" PRIu64,
              slot->sequence, last_sequence);
        if (!first) {
            CHECK(slot->sequence > last_sequence,
                  "duplicate sequence %" PRIu64, slot->sequence);
        }
        first = false;

        expected = (uint8_t)(slot->sequence & 0xFFU);
        CHECK(slot->data[0] == expected,
              "frame %" PRIu64 " has payload %u",
              slot->sequence, slot->data[0]);
        CHECK(slot->data[199] == expected,
              "frame %" PRIu64 " tail payload %u",
              slot->sequence, slot->data[199]);

        last_sequence = slot->sequence;
        received++;
        frame_ring_release(ring);

        /* Far slower than the producer, so the ring has to overflow. */
        {
            struct timespec pause;

            pause.tv_sec = 0;
            pause.tv_nsec = 2000000L;
            (void)nanosleep(&pause, NULL);
        }
    }

    (void)pthread_join(producer, NULL);

    /* Drain whatever the producer managed to leave behind. */
    while (frame_ring_acquire(ring, &slot, 0) == FRAME_RING_OK) {
        CHECK(slot->sequence >= last_sequence,
              "tail sequence went backwards: %" PRIu64, slot->sequence);
        last_sequence = slot->sequence;
        received++;
        frame_ring_release(ring);
    }

    frame_ring_stats(ring, &stats);
    CHECK(stats.pushed == args.frames, "pushed %" PRIu64, stats.pushed);
    CHECK(received > 0UL, "consumer received %lu frames", received);
    CHECK(stats.dropped_oldest > 0U,
          "%s", "test is meaningless unless the ring actually overflowed");
    CHECK(stats.popped + stats.dropped_oldest + stats.dropped_busy ==
              stats.pushed,
          "accounting mismatch: popped=%" PRIu64 " dropped_oldest=%" PRIu64
          " dropped_busy=%" PRIu64 " pushed=%" PRIu64,
          stats.popped, stats.dropped_oldest, stats.dropped_busy,
          stats.pushed);

    printf("  concurrency: received %lu of %" PRIu64
           " pushed, dropped_oldest=%" PRIu64 " dropped_busy=%" PRIu64 "\n",
           received, stats.pushed, stats.dropped_oldest, stats.dropped_busy);

    frame_ring_destroy(ring);
}

int main(void)
{
    printf("frame_ring self test\n");

    test_ordering_and_content();
    test_borrowed_frame_is_stable();
    test_overflow_overwrites_oldest();
    test_oversize_is_rejected();
    test_stop_drains_then_stops();
    test_producer_consumer();

    printf("checks=%d failures=%d\n", g_checks, g_failures);

    if (g_failures != 0) {
        printf("RESULT: FAIL\n");
        return EXIT_FAILURE;
    }

    printf("RESULT: PASS\n");
    return EXIT_SUCCESS;
}
