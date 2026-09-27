#include "sink_queue.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#define DRAIN_POLL_TIMEOUT_MS 100

static int sink_queue_write(struct packet_sink *sink,
                            uint64_t pts_us,
                            const struct packet_segment *segments,
                            size_t count)
{
    if (sink == NULL || sink->context == NULL) {
        return -1;
    }

    return packet_queue_push((struct packet_queue *)sink->context,
                             pts_us, segments, count);
}

static int sink_queue_flush(struct packet_sink *sink)
{
    if (sink == NULL || sink->context == NULL) {
        return -1;
    }

    return 0;
}

static void sink_queue_close(struct packet_sink *sink)
{
    if (sink == NULL) {
        return;
    }

    sink->context = NULL;
    free(sink);
}

static const struct packet_sink_ops sink_queue_ops = {
    sink_queue_write,
    sink_queue_flush,
    sink_queue_close
};

int sink_queue_open(struct packet_queue *queue, struct packet_sink **sink)
{
    struct packet_sink *created;

    if (queue == NULL || sink == NULL) {
        return -1;
    }
    *sink = NULL;

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        perror("sink_queue_open: calloc");
        return -1;
    }

    created->ops = &sink_queue_ops;
    created->context = queue;

    *sink = created;
    return 0;
}

struct queue_file_drain {
    struct packet_queue *queue;
    FILE *file;
    pthread_t thread;
    bool started;
};

static void *queue_file_drain_thread(void *argument)
{
    struct queue_file_drain *drain = argument;

    for (;;) {
        const struct packet_queue_slot *slot = NULL;
        int result;

        result = packet_queue_acquire(drain->queue, &slot,
                                      DRAIN_POLL_TIMEOUT_MS);
        if (result == PACKET_QUEUE_STOPPED) {
            break;
        }
        if (result != PACKET_QUEUE_OK) {
            continue;
        }

        if (slot->length > 0U &&
            fwrite(slot->data, 1U, slot->length, drain->file) !=
                slot->length) {
            fprintf(stderr, "Drain write failed: %s\n", strerror(errno));
            packet_queue_release(drain->queue);
            break;
        }

        packet_queue_release(drain->queue);
    }

    return NULL;
}

int queue_file_drain_start(struct packet_queue *queue,
                           const char *path,
                           struct queue_file_drain **drain)
{
    struct queue_file_drain *created;
    FILE *file;

    if (queue == NULL || path == NULL || drain == NULL) {
        return -1;
    }
    *drain = NULL;

    file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        return -1;
    }

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        (void)fclose(file);
        perror("queue_file_drain_start: calloc");
        return -1;
    }

    created->queue = queue;
    created->file = file;

    if (pthread_create(&created->thread, NULL,
                       queue_file_drain_thread, created) != 0) {
        fprintf(stderr, "queue_file_drain_start: pthread_create failed\n");
        (void)fclose(file);
        free(created);
        return -1;
    }
    created->started = true;

    *drain = created;
    return 0;
}

void queue_file_drain_stop(struct queue_file_drain *drain)
{
    if (drain == NULL) {
        return;
    }

    packet_queue_stop(drain->queue);

    if (drain->started) {
        (void)pthread_join(drain->thread, NULL);
        drain->started = false;
    }

    (void)fflush(drain->file);
    (void)fclose(drain->file);
    drain->file = NULL;
    free(drain);
}
