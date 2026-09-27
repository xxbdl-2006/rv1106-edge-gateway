/*
 * pthread_condattr_setclock() needs the XOPEN2K feature set. uclibc does not
 * enable it by default and would otherwise hide the declaration, so turn it on
 * before any system header is pulled in.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "packet_queue.h"

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

struct queue_slot {
    struct packet_queue_slot view;
    uint8_t *buffer;
    size_t capacity;
    enum slot_state state;
};

struct packet_queue {
    struct queue_slot *slots;
    size_t capacity;
    size_t slot_bytes;
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool stopped;
    uint64_t next_sequence;
    struct packet_queue_stats stats;
};

static void lock_queue(struct packet_queue *queue)
{
    (void)pthread_mutex_lock(&queue->mutex);
}

static void unlock_queue(struct packet_queue *queue)
{
    (void)pthread_mutex_unlock(&queue->mutex);
}

static void advance_head(struct packet_queue *queue)
{
    queue->head = (queue->head + 1U) % queue->capacity;
}

static void advance_tail(struct packet_queue *queue)
{
    queue->tail = (queue->tail + 1U) % queue->capacity;
}

/*
 * Free space by discarding the oldest frames.
 *
 * Frames are dropped until the oldest remaining frame is an IDR, so whatever
 * the consumer receives next is decodable on its own. A borrowed frame cannot
 * be touched, which means a slow consumer briefly caps how much we can drop.
 *
 * Returns true when at least one frame was freed.
 */
static bool evict_stale_frames(struct packet_queue *queue)
{
    bool freed = false;

    while (queue->count > 0U &&
           queue->slots[queue->tail].state == SLOT_READY) {
        struct queue_slot *oldest = &queue->slots[queue->tail];

        oldest->state = SLOT_FREE;
        oldest->view.length = 0U;
        oldest->view.flags = 0U;
        advance_tail(queue);
        queue->count--;
        queue->stats.dropped_stale++;
        freed = true;

        if (queue->count == 0U) {
            break;
        }

        if ((queue->slots[queue->tail].view.flags & H264_FLAG_KEY) != 0U) {
            break;
        }
    }

    return freed;
}

static void release_slots(struct packet_queue *queue)
{
    size_t index;

    for (index = 0U; index < queue->capacity; index++) {
        free(queue->slots[index].buffer);
        queue->slots[index].buffer = NULL;
        queue->slots[index].view.data = NULL;
    }

    free(queue->slots);
    queue->slots = NULL;
    queue->capacity = 0U;
}

int packet_queue_open(const struct packet_queue_config *config,
                      struct packet_queue **queue)
{
    struct packet_queue *created;
    pthread_condattr_t cond_attr;
    size_t slot_bytes;
    size_t slot_count;
    size_t index;

    if (queue == NULL) {
        return -1;
    }
    *queue = NULL;

    slot_count = (config != NULL && config->slot_count != 0U)
                     ? config->slot_count
                     : (size_t)PACKET_QUEUE_DEFAULT_SLOTS;
    slot_bytes = (config != NULL && config->slot_bytes != 0U)
                     ? config->slot_bytes
                     : (size_t)PACKET_QUEUE_DEFAULT_SLOT_BYTES;

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        perror("packet_queue_open: calloc");
        return -1;
    }

    created->slots = calloc(slot_count, sizeof(*created->slots));
    if (created->slots == NULL) {
        perror("packet_queue_open: calloc slots");
        free(created);
        return -1;
    }

    created->capacity = slot_count;
    created->slot_bytes = slot_bytes;

    for (index = 0U; index < slot_count; index++) {
        created->slots[index].buffer = malloc(slot_bytes);
        if (created->slots[index].buffer == NULL) {
            perror("packet_queue_open: malloc slot");
            release_slots(created);
            free(created);
            return -1;
        }
        created->slots[index].capacity = slot_bytes;
        created->slots[index].view.data = created->slots[index].buffer;
    }

    if (pthread_mutex_init(&created->mutex, NULL) != 0) {
        fprintf(stderr, "packet_queue_open: mutex init failed\n");
        release_slots(created);
        free(created);
        return -1;
    }

    if (pthread_condattr_init(&cond_attr) != 0) {
        fprintf(stderr, "packet_queue_open: condattr init failed\n");
        (void)pthread_mutex_destroy(&created->mutex);
        release_slots(created);
        free(created);
        return -1;
    }

    /*
     * Time out against CLOCK_MONOTONIC so that wall clock adjustments cannot
     * disturb the consumer timeout.
     */
    (void)pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);

    if (pthread_cond_init(&created->cond, &cond_attr) != 0) {
        fprintf(stderr, "packet_queue_open: cond init failed\n");
        (void)pthread_condattr_destroy(&cond_attr);
        (void)pthread_mutex_destroy(&created->mutex);
        release_slots(created);
        free(created);
        return -1;
    }

    (void)pthread_condattr_destroy(&cond_attr);

    *queue = created;
    return 0;
}

