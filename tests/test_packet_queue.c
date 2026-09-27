/*
 * Host side self test for the packet queue and the H.264 helpers.
 *
 * Built and run natively (no SDK required):
 *
 *     make test-packet-queue
 *
 * It covers the invariants that matter for the RTSP stage: ordering,
 * zero copy borrowing, oversize frames, "drop the oldest GOP instead of a
 * random middle frame", and that nothing gets corrupted while a frame is
 * borrowed by the consumer.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h264_util.h"
#include "packet_queue.h"

static int g_checks;
static int g_failures;

#define CHECK(condition, format, ...)                                      \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(condition)) {                                                \
            g_failures++;                                                  \
            printf("FAIL line %d: " format "\n", __LINE__, __VA_ARGS__);   \
        }                                                                  \
    } while (false)

static const uint8_t g_idr[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x80, 0x1E,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x33, 0xFF
};

static const uint8_t g_p[] = {
    0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x01, 0x11, 0x22
};

static void push_bytes(struct packet_queue *queue,
                       uint64_t pts_us,
                       const void *data,
                       size_t length)
{
    struct packet_segment segment;

    segment.data = (const uint8_t *)data;
    segment.length = length;

    (void)packet_queue_push(queue, pts_us, &segment, 1U);
}

static void test_h264_flags(void)
{
    CHECK((h264_frame_flags(g_idr, sizeof(g_idr)) & H264_FLAG_KEY) != 0U,
          "%s", "IDR frame must be flagged as a key frame");
    CHECK((h264_frame_flags(g_p, sizeof(g_p)) & H264_FLAG_KEY) == 0U,
          "%s", "P frame must not be flagged as a key frame");
    CHECK(h264_frame_flags(g_p, 3U) == 0U,
          "short input must not be flagged, got %u",
          h264_frame_flags(g_p, 3U));
    CHECK(h264_frame_flags(NULL, 0U) == 0U, "%s", "NULL input must be handled");
}

static void test_ordering_and_content(void)
{
    struct packet_queue_config config;
    struct packet_queue *queue = NULL;
    const struct packet_queue_slot *slot = NULL;

    memset(&config, 0, sizeof(config));
    config.slot_count = 4U;
    config.slot_bytes = 1024U;
    CHECK(packet_queue_open(&config, &queue) == 0, "open failed: %d", 1);

    push_bytes(queue, 1000U, g_idr, sizeof(g_idr));
    push_bytes(queue, 2000U, g_p, sizeof(g_p));

    CHECK(packet_queue_acquire(queue, &slot, 100) == PACKET_QUEUE_OK,
          "expected a frame: %d", 1);
    CHECK(slot->length == sizeof(g_idr), "wrong length %zu", slot->length);
    CHECK(slot->pts_us == 1000U, "wrong pts %" PRIu64, slot->pts_us);
    CHECK((slot->flags & H264_FLAG_KEY) != 0U, "expected key frame: %d", 1);
    CHECK(memcmp(slot->data, g_idr, sizeof(g_idr)) == 0,
          "payload mismatch: %d", 1);
    packet_queue_release(queue);

    CHECK(packet_queue_acquire(queue, &slot, 100) == PACKET_QUEUE_OK,
          "expected a second frame: %d", 1);
    CHECK(slot->length == sizeof(g_p), "wrong length %zu", slot->length);
    CHECK(slot->pts_us == 2000U, "wrong pts %" PRIu64, slot->pts_us);
    CHECK((slot->flags & H264_FLAG_KEY) == 0U,
          "second frame must not be a key: %d", 1);
    packet_queue_release(queue);

    CHECK(packet_queue_acquire(queue, &slot, 0) == PACKET_QUEUE_TIMEOUT,
          "empty queue must time out: %d", 1);

    packet_queue_destroy(queue);
}

static void test_multiple_segments(void)
{
    struct packet_queue_config config;
    struct packet_queue *queue = NULL;
    const struct packet_queue_slot *slot = NULL;
    struct packet_segment segments[3];

    memset(&config, 0, sizeof(config));
    config.slot_count = 2U;
    config.slot_bytes = 1024U;
    CHECK(packet_queue_open(&config, &queue) == 0, "open failed: %d", 1);

    segments[0].data = g_idr;
    segments[0].length = 4U;
    segments[1].data = g_idr + 4U;
    segments[1].length = 12U;
    segments[2].data = g_idr + 16U;
    segments[2].length = sizeof(g_idr) - 16U;

    (void)packet_queue_push(queue, 7U, segments, 3U);

    CHECK(packet_queue_acquire(queue, &slot, 100) == PACKET_QUEUE_OK,
          "expected a frame: %d", 1);
    CHECK(slot->length == sizeof(g_idr),
          "segments must be concatenated, got %zu", slot->length);
    CHECK(memcmp(slot->data, g_idr, sizeof(g_idr)) == 0,
          "gathered payload mismatch: %d", 1);
    packet_queue_release(queue);

    packet_queue_destroy(queue);
}

static void test_oversize_frame(void)
{
    struct packet_queue_config config;
    struct packet_queue *queue = NULL;
    struct packet_queue_stats stats;

    memset(&config, 0, sizeof(config));
    config.slot_count = 2U;
    config.slot_bytes = 16U;
    CHECK(packet_queue_open(&config, &queue) == 0, "open failed: %d", 1);

    push_bytes(queue, 1U, g_idr, sizeof(g_idr));

    packet_queue_stats(queue, &stats);
    CHECK(stats.dropped_oversize == 1U,
          "oversize frame must be counted, got %" PRIu64,
          stats.dropped_oversize);
    CHECK(stats.depth == 0U, "queue must stay empty, depth %zu", stats.depth);

    packet_queue_destroy(queue);
}

static void test_eviction_drops_whole_gop(void)
{
    struct packet_queue_config config;
    struct packet_queue *queue = NULL;
    const struct packet_queue_slot *slot = NULL;
    struct packet_queue_stats stats;

    memset(&config, 0, sizeof(config));
    config.slot_count = 4U;
    config.slot_bytes = 1024U;
    CHECK(packet_queue_open(&config, &queue) == 0, "open failed: %d", 1);

    push_bytes(queue, 1U, g_idr, sizeof(g_idr));
    push_bytes(queue, 2U, g_p, sizeof(g_p));
    push_bytes(queue, 3U, g_p, sizeof(g_p));
    push_bytes(queue, 4U, g_p, sizeof(g_p));

    /* Overflowing must discard stale data, never the incoming frame. */
    push_bytes(queue, 5U, g_idr, sizeof(g_idr));

    packet_queue_stats(queue, &stats);
    CHECK(stats.dropped_stale == 4U,
          "expected 4 stale frames dropped, got %" PRIu64,
          stats.dropped_stale);
    CHECK(stats.depth == 1U, "expected depth 1, got %zu", stats.depth);

    CHECK(packet_queue_acquire(queue, &slot, 100) == PACKET_QUEUE_OK,
          "expected the surviving frame: %d", 1);
    CHECK(slot->pts_us == 5U, "newest frame must survive, got pts %" PRIu64,
          slot->pts_us);
    CHECK((slot->flags & H264_FLAG_KEY) != 0U,
          "queue must restart at an IDR: %d", 1);
    packet_queue_release(queue);

    packet_queue_destroy(queue);
}

