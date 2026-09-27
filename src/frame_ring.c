/*
 * pthread_condattr_setclock() needs the XOPEN2K feature set. uclibc does not
 * enable it by default and would otherwise hide the declaration, so turn it on
 * before any system header is pulled in.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "frame_ring.h"

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
    struct frame_ring_slot view;
    uint8_t *buffer;
    size_t capacity;
    enum slot_state state;
};

struct frame_ring {
    struct ring_slot *slots;
    size_t capacity;
    size_t slot_bytes;
    size_t head;
    size_t tail;
    size_t count;
    uint64_t next_sequence;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool stopped;
    /*
     * Which clock the condition variable actually waits on. Resolved at open
     * time because winpthreads rejects pthread_condattr_setclock(); see
     * build_deadline() for why the deadline must match it.
     */
    clockid_t cond_clock;
    struct frame_ring_stats stats;
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

static void lock_ring(struct frame_ring *ring)
{
    (void)pthread_mutex_lock(&ring->mutex);
}

static void unlock_ring(struct frame_ring *ring)
{
    (void)pthread_mutex_unlock(&ring->mutex);
}

static void advance_tail(struct frame_ring *ring)
{
    ring->tail = (ring->tail + 1U) % ring->capacity;
}

static void release_slots(struct frame_ring *ring)
{
    size_t index;

    for (index = 0U; index < ring->capacity; index++) {
        free(ring->slots[index].buffer);
        ring->slots[index].buffer = NULL;
        ring->slots[index].view.data = NULL;
    }

    free(ring->slots);
    ring->slots = NULL;
    ring->capacity = 0U;
}

int frame_ring_open(const struct frame_ring_config *config,
                    struct frame_ring **ring)
{
    struct frame_ring *created;
    pthread_condattr_t cond_attr;
    size_t slot_bytes;
    size_t slot_count;
    size_t index;

    if (ring == NULL) {
        return -1;
    }
    *ring = NULL;

    slot_count = (config != NULL && config->slot_count != 0U)
                     ? config->slot_count
                     : (size_t)FRAME_RING_DEFAULT_SLOTS;
    slot_bytes = (config != NULL && config->slot_bytes != 0U)
                     ? config->slot_bytes
                     : (size_t)FRAME_RING_DEFAULT_SLOT_BYTES;

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        perror("frame_ring_open: calloc");
        return -1;
    }

    created->slots = calloc(slot_count, sizeof(*created->slots));
    if (created->slots == NULL) {
        perror("frame_ring_open: calloc slots");
        free(created);
        return -1;
    }

    created->capacity = slot_count;
    created->slot_bytes = slot_bytes;

    for (index = 0U; index < slot_count; index++) {
        created->slots[index].buffer = malloc(slot_bytes);
        if (created->slots[index].buffer == NULL) {
            perror("frame_ring_open: malloc slot");
            release_slots(created);
            free(created);
            return -1;
        }
        created->slots[index].capacity = slot_bytes;
        created->slots[index].view.data = created->slots[index].buffer;
    }

    if (pthread_mutex_init(&created->mutex, NULL) != 0) {
        fprintf(stderr, "frame_ring_open: mutex init failed\n");
        release_slots(created);
        free(created);
        return -1;
    }

    if (pthread_condattr_init(&cond_attr) != 0) {
        fprintf(stderr, "frame_ring_open: condattr init failed\n");
        (void)pthread_mutex_destroy(&created->mutex);
        release_slots(created);
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
        fprintf(stderr, "frame_ring_open: cond init failed\n");
        (void)pthread_condattr_destroy(&cond_attr);
        (void)pthread_mutex_destroy(&created->mutex);
        release_slots(created);
        free(created);
        return -1;
    }

    (void)pthread_condattr_destroy(&cond_attr);

    *ring = created;
    return 0;
}

void frame_ring_destroy(struct frame_ring *ring)
{
    if (ring == NULL) {
        return;
    }

    (void)pthread_cond_destroy(&ring->cond);
    (void)pthread_mutex_destroy(&ring->mutex);
    release_slots(ring);
    free(ring);
}

/*
 * Find a slot the producer may write into, given that the ring reports itself
 * full. Returns the slot index, or -1 when every slot is in use.
 *
 * Three cases, in order of preference:
 *
 *   1. The slot just behind the head is FREE. That is the normal steady state:
 *      the consumer released it, the write cursor has not been wound back yet,
 *      so we simply step the head backwards and reuse it. Nothing is lost.
 *
 *   2. Otherwise the oldest READY frame is overwritten. It is the one the
 *      consumer has not looked at for the longest time, so this costs the least
 *      freshness and keeps latency bounded.
 *
 *   3. The tail is BORROWED and everything else is full: the consumer is
 *      holding the only recyclable slot. There is nothing safe to evict and the
 *      caller must drop the incoming frame instead.
 */