void packet_queue_destroy(struct packet_queue *queue)
{
    if (queue == NULL) {
        return;
    }

    (void)pthread_cond_destroy(&queue->cond);
    (void)pthread_mutex_destroy(&queue->mutex);
    release_slots(queue);
    free(queue);
}

int packet_queue_push(struct packet_queue *queue,
                      uint64_t pts_us,
                      const struct packet_segment *segments,
                      size_t count)
{
    struct queue_slot *target;
    uint8_t *cursor;
    size_t total = 0U;
    size_t index;

    if (queue == NULL || segments == NULL || count == 0U) {
        return 0;
    }

    for (index = 0U; index < count; index++) {
        if (segments[index].length == 0U) {
            continue;
        }
        if (segments[index].data == NULL) {
            return -1;
        }
        total += segments[index].length;
    }

    if (total == 0U) {
        return 0;
    }

    lock_queue(queue);

    if (total > queue->slot_bytes) {
        queue->stats.dropped_oversize++;
        unlock_queue(queue);
        return 0;
    }

    if (queue->count == queue->capacity && !evict_stale_frames(queue)) {
        queue->stats.dropped_full++;
        unlock_queue(queue);
        return 0;
    }

    target = &queue->slots[queue->head];
    cursor = target->buffer;

    for (index = 0U; index < count; index++) {
        if (segments[index].length == 0U) {
            continue;
        }
        memcpy(cursor, segments[index].data, segments[index].length);
        cursor += segments[index].length;
    }

    target->view.length = total;
    target->view.pts_us = pts_us;
    target->view.sequence = queue->next_sequence++;
    target->view.flags = h264_frame_flags(target->buffer, total);
    target->state = SLOT_READY;

    advance_head(queue);
    queue->count++;
    queue->stats.pushed++;

    if (queue->count > queue->stats.peak_depth) {
        queue->stats.peak_depth = queue->count;
    }

    (void)pthread_cond_signal(&queue->cond);
    unlock_queue(queue);

    return 0;
}

int packet_queue_acquire(struct packet_queue *queue,
                         const struct packet_queue_slot **slot,
                         int timeout_ms)
{
    struct queue_slot *head;

    if (queue == NULL || slot == NULL) {
        return PACKET_QUEUE_STOPPED;
    }

    lock_queue(queue);

    while (!queue->stopped && queue->count == 0U) {
        if (timeout_ms < 0) {
            (void)pthread_cond_wait(&queue->cond, &queue->mutex);
        } else {
            struct timespec deadline;
            int result;

            (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
            deadline.tv_sec += (time_t)(timeout_ms / 1000);
            deadline.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }

            result = pthread_cond_timedwait(&queue->cond,
                                            &queue->mutex,
                                            &deadline);
            if (result == ETIMEDOUT) {
                unlock_queue(queue);
                return PACKET_QUEUE_TIMEOUT;
            }
        }
    }

    if (queue->count == 0U) {
        unlock_queue(queue);
        return PACKET_QUEUE_STOPPED;
    }

    head = &queue->slots[queue->tail];
    head->state = SLOT_BORROWED;
    *slot = &head->view;

    unlock_queue(queue);
    return PACKET_QUEUE_OK;
}

void packet_queue_release(struct packet_queue *queue)
{
    struct queue_slot *head;

    if (queue == NULL) {
        return;
    }

    lock_queue(queue);

    if (queue->count == 0U) {
        unlock_queue(queue);
        return;
    }

    head = &queue->slots[queue->tail];
    if (head->state != SLOT_BORROWED) {
        unlock_queue(queue);
        return;
    }

    head->state = SLOT_FREE;
    head->view.length = 0U;
    head->view.flags = 0U;
    advance_tail(queue);
    queue->count--;
    queue->stats.popped++;

    unlock_queue(queue);
}

void packet_queue_stop(struct packet_queue *queue)
{
    if (queue == NULL) {
        return;
    }

    lock_queue(queue);
    queue->stopped = true;
    (void)pthread_cond_broadcast(&queue->cond);
    unlock_queue(queue);
}

void packet_queue_stats(struct packet_queue *queue,
                        struct packet_queue_stats *stats)
{
    if (queue == NULL || stats == NULL) {
        return;
    }

    lock_queue(queue);
    *stats = queue->stats;
    stats->depth = queue->count;
    unlock_queue(queue);
}