static void test_borrowed_frame_is_never_overwritten(void)
{
    struct packet_queue_config config;
    struct packet_queue *queue = NULL;
    const struct packet_queue_slot *slot = NULL;
    struct packet_queue_stats stats;

    memset(&config, 0, sizeof(config));
    config.slot_count = 1U;
    config.slot_bytes = 1024U;
    CHECK(packet_queue_open(&config, &queue) == 0, "open failed: %d", 1);

    push_bytes(queue, 1U, g_idr, sizeof(g_idr));
    CHECK(packet_queue_acquire(queue, &slot, 100) == PACKET_QUEUE_OK,
          "expected a frame: %d", 1);
    CHECK(slot->pts_us == 1U, "wrong pts %" PRIu64, slot->pts_us);

    /* Fill up while that frame is still borrowed. */
    push_bytes(queue, 2U, g_p, sizeof(g_p));

    CHECK(memcmp(slot->data, g_idr, sizeof(g_idr)) == 0,
          "borrowed payload was overwritten: %d", 1);
    CHECK(slot->length == sizeof(g_idr),
          "borrowed length changed to %zu", slot->length);

    packet_queue_stats(queue, &stats);
    CHECK(stats.dropped_full == 1U,
          "expected the incoming frame to be dropped, got %" PRIu64,
          stats.dropped_full);

    packet_queue_release(queue);

    packet_queue_destroy(queue);
}