static long evict_oldest(struct frame_ring *ring)
{
    size_t behind = (ring->head + ring->capacity - 1U) % ring->capacity;

    if (ring->slots[behind].state == SLOT_FREE) {
        ring->head = behind;
        return (long)behind;
    }

    if (ring->slots[ring->tail].state == SLOT_READY) {
        struct ring_slot *oldest = &ring->slots[ring->tail];

        oldest->state = SLOT_FREE;
        oldest->view.length = 0U;
        ring->stats.dropped_oldest++;
        advance_tail(ring);
        ring->count--;
        return (long)ring->head;
    }

    return -1;
}

int frame_ring_push(struct frame_ring *ring,
                    const void *data,
                    size_t length,
                    uint64_t pts_us,
                    unsigned index)
{
    struct ring_slot *target;
    long slot_index;

    if (ring == NULL || data == NULL) {
        return 0;
    }

    if (length == 0U) {
        return 0;
    }

    lock_ring(ring);

    if (length > ring->slot_bytes) {
        ring->stats.dropped_oversize++;
        unlock_ring(ring);
        return 0;
    }

    slot_index = (long)ring->head;

    if (ring->count == ring->capacity) {
        slot_index = evict_oldest(ring);
        if (slot_index < 0) {
            /*
             * The consumer is holding the only slot that could be recycled and
             * no other slot is free. Losing this frame is correct: blocking
             * here would stall V4L2 and push the delay into the sensor.
             */
            ring->stats.dropped_busy++;
            unlock_ring(ring);
            return 0;
        }
    }

    target = &ring->slots[slot_index];

    memcpy(target->buffer, data, length);

    target->view.length = length;
    target->view.pts_us = pts_us;
    target->view.sequence = ring->next_sequence++;
    target->view.index = index;
    target->state = SLOT_READY;

    ring->head = ((size_t)slot_index + 1U) % ring->capacity;
    ring->count++;
    ring->stats.pushed++;

    if (ring->count > ring->stats.peak_depth) {
        ring->stats.peak_depth = ring->count;
    }

    (void)pthread_cond_signal(&ring->cond);
    unlock_ring(ring);

    return 0;
}

int frame_ring_acquire(struct frame_ring *ring,
                       const struct frame_ring_slot **slot,
                       int timeout_ms)
{
    struct ring_slot *head;

    if (ring == NULL || slot == NULL) {
        return FRAME_RING_STOPPED;
    }

    lock_ring(ring);

    while (!ring->stopped && ring->count == 0U) {
        if (timeout_ms < 0) {
            (void)pthread_cond_wait(&ring->cond, &ring->mutex);
        } else {
            struct timespec deadline;
            int result;

            build_deadline(ring->cond_clock, timeout_ms, &deadline);

            result = pthread_cond_timedwait(&ring->cond,
                                            &ring->mutex,
                                            &deadline);
            if (result == ETIMEDOUT) {
                unlock_ring(ring);
                return FRAME_RING_TIMEOUT;
            }
        }
    }

    if (ring->count == 0U) {
        unlock_ring(ring);
        return FRAME_RING_STOPPED;
    }

    head = &ring->slots[ring->tail];
    head->state = SLOT_BORROWED;
    *slot = &head->view;

    unlock_ring(ring);
    return FRAME_RING_OK;
}

void frame_ring_release(struct frame_ring *ring)
{
    struct ring_slot *head;

    if (ring == NULL) {
        return;
    }

    lock_ring(ring);

    if (ring->count == 0U) {
        unlock_ring(ring);
        return;
    }

    head = &ring->slots[ring->tail];
    if (head->state != SLOT_BORROWED) {
        unlock_ring(ring);
        return;
    }

    head->state = SLOT_FREE;
    head->view.length = 0U;
    advance_tail(ring);
    ring->count--;
    ring->stats.popped++;

    unlock_ring(ring);
}

void frame_ring_stop(struct frame_ring *ring)
{
    if (ring == NULL) {
        return;
    }

    lock_ring(ring);
    ring->stopped = true;
    (void)pthread_cond_broadcast(&ring->cond);
    unlock_ring(ring);
}

void frame_ring_stats(struct frame_ring *ring, struct frame_ring_stats *stats)
{
    if (ring == NULL || stats == NULL) {
        return;
    }

    lock_ring(ring);
    *stats = ring->stats;
    stats->depth = ring->count;
    unlock_ring(ring);
}
