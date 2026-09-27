#include "sink_file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sink_file_context {
    FILE *file;
};

static int sink_file_write(struct packet_sink *sink,
                           uint64_t pts_us,
                           const struct packet_segment *segments,
                           size_t count)
{
    struct sink_file_context *context;
    size_t index;

    if (sink == NULL || sink->context == NULL) {
        return -1;
    }
    context = sink->context;

    for (index = 0U; index < count; index++) {
        if (segments[index].length == 0U) {
            continue;
        }

        if (fwrite(segments[index].data, 1U, segments[index].length,
                   context->file) != segments[index].length) {
            fprintf(stderr, "Failed to write H.264 packet: %s\n",
                    strerror(errno));
            return -1;
        }
    }

    (void)pts_us;
    return 0;
}

static int sink_file_flush(struct packet_sink *sink)
{
    struct sink_file_context *context;

    if (sink == NULL || sink->context == NULL) {
        return -1;
    }
    context = sink->context;

    if (fflush(context->file) == EOF) {
        return -1;
    }

    return 0;
}

static void sink_file_close(struct packet_sink *sink)
{
    struct sink_file_context *context;

    if (sink == NULL || sink->context == NULL) {
        return;
    }
    context = sink->context;

    (void)fflush(context->file);
    (void)fclose(context->file);
    free(context);
    free(sink);
}

static const struct packet_sink_ops sink_file_ops = {
    sink_file_write,
    sink_file_flush,
    sink_file_close
};

int sink_file_open(const char *path, struct packet_sink **sink)
{
    struct sink_file_context *context;
    struct packet_sink *created;
    FILE *file;

    if (path == NULL || sink == NULL) {
        return -1;
    }
    *sink = NULL;

    file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        return -1;
    }

    context = calloc(1U, sizeof(*context));
    if (context == NULL) {
        (void)fclose(file);
        perror("sink_file_open: calloc");
        return -1;
    }

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        free(context);
        (void)fclose(file);
        perror("sink_file_open: calloc");
        return -1;
    }

    context->file = file;
    created->ops = &sink_file_ops;
    created->context = context;

    *sink = created;
    return 0;
}
