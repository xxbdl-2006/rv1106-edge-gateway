#ifndef PACKET_SINK_H
#define PACKET_SINK_H

#include <stddef.h>
#include <stdint.h>

/*
 * One contiguous run of encoded bytes inside MPP/Rockit encoder memory.
 *
 * A single encoded frame comes back from RK_MPI_VENC_GetStream as a small
 * list of these runs (VENC_PACK_S with several stPackInfo entries, or plain
 * u32Offset/u32Len). Sinks therefore receive scatter/gather input so that no
 * intermediate copy is needed just to hand a frame over.
 *
 * The pointers are only valid until the encoder releases the stream, so every
 * sink MUST consume or copy them before returning.
 */
struct packet_segment {
    const uint8_t *data;
    size_t length;
};

/*
 * Upper bound for the segments of one encoded frame. The allocator in
 * mpp_encoder.c refuses frames that need more, which keeps the gather array on
 * the stack. 8 is the observed worst case with this SDK.
 */
#define PACKET_SINK_MAX_SEGMENTS 16

struct packet_sink;

struct packet_sink_ops {
    /*
     * Consume one encoded frame.
     *
     * pts_us        presentation timestamp in microseconds, monotonic.
     * segments      count entries, all valid for the duration of the call.
     *
     * Returns 0 on success, -1 on a fatal error that must stop the pipeline.
     * Dropping a frame (queue overflow, optional sinks) is NOT an error.
     */
    int (*write)(struct packet_sink *sink,
                 uint64_t pts_us,
                 const struct packet_segment *segments,
                 size_t count);

    /* Push buffered data outwards. Returns 0 on success, -1 on error. */
    int (*flush)(struct packet_sink *sink);

    /*
     * Release every resource owned by the sink, including the sink itself.
     * The pointer must not be used afterwards. Must tolerate being called
     * more than once on the same pointer only if the caller cleared it.
     */
    void (*close)(struct packet_sink *sink);
};

struct packet_sink {
    const struct packet_sink_ops *ops;
    void *context;
};

static inline int packet_sink_write(struct packet_sink *sink,
                                    uint64_t pts_us,
                                    const struct packet_segment *segments,
                                    size_t count)
{
    if (sink == NULL || sink->ops == NULL || sink->ops->write == NULL) {
        return -1;
    }

    if (segments == NULL || count == 0U) {
        return 0;
    }

    return sink->ops->write(sink, pts_us, segments, count);
}

static inline int packet_sink_flush(struct packet_sink *sink)
{
    if (sink == NULL || sink->ops == NULL || sink->ops->flush == NULL) {
        return -1;
    }

    return sink->ops->flush(sink);
}

static inline void packet_sink_close(struct packet_sink *sink)
{
    if (sink == NULL || sink->ops == NULL || sink->ops->close == NULL) {
        return;
    }

    sink->ops->close(sink);
}

#endif
