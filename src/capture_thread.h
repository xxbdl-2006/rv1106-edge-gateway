#ifndef CAPTURE_THREAD_H
#define CAPTURE_THREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame_ring.h"

/*
 * Runs the V4L2 dequeue/requeue half of the pipeline on its own thread.
 *
 * Before this existed the capture and the encode shared one loop: the encoder
 * was called directly after VIDIOC_DQBUF, so whenever the encoder took longer
 * than a frame interval the sensor buffer queue quietly filled up behind us and
 * the latency grew with it. With the capture on its own thread the only thing
 * that can delay a requeue is a memcpy into the ring, which is bounded and
 * never blocks.
 *
 * The thread does NOT understand the NV12 layout. It asks the caller's
 * nv12_source callback for the contiguous frame bytes and copies them into the
 * ring. That keeps every V4L2 detail in v4l2_capture.c where it already lived,
 * and keeps this file free of any camera dependency.
 *
 * Shutdown: capture_thread_stop() raises g_stop and stops the ring, then joins.
 * A DQBUF already in flight ends on the configured poll timeout, so the join is
 * bounded by that timeout instead of by an arbitrary sleep.
 */

struct capture_thread;

/*
 * One dequeued V4L2 buffer, ready for the ring.
 *
 *   data     tightly packed NV12 bytes, valid until the next call
 *   length   number of valid bytes
 *   index    V4L2 buffer index, handed back through the release callback
 *   pts_us   capture timestamp in microseconds, monotonic
 *
 * The callback returns false to skip the frame (warmup, empty buffer, stop).
 */
struct capture_frame_view {
    const uint8_t *data;
    size_t length;
    unsigned int index;
    uint64_t pts_us;
};

typedef bool (*capture_nv12_source)(void *capture_context,
                                    struct capture_frame_view *view);

/* Hand the V4L2 buffer back to the driver. Returns 0 on success, -1 on error. */
typedef int (*capture_buffer_release)(void *capture_context,
                                      unsigned int buffer_index);

struct capture_thread_config {
    size_t ring_slots;
    size_t slot_bytes;
    unsigned int warmup_frames;
    int source_timeout_ms;
};

struct capture_thread_stats {
    unsigned long captured;
    unsigned long skipped;
    unsigned long capture_errors;
};

int capture_thread_start(const struct capture_thread_config *config,
                         capture_nv12_source source,
                         capture_buffer_release release,
                         void *capture_context,
                         struct capture_thread **thread);

/*
 * Wait until the first frame has reached the ring, so the encoder does not
 * start on an empty ring and afterwards report a bogus frame rate.
 * Returns 0 on success, -1 on timeout or capture failure.
 */
int capture_thread_wait_ready(struct capture_thread *thread, int timeout_ms);

/*
 * Request shutdown and join the thread. Does NOT free the ring, because the
 * encoder may still be holding a borrowed frame: call this first, drain the
 * ring, then call capture_thread_destroy().
 */
void capture_thread_stop(struct capture_thread *thread);

/* Join if needed and release the thread, its ring and its mutex. */
void capture_thread_destroy(struct capture_thread *thread);

/* True once stop() has joined the thread, or the thread left on its own. */
bool capture_thread_finished(struct capture_thread *thread);

struct frame_ring *capture_thread_ring(struct capture_thread *thread);

void capture_thread_stats(struct capture_thread *thread,
                          struct capture_thread_stats *stats);

/* True once the capture side has hit an unrecoverable error. */
bool capture_thread_failed(struct capture_thread *thread);

#endif
