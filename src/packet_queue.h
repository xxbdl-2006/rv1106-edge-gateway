#ifndef PACKET_QUEUE_H
#define PACKET_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "h264_util.h"
#include "packet_sink.h"

/*
 * Bounded single producer / single consumer queue of encoded frames.
 *
 * Every resource is allocated once at creation time. Neither push nor acquire
 * allocates while the pipeline is running, which is required by the handoff
 * rules about never allocating inside the capture/encode loop.
 *
 * Backpressure policy: push NEVER blocks. When the queue is full the oldest
 * complete GOP is discarded so the queue starts again at an IDR (dropping the
 * middle of a GOP would leave the client with unreferenceable P frames). If
 * even that is impossible, the incoming frame is dropped. Either way the
 * encoder keeps running, which is what "client disconnect must not block the
 * encoder" means in practice.
 */

#define PACKET_QUEUE_DEFAULT_SLOTS 64U
#define PACKET_QUEUE_DEFAULT_SLOT_BYTES (256U * 1024U)

struct packet_queue_config {
    size_t slot_count;
    size_t slot_bytes;
};

struct packet_queue_slot {
    const uint8_t *data;
    size_t length;
    uint64_t pts_us;
    uint64_t sequence;
    unsigned flags;
};

struct packet_queue_stats {
    /* Frames admitted into the queue. */
    uint64_t pushed;
    /* Frames the consumer took and released. */
    uint64_t popped;
    /* Admitted frames later discarded to make room for newer data. */
    uint64_t dropped_stale;
    /* Frames refused at admission time because the queue had no room. */
    uint64_t dropped_full;
    /* Frames refused because one slot is smaller than the frame. */
    uint64_t dropped_oversize;
    size_t depth;
    size_t peak_depth;
};

#define PACKET_QUEUE_OK 0
#define PACKET_QUEUE_TIMEOUT (-1)
#define PACKET_QUEUE_STOPPED (-2)

struct packet_queue;

int packet_queue_open(const struct packet_queue_config *config,
                      struct packet_queue **queue);

void packet_queue_destroy(struct packet_queue *queue);

/*
 * Queue one encoded frame. Never blocks, never fails on overflow.
 * Returns 0 in every non fatal case; -1 only on internal misuse.
 */
int packet_queue_push(struct packet_queue *queue,
                      uint64_t pts_us,
                      const struct packet_segment *segments,
                      size_t count);

/*
 * Borrow the oldest frame without copying.
 *
 * The returned slot stays owned by the queue until packet_queue_release() is
 * called, so the producer cannot overwrite it. At most one slot can be
 * borrowed at a time.
 *
 * timeout_ms < 0 waits forever.
 * Returns PACKET_QUEUE_OK, PACKET_QUEUE_TIMEOUT or PACKET_QUEUE_STOPPED.
 */
int packet_queue_acquire(struct packet_queue *queue,
                         const struct packet_queue_slot **slot,
                         int timeout_ms);

void packet_queue_release(struct packet_queue *queue);

/*
 * Request shutdown. Pending frames stay readable; consumers drain them and
 * then receive PACKET_QUEUE_STOPPED.
 */
void packet_queue_stop(struct packet_queue *queue);

void packet_queue_stats(struct packet_queue *queue,
                        struct packet_queue_stats *stats);

#endif
