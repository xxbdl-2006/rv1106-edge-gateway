#ifndef FRAME_RING_H
#define FRAME_RING_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Bounded single producer / single consumer ring of raw capture frames.
 *
 * Where packet_queue stores already encoded frames on their way out to the
 * network, this ring stores NV12 frames on their way IN to the encoder. It
 * exists to cut the capture loop loose from the encoder: today a slow encoder
 * stalls VIDIOC_DQBUF, so the sensor queues up behind us and, worse, a future
 * OSD or disk write would drag the capture cadence along with it.
 *
 * Same allocation rule as the packet queue: every buffer is allocated once at
 * creation time. Neither push nor acquire allocates while running, which is
 * what the handoff rule about "no unbounded allocation in the main loop" asks
 * for.
 *
 * Backpressure policy is also taken straight from the packet queue: push NEVER
 * blocks. When the ring is full the OLDEST frame is overwritten, so the
 * consumer always works on the freshest data and latency cannot grow without
 * bound. Dropping the newest frame instead would trade latency for a gap the
 * encoder cannot recover from.
 *
 * Frames carry an opaque payload byte count plus the capture metadata the
 * encoder needs (the V4L2 index, so the consumer can recycle the buffer). The
 * payload layout itself is left to the caller: this module copies bytes and
 * never interprets them.
 */

#define FRAME_RING_DEFAULT_SLOTS 3U
#define FRAME_RING_DEFAULT_SLOT_BYTES (1280U * 720U * 3U / 2U)

struct frame_ring_config {
    size_t slot_count;
    size_t slot_bytes;
};

/*
 * A borrowed frame. Valid only until frame_ring_release() is called, because
 * the producer may then reuse the buffer.
 */
struct frame_ring_slot {
    const uint8_t *data;
    size_t length;
    uint64_t pts_us;
    uint64_t sequence;
    unsigned index;
};

struct frame_ring_stats {
    /* Frames the producer handed over. */
    uint64_t pushed;
    /* Frames the consumer took and released. */
    uint64_t popped;
    /* Frames overwritten by a newer frame before anyone consumed them. */
    uint64_t dropped_oldest;
    /* Frames refused because one slot is smaller than the frame. */
    uint64_t dropped_oversize;
    /* Frames refused because the consumer was still borrowing. */
    uint64_t dropped_busy;
    size_t depth;
    size_t peak_depth;
};

#define FRAME_RING_OK 0
#define FRAME_RING_TIMEOUT (-1)
#define FRAME_RING_STOPPED (-2)

struct frame_ring;

int frame_ring_open(const struct frame_ring_config *config,
                    struct frame_ring **ring);

void frame_ring_destroy(struct frame_ring *ring);

/*
 * Hand one captured frame to the consumer. Never blocks, never fails on
 * overflow; overflow is reported through the statistics instead.
 * Returns 0 in every non fatal case, -1 only on internal misuse.
 */
int frame_ring_push(struct frame_ring *ring,
                    const void *data,
                    size_t length,
                    uint64_t pts_us,
                    unsigned index);

/*
 * Borrow the oldest frame without copying.
 *
 * The slot stays owned by the ring until frame_ring_release() is called, so
 * the producer cannot overwrite it in the meantime. At most one slot can be
 * borrowed at a time, exactly like the packet queue.
 *
 * timeout_ms < 0 waits forever.
 * Returns FRAME_RING_OK, FRAME_RING_TIMEOUT or FRAME_RING_STOPPED.
 */
int frame_ring_acquire(struct frame_ring *ring,
                       const struct frame_ring_slot **slot,
                       int timeout_ms);

void frame_ring_release(struct frame_ring *ring);

/*
 * Request shutdown. Pending frames stay readable; consumers drain them and
 * then receive FRAME_RING_STOPPED.
 */
void frame_ring_stop(struct frame_ring *ring);

void frame_ring_stats(struct frame_ring *ring, struct frame_ring_stats *stats);

#endif
