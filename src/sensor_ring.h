#ifndef SENSOR_RING_H
#define SENSOR_RING_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensor_source.h"

/*
 * Bounded single producer / single consumer ring of sensor samples.
 *
 * Same shape and same guarantees as frame_ring and packet_queue, on purpose:
 * a reader who has understood one of the three has understood all of them, and
 * a reviewer comparing them is checking one policy instead of three different
 * ones.
 *
 *   - Every buffer is allocated once at open time. Neither push nor acquire
 *     allocates, which is what the handoff rule about allocation in the main
 *     loop requires.
 *   - push NEVER blocks. When the ring is full the OLDEST sample is
 *     overwritten.
 *
 * Why oldest rather than newest: this carries telemetry, not a video stream.
 * The consumer is an OSD that wants to draw the current attitude, and a
 * consumer that falls behind should catch up to reality rather than work
 * through a backlog of stale angles. Dropping the newest would mean a slow
 * consumer shows an ever-older reading while the physical board has moved on,
 * which is the opposite of what a level display is for. Contrast with
 * packet_queue, where dropping the oldest frame also has to respect GOP
 * boundaries; samples have no such structure, so plain overwrite is correct.
 *
 * Unusually for the three, the slots here hold values not bytes. A sensor
 * sample is a fixed size struct of plain floats, so there is no payload
 * pointer, no per-slot buffer and no copy size to get wrong - the slot IS the
 * storage. That removes a whole class of bug the byte-oriented rings have to
 * guard against, and it is the reason this file is shorter than frame_ring.c.
 */

#define SENSOR_RING_DEFAULT_SLOTS 16U

struct sensor_ring_config {
    size_t slot_count;
};

/* A borrowed sample. Valid until sensor_ring_release() is called. */
struct sensor_ring_slot {
    struct sensor_sample sample;
};

struct sensor_ring_stats {
    /*
     * Samples actually stored in a slot.
     *
     * This is NOT the number of push() calls: a push that finds every slot in
     * use (the consumer is holding the one recyclable slot) drops its sample
     * and does not count here. Use dropped_oldest to see the whole picture -
     * pushed + refused == calls, and pushed == popped + dropped_oldest + depth
     * always holds between them. Tests should assert the identity, not that
     * pushed equals the call count.
     */
    uint64_t pushed;
    /* Samples the consumer took and released. */
    uint64_t popped;
    /* Samples discarded before anyone consumed them, by overwrite or refusal. */
    uint64_t dropped_oldest;
    size_t depth;
    size_t peak_depth;
};

#define SENSOR_RING_OK 0
#define SENSOR_RING_TIMEOUT (-1)
#define SENSOR_RING_STOPPED (-2)

struct sensor_ring;

int sensor_ring_open(const struct sensor_ring_config *config,
                     struct sensor_ring **ring);

void sensor_ring_destroy(struct sensor_ring *ring);

/*
 * Hand one sample to the consumer. Never blocks and never fails on overflow;
 * overflow is reported through the statistics instead.
 *
 * Returns 0 in every non-fatal case, -1 only on internal misuse. Note that 0
 * means "the ring accepted the call", not "the sample was stored": when every
 * slot is in use the sample is dropped and 0 is still returned, because a
 * refused push is an ordinary full-ring outcome rather than an error. Callers
 * that need to know whether this particular sample survived should compare
 * stats.pushed before and after, or read dropped_oldest.
 */
int sensor_ring_push(struct sensor_ring *ring, const struct sensor_sample *sample);

/*
 * Borrow the oldest sample without copying.
 *
 * The slot stays owned by the ring until sensor_ring_release() is called, so
 * the producer cannot overwrite it meanwhile. At most one slot can be borrowed
 * at a time, exactly like the other two rings.
 *
 * timeout_ms < 0 waits forever.
 * Returns SENSOR_RING_OK, SENSOR_RING_TIMEOUT or SENSOR_RING_STOPPED.
 */
int sensor_ring_acquire(struct sensor_ring *ring,
                        const struct sensor_ring_slot **slot,
                        int timeout_ms);

void sensor_ring_release(struct sensor_ring *ring);

/*
 * Request shutdown. Pending samples stay readable; consumers drain them and
 * then receive SENSOR_RING_STOPPED.
 */
void sensor_ring_stop(struct sensor_ring *ring);

void sensor_ring_stats(struct sensor_ring *ring, struct sensor_ring_stats *stats);

#endif /* SENSOR_RING_H */
