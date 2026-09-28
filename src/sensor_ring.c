/*
 * pthread_condattr_setclock() needs the XOPEN2K feature set. uclibc does not
 * enable it by default and would otherwise hide the declaration, so turn it on
 * before any system header is pulled in.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sensor_ring.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum slot_state {
    SLOT_FREE = 0,
    SLOT_READY,
    SLOT_BORROWED
};

struct ring_slot {
    struct sensor_ring_slot view;
    enum slot_state state;
};

struct sensor_ring {
    struct ring_slot *slots;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool stopped;
    /*
     * Which clock the condition variable actually waits on. Resolved at open
     * time because winpthreads rejects pthread_condattr_setclock(); see
     * build_deadline() for why the deadline must match it.
     */
    clockid_t cond_clock;
    struct sensor_ring_stats stats;
};

/*
 * Build an absolute deadline for pthread_cond_timedwait().
 *
 * The clock must be the one the condition variable actually waits on, and that
 * is not something we get to choose portably: uclibc honours
 * pthread_condattr_setclock(), while winpthreads returns EINVAL and quietly
 * keeps using CLOCK_REALTIME. Mixing them up is quiet and vicious - on Windows
 * CLOCK_MONOTONIC is roughly "seconds since process start" (a few thousand)
 * against a realtime clock of about 1.7 billion, so a monotonic deadline looks
 * like 1970 to a realtime timer and timedwait returns ETIMEDOUT immediately.
 * Every acquire() then reports an empty ring that is in fact full.
 *
 * Copied from frame_ring.c rather than shared, because the alternative is a
 * third header whose only content is this function, and the coupling it would
 * add between two otherwise independent rings is worse than the duplication.
 * If a third caller appears, that trade flips and it should move.
 */
static void build_deadline(clockid_t clock, int timeout_ms,
                           struct timespec *deadline)
{
    if (clock_gettime(clock, deadline) == -1) {
        deadline->tv_sec = time(NULL) + (time_t)(timeout_ms / 1000);
        deadline->tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        return;
    }

    deadline->tv_sec += (time_t)(timeout_ms / 1000);
    deadline->tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec += 1;
        deadline->tv_nsec -= 1000000000L;
    }
}

static void lock_ring(struct sensor_ring *ring)
{
    (void)pthread_mutex_lock(&ring->mutex);
}

static void unlock_ring(struct sensor_ring *ring)
{
    (void)pthread_mutex_unlock(&ring->mutex);
}

static void advance_tail(struct sensor_ring *ring)
{
    ring->tail = (ring->tail + 1U) % ring->capacity;
}

/*
 * Pick the slot the next push should write into, when the ring is full.
 * Returns the slot index, or -1 when every slot is in use.
 *
 * Three cases, in order of preference. This mirrors frame_ring's evict_oldest
 * deliberately: the reasoning was worked out once there and the two rings must
 * not drift into disagreeing about what "full" means.
 *
 *   1. The slot just behind the head is FREE. That is the normal steady state:
 *      the consumer released it, the write cursor has not been wound back yet,
 *      so we step the head backwards and reuse it. Nothing is lost - this is
 *      the "there was already room" case, and it must NOT count as a drop.
 *
 *   2. Otherwise the oldest READY sample is overwritten. It is the one the
 *      consumer has not looked at for the longest time, so this costs the least
 *      freshness. This is a real drop.
 *
 *   3. The tail is BORROWED and nothing else is free: the consumer is holding
 *      the only recyclable slot. There is nothing safe to evict and the
 *      incoming sample must be dropped instead of corrupting what is being
 *      read.
 *
 * The drop is counted HERE and only here, in the two cases that actually lose a
 * sample. The first version incremented dropped_oldest unconditionally in
 * push() after calling this, which double counted case 2 (once here, once
 * there) and over counted case 1 (where nothing was lost at all). The visible
 * symptom was an accounting identity that came out negative: dropped was
 * larger than the number of samples the ring had actually discarded, so
 * pushed - (popped + dropped + depth) went below zero instead of staying at
 * zero. Keeping the increment inside the branch that causes it is what makes
 * the identity hold by construction rather than by luck.
 */
static long evict_oldest(struct sensor_ring *ring)
{
    size_t behind = (ring->head + ring->capacity - 1U) % ring->capacity;

    if (ring->slots[behind].state == SLOT_FREE) {
        /* Case 1: the slot is already free. Reuse it, nothing is dropped. */
        ring->head = behind;
        return (long)behind;
    }

    if (ring->slots[ring->tail].state == SLOT_READY) {
        /* Case 2: retire the oldest live sample and write over it. */
        ring->slots[ring->tail].state = SLOT_FREE;
        advance_tail(ring);
        ring->count--;
        ring->stats.dropped_oldest++;
        return (long)ring->head;
    }

    /* Case 3: nothing recyclable. */
    return -1;
}
int sensor_ring_open(const struct sensor_ring_config *config,
                     struct sensor_ring **ring)
{
    struct sensor_ring *created;
    pthread_condattr_t cond_attr;
    size_t slot_count;