struct thread_context {
    struct packet_queue *queue;
    uint64_t pushed;
    uint64_t popped;
    uint64_t mismatches;
};

static void *producer_thread(void *argument)
{
    struct thread_context *shared = argument;
    uint64_t index;

    for (index = 0U; index < 4000U; index++) {
        if ((index % 30U) == 0U) {
            push_bytes(shared->queue, index, g_idr, sizeof(g_idr));
        } else {
            push_bytes(shared->queue, index, g_p, sizeof(g_p));
        }
        shared->pushed++;
    }

    return NULL;
}

static void *consumer_thread(void *argument)
{
    struct thread_context *shared = argument;

    for (;;) {
        const struct packet_queue_slot *slot = NULL;
        int result;

        result = packet_queue_acquire(shared->queue, &slot, 50);
        if (result == PACKET_QUEUE_STOPPED) {
            break;
        }
        if (result != PACKET_QUEUE_OK) {
            continue;
        }

        if (slot->length != sizeof(g_idr) && slot->length != sizeof(g_p)) {
            shared->mismatches++;
        } else if (memcmp(slot->data,
                          slot->length == sizeof(g_idr) ? g_idr : g_p,
                          slot->length) != 0) {
            shared->mismatches++;
        }

        shared->popped++;
        packet_queue_release(shared->queue);
    }

    return NULL;
}

static void test_producer_consumer(void)
{
    struct packet_queue_config config;
    struct packet_queue *queue = NULL;
    struct thread_context shared;
    struct packet_queue_stats stats;
    pthread_t producer;
    pthread_t consumer;

    memset(&config, 0, sizeof(config));
    config.slot_count = 16U;
    config.slot_bytes = 1024U;
    CHECK(packet_queue_open(&config, &queue) == 0, "open failed: %d", 1);

    memset(&shared, 0, sizeof(shared));
    shared.queue = queue;

    CHECK(pthread_create(&consumer, NULL, consumer_thread, &shared) == 0,
          "consumer thread failed: %s", strerror(errno));
    CHECK(pthread_create(&producer, NULL, producer_thread, &shared) == 0,
          "producer thread failed: %s", strerror(errno));

    (void)pthread_join(producer, NULL);
    packet_queue_stop(queue);
    (void)pthread_join(consumer, NULL);

    CHECK(shared.mismatches == 0U,
          "%" PRIu64 " corrupted frames were delivered",
          shared.mismatches);
    CHECK(shared.popped > 0U, "consumer received nothing: %d", 1);

    packet_queue_stats(queue, &stats);
    /*
     * Two separate balances:
     *   offered == admitted + refused at admission
     *   admitted == consumed + discarded later + never fitted
     */
    CHECK(stats.pushed + stats.dropped_full == shared.pushed,
          "admitted %" PRIu64 " plus refused %" PRIu64 " must equal offered %"
          PRIu64,
          stats.pushed, stats.dropped_full, shared.pushed);
    CHECK(stats.pushed == stats.popped + stats.dropped_stale +
                              stats.dropped_oversize,
          "admitted %" PRIu64 " does not match consumed %" PRIu64
          " plus discards",
          stats.pushed, stats.popped);

    printf("threads: pushed=%" PRIu64 " popped=%" PRIu64
           " stale=%" PRIu64 " full=%" PRIu64 " peak=%zu\n",
           stats.pushed, stats.popped, stats.dropped_stale,
           stats.dropped_full, stats.peak_depth);

    packet_queue_destroy(queue);
}

static void test_stop_behaviour(void)
{
    struct packet_queue_config config;
    struct packet_queue *queue = NULL;
    const struct packet_queue_slot *slot = NULL;

    memset(&config, 0, sizeof(config));
    config.slot_count = 2U;
    config.slot_bytes = 1024U;
    CHECK(packet_queue_open(&config, &queue) == 0, "open failed: %d", 1);

    packet_queue_stop(queue);
    CHECK(packet_queue_acquire(queue, &slot, 0) == PACKET_QUEUE_STOPPED,
          "stopped empty queue must report stopped: %d", 1);

    packet_queue_destroy(queue);
}

int main(void)
{
    test_h264_flags();
    test_ordering_and_content();
    test_multiple_segments();
    test_oversize_frame();
    test_eviction_drops_whole_gop();
    test_borrowed_frame_is_never_overwritten();
    test_producer_consumer();
    test_stop_behaviour();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