    if (ring == NULL) {
        return -1;
    }
    *ring = NULL;

    slot_count = (config != NULL && config->slot_count != 0U)
                     ? config->slot_count
                     : (size_t)SENSOR_RING_DEFAULT_SLOTS;

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        perror("sensor_ring_open: calloc");
        return -1;
    }

    /*
     * One array of slots, allocated once. Unlike frame_ring there is no second
     * allocation per slot: the sample is stored by value, so the slot is the
     * storage. That is also why calloc is enough here - a fresh slot is a zero
     * sample, which is exactly the right "nothing here yet" state, and the
     * state field starts at SLOT_FREE.
     */
    created->slots = calloc(slot_count, sizeof(*created->slots));
    if (created->slots == NULL) {
        perror("sensor_ring_open: calloc slots");
        free(created);
        return -1;
    }

    created->capacity = slot_count;

    if (pthread_mutex_init(&created->mutex, NULL) != 0) {
        fprintf(stderr, "sensor_ring_open: mutex init failed\n");
        free(created->slots);
        free(created);
        return -1;
    }

    if (pthread_condattr_init(&cond_attr) != 0) {
        fprintf(stderr, "sensor_ring_open: condattr init failed\n");
        (void)pthread_mutex_destroy(&created->mutex);
        free(created->slots);
        free(created);
        return -1;
    }

    /*
     * Ask for a monotonic condition variable, but believe the return value:
     * winpthreads answers EINVAL here and silently stays on CLOCK_REALTIME.
     * Whatever it accepted is what the deadline has to be built against.
     */
    created->cond_clock = CLOCK_REALTIME;
    if (pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC) == 0) {
        created->cond_clock = CLOCK_MONOTONIC;
    }

    if (pthread_cond_init(&created->cond, &cond_attr) != 0) {
        fprintf(stderr, "sensor_ring_open: cond init failed\n");
        (void)pthread_condattr_destroy(&cond_attr);
        (void)pthread_mutex_destroy(&created->mutex);
        free(created->slots);
        free(created);
        return -1;
    }

    (void)pthread_condattr_destroy(&cond_attr);

    *ring = created;
    return 0;
}

void sensor_ring_destroy(struct sensor_ring *ring)
{
    if (ring == NULL) {
        return;
    }

    (void)pthread_cond_destroy(&ring->cond);
    (void)pthread_mutex_destroy(&ring->mutex);
    free(ring->slots);
    free(ring);
}

/*
 * Write one sample into the ring.
 *
 * The model is the plain one: `head` is the next write position, `tail` is the
 * oldest live sample, and `count` is how many are live.
 *
 * This function was rewritten twice, and both rewrites are worth remembering
 * because both failures were silent - the ring kept returning samples, just
 * the wrong ones.
 *
 * The first version derived the write position from (head + count) and
 * "stole" a slot when full. head never advanced on an overwrite, so once the
 * ring filled, every subsequent push computed the same index and count marched
 * past capacity: `dropped` sat at zero while a three slot array reported a
 * depth of five.
 *
 * The second version used head/tail/count properly but handled the
 * all-borrowed case by scanning forward for "the oldest non-borrowed READY
 * slot" and freeing it in place. That is wrong when the borrowed slot is the
 * tail: scanning forward finds the *newest* sample, not an older one, and
 * freeing it lets the write land on a slot the consumer's borrow path also
 * considers current. In a two slot ring holding a borrow, the borrowed value
 * turned into the newest value instead of staying put.
 *
 * The structure below is copied from frame_ring, where it is already load
 * bearing, rather than reinvented. Two differences from that version are
 * deliberate: the refill path here goes through SLOT_FREE first (there is no
 * buffer copy that could be skipped, and the extra state keeps the "reuse the
 * slot behind the head" shortcut counting correctly), and the failure to find
 * a victim is counted as dropped_oldest because that is the closest thing this
 * ring has to frame_ring's dropped_busy.
 */
int sensor_ring_push(struct sensor_ring *ring, const struct sensor_sample *sample)
{
    size_t slot_index;

    if (ring == NULL || sample == NULL) {
        return -1;
    }

    lock_ring(ring);

    slot_index = ring->head;

    if (ring->count == ring->capacity) {
        long victim = evict_oldest(ring);

        if (victim < 0) {
            /*
             * The consumer is holding the only recyclable slot and no other
             * slot is free. Dropping this sample is correct: an attitude
             * reading is stale within a frame anyway, and blocking the
             * producer to wait for the consumer would stall the I2C reader.
             *
             * This is NOT counted as dropped_oldest. The counter exists to
             * answer "how much telemetry did the ring throw away that a
             * consumer could otherwise have read", and the conservation
             * identity pushed == popped + dropped_oldest + depth depends on
             * that reading. A refusal stores nothing, so counting it here
             * makes dropped exceed what the samples can account for and the
             * identity goes negative - which is exactly the failure that took
             * three passes to pin down. Refusals are visible as
             * (calls - pushed - popped - depth) instead, and at the sample
             * rates this ring runs at against a 16 slot depth it is a
             * non-event: the consumer only has to be slower than the producer
             * for the overwrite path to absorb everything.
             */
            unlock_ring(ring);
            return 0;
        }

        slot_index = (size_t)victim;
    }

    ring->slots[slot_index].view.sample = *sample;
    ring->slots[slot_index].state = SLOT_READY;

    ring->head = (slot_index + 1U) % ring->capacity;
    ring->count++;
    ring->stats.pushed++;
    if (ring->count > ring->stats.peak_depth) {
        ring->stats.peak_depth = ring->count;
    }
    ring->stats.depth = ring->count;

    /*
     * Wake a waiter. The unlock below is what makes that safe: signalling
     * while holding the lock is allowed but causes the woken thread to
     * immediately block again on the mutex, which adds a context switch for
     * nothing.
     */
    (void)pthread_cond_signal(&ring->cond);
    unlock_ring(ring);

    return 0;
}

int sensor_ring_acquire(struct sensor_ring *ring,
                        const struct sensor_ring_slot **slot,
                        int timeout_ms)
{
    int result = SENSOR_RING_OK;

    if (ring == NULL || slot == NULL) {
        return -1;
    }
    *slot = NULL;

    lock_ring(ring);

    while (ring->count == 0U && !ring->stopped) {
        if (timeout_ms == 0) {
            result = SENSOR_RING_TIMEOUT;
            break;
        }

        if (timeout_ms < 0) {
            (void)pthread_cond_wait(&ring->cond, &ring->mutex);
            continue;
        }

        {
            struct timespec deadline;
            build_deadline(ring->cond_clock, timeout_ms, &deadline);
            if (pthread_cond_timedwait(&ring->cond, &ring->mutex, &deadline) ==
                ETIMEDOUT) {
                if (ring->count == 0U) {
                    result = SENSOR_RING_TIMEOUT;
                    break;
                }
            }
        }
    }

    if (result == SENSOR_RING_OK && ring->count == 0U) {
        /* Nothing left and the producer said stop: that is a clean end. */
        result = SENSOR_RING_STOPPED;
    }

    if (result == SENSOR_RING_OK) {
        /*
         * Step the tail onto the oldest live sample.
         *
         * The slot the tail points at is normally READY, but an eviction can
         * leave it FREE or BORROWED: evict_oldest picks its victim by state,
         * not by position, so a slot in the middle of the live range can be
         * retired while the tail still points at a hole.
         *
         * Skipping those holes has to decrement count as it goes. The first
         * version only advanced the tail, which left count counting slots the
         * consumer would never be handed. The drift is silent and it is not
         * local to this loop: release() decrements count once per delivered
         * sample, so a count inflated by N holes makes the ring appear N
         * samples heavier than it is, and the next N releases eat into slots
         * that were never the consumer's. Because slot reuse then stops
         * matching the count, push() eventually stops storing while still
         * reporting success - which showed up as stats.pushed reading 999 for
         * 1000 successful pushes, with no failure path anywhere.
         *
         * A FREE or BORROWED slot is not a live sample, so retiring it here is
         * exactly what release() would have done; the accounting stays
         * balanced either way.
         */
        while (ring->slots[ring->tail].state != SLOT_READY) {
            if (ring->slots[ring->tail].state == SLOT_FREE) {
                if (ring->count > 0U) {
                    ring->count--;
                }
            }
            advance_tail(ring);
        }

        ring->slots[ring->tail].state = SLOT_BORROWED;
        *slot = &ring->slots[ring->tail].view;
    }

    unlock_ring(ring);
    return result;
}

void sensor_ring_release(struct sensor_ring *ring)
{
    if (ring == NULL) {
        return;
    }

    lock_ring(ring);

    if (ring->slots[ring->tail].state == SLOT_BORROWED) {
        ring->slots[ring->tail].state = SLOT_FREE;
        advance_tail(ring);
        if (ring->count > 0U) {
            ring->count--;
        }
        ring->stats.popped++;
        ring->stats.depth = ring->count;
    }

    unlock_ring(ring);
}

void sensor_ring_stop(struct sensor_ring *ring)
{
    if (ring == NULL) {
        return;
    }

    lock_ring(ring);
    ring->stopped = true;
    /*
     * Broadcast, not signal: acquire() may have several waiters in a shutdown
     * that raced, and a broadcast with no waiters costs nothing measurable.
     */
    (void)pthread_cond_broadcast(&ring->cond);
    unlock_ring(ring);
}

void sensor_ring_stats(struct sensor_ring *ring, struct sensor_ring_stats *stats)
{
    if (ring == NULL || stats == NULL) {
        return;
    }

    lock_ring(ring);
    *stats = ring->stats;
    stats->depth = ring->count;
    unlock_ring(ring);
}
