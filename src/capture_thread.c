/*
 * pthread_condattr_setclock() needs the XOPEN2K feature set. uclibc does not
 * enable it by default and would otherwise hide the declaration, so turn it on
 * before any system header is pulled in.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "capture_thread.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

#include "capture_signal.h"

/*
 * How many consecutive empty dequeues before the capture thread declares the
 * camera dead. A DQBUF timeout is normal while the sensor settles, but an
 * endless run of them is not, and retrying forever would spin a core.
 */
#define CAPTURE_THREAD_MAX_EMPTY 64U

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
 * The caller then sees "the camera produced nothing" while the capture thread
 * is producing millions of frames a second.
 *
 * So the caller records which clock the condattr call actually accepted, and
 * this builds the deadline against exactly that clock.
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

struct capture_thread {
    struct frame_ring *ring;
    pthread_t thread;
    bool running;

    capture_nv12_source source;
    capture_buffer_release release;
    void *capture_context;
    int source_timeout_ms;

    unsigned int warmup_frames;
    unsigned int warmup_done;

    /*
     * Guards ready/failed and the counters. The condition is raised once the
     * first frame has been pushed, so that capture_thread_wait_ready() gives
     * the encoder a ring that is actually populated.
     */
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    /*
     * Atomic because the capture thread tests it on every frame without taking
     * the mutex. Reading a plain bool here was a data race: the compiler was
     * free to hoist the load out of the loop, and the waiter then timed out
     * even though frames were flowing.
     */
    atomic_bool ready;
    bool failed;
    bool stopped;

    /*
     * Which clock the condition variable actually waits on. Resolved at start
     * time because winpthreads rejects pthread_condattr_setclock(); see
     * build_deadline() for why the two must agree.
     */
    clockid_t cond_clock;

    unsigned long captured;
    unsigned long skipped;
    unsigned long capture_errors;
};

/*
 * Publish the thread's result, exactly once.
 *
 * Both the success path and the "camera is dead" path land here, and whichever
 * gets there first wins: a later report must not erase an earlier failure.
 */
static void publish_result(struct capture_thread *thread, bool ok)
{
    (void)pthread_mutex_lock(&thread->mutex);

    if (!atomic_load_explicit(&thread->ready, memory_order_acquire)) {
        atomic_store_explicit(&thread->ready, true, memory_order_release);
        thread->failed = !ok;
    }

    (void)pthread_cond_broadcast(&thread->cond);
    (void)pthread_mutex_unlock(&thread->mutex);
}

static void *capture_thread_main(void *argument)
{
    struct capture_thread *thread = argument;
    unsigned int consecutive_empty = 0U;

    for (;;) {
        struct capture_frame_view view;
        bool ok;

        if (g_stop) {
            break;
        }

        memset(&view, 0, sizeof(view));

        /*
         * The callback performs VIDIOC_DQBUF and refills view. A false return
         * means "nothing this time" (timeout, warmup, stop) and never carries a
         * buffer we still have to recycle, which keeps this loop simple.
         */
        ok = thread->source(thread->capture_context, &view);
        if (!ok) {
            if (g_stop) {
                break;
            }

            /*
             * A healthy DQBUF returns quickly with a frame or reports a
             * timeout after a while. An immediately and repeatedly empty source
             * is a broken camera, and retrying it forever would burn a core
             * while producing nothing. Give up so the caller can react.
             */
            consecutive_empty++;
            if (consecutive_empty >= CAPTURE_THREAD_MAX_EMPTY) {
                publish_result(thread, false);
                break;
            }

            thread->skipped++;
            continue;
        }

        consecutive_empty = 0U;

        if (thread->warmup_done < thread->warmup_frames) {
            thread->warmup_done++;
            thread->skipped++;
            (void)thread->release(thread->capture_context, view.index);
            continue;
        }

        (void)frame_ring_push(thread->ring,
                              view.data,
                              view.length,
                              view.pts_us,
                              view.index);

        thread->captured++;

        /*
         * Read outside the mutex on purpose: this is the hot path and taking
         * the lock on every frame would be pure overhead. The flag is atomic,
         * so the check is still well defined, and publish_result() only acts
         * on the first call.
         */
        if (!atomic_load_explicit(&thread->ready, memory_order_acquire)) {
            publish_result(thread, true);
        }

        /*
         * Requeue immediately after the copy. The ring owns its own bytes, so
         * the driver may start filling this buffer again right away.
         */
        if (thread->release(thread->capture_context, view.index) == -1) {
            thread->capture_errors++;
        }
    }

    /*
     * Unblock anyone still parked in capture_thread_wait_ready(). A thread that
     * died on the "camera is dead" path has already published failed=true, and
     * publish_result() will not overwrite it; a thread that exited because of
     * g_stop publishes a clean result, which is correct - a requested stop is
     * not a failure.
     */
    publish_result(thread, true);

    (void)pthread_mutex_lock(&thread->mutex);
    thread->stopped = true;
    (void)pthread_cond_broadcast(&thread->cond);
    (void)pthread_mutex_unlock(&thread->mutex);

    return NULL;
}

int capture_thread_start(const struct capture_thread_config *config,
                         capture_nv12_source source,
                         capture_buffer_release release,
                         void *capture_context,
                         struct capture_thread **thread)
{
    struct capture_thread *created;
    struct frame_ring_config ring_config;
    pthread_condattr_t cond_attr;

    if (source == NULL || release == NULL || thread == NULL) {
        return -1;
    }
    *thread = NULL;

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        perror("capture_thread_start: calloc");
        return -1;
    }

    memset(&ring_config, 0, sizeof(ring_config));
    ring_config.slot_count = (config != NULL && config->ring_slots != 0U)
                                 ? config->ring_slots
                                 : (size_t)FRAME_RING_DEFAULT_SLOTS;
    ring_config.slot_bytes = (config != NULL && config->slot_bytes != 0U)
                                 ? config->slot_bytes
                                 : (size_t)FRAME_RING_DEFAULT_SLOT_BYTES;

    if (frame_ring_open(&ring_config, &created->ring) == -1) {
        free(created);
        return -1;
    }

    if (pthread_mutex_init(&created->mutex, NULL) != 0) {
        fprintf(stderr, "capture_thread_start: mutex init failed\n");
        frame_ring_destroy(created->ring);
        free(created);
        return -1;
    }

    /*
     * Time out against CLOCK_MONOTONIC, matching the deadline that
     * capture_thread_wait_ready() builds. A condition variable created with
     * the default attributes waits against the realtime clock, so pairing it
     * with a monotonic deadline makes timedwait return ETIMEDOUT immediately
     * (the monotonic value looks like ancient history). The symptom is nasty:
     * wait_ready() reports "no frames" while the capture thread is happily
     * producing millions of them.
     */
    if (pthread_condattr_init(&cond_attr) != 0) {
        fprintf(stderr, "capture_thread_start: condattr init failed\n");
        (void)pthread_mutex_destroy(&created->mutex);
        frame_ring_destroy(created->ring);
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
        fprintf(stderr, "capture_thread_start: cond init failed\n");
        (void)pthread_condattr_destroy(&cond_attr);
        (void)pthread_mutex_destroy(&created->mutex);
        frame_ring_destroy(created->ring);
        free(created);
        return -1;
    }

    (void)pthread_condattr_destroy(&cond_attr);

    created->source = source;
    created->release = release;
    created->capture_context = capture_context;
    created->warmup_frames = (config != NULL) ? config->warmup_frames : 0U;
    created->source_timeout_ms = (config != NULL) ? config->source_timeout_ms
                                                 : 0;

    if (pthread_create(&created->thread, NULL, capture_thread_main,
                       created) != 0) {
        fprintf(stderr, "capture_thread_start: pthread_create failed\n");
        (void)pthread_cond_destroy(&created->cond);
        (void)pthread_mutex_destroy(&created->mutex);
        frame_ring_destroy(created->ring);
        free(created);
        return -1;
    }
    created->running = true;

    *thread = created;
    return 0;
}

int capture_thread_wait_ready(struct capture_thread *thread, int timeout_ms)
{
    struct timespec deadline;
    int result = 0;

    if (thread == NULL) {
        return -1;
    }

    build_deadline(thread->cond_clock, timeout_ms, &deadline);

    (void)pthread_mutex_lock(&thread->mutex);
    while (!atomic_load_explicit(&thread->ready, memory_order_acquire)) {
        if (pthread_cond_timedwait(&thread->cond,
                                   &thread->mutex,
                                   &deadline) == ETIMEDOUT) {
            result = -1;
            break;
        }
    }
    if (result == 0 && thread->failed) {
        result = -1;
    }
    (void)pthread_mutex_unlock(&thread->mutex);

    return result;
}

void capture_thread_stop(struct capture_thread *thread)
{
    if (thread == NULL) {
        return;
    }

    /*
     * g_stop is the same flag the signal handler raises, which is what lets an
     * in-flight DQBUF poll end early instead of waiting out its timeout. The
     * ring stop below additionally releases a consumer that is parked in
     * frame_ring_acquire().
     */
    g_stop = 1;
    frame_ring_stop(thread->ring);

    if (thread->running) {
        (void)pthread_join(thread->thread, NULL);
        thread->running = false;
    }

    /*
     * Deliberately does NOT touch the ring: the encoder may still be holding a
     * borrowed frame. Capture first, drain the ring, then destroy.
     */
    (void)pthread_mutex_lock(&thread->mutex);
    thread->stopped = true;
    (void)pthread_cond_broadcast(&thread->cond);
    (void)pthread_mutex_unlock(&thread->mutex);
}

void capture_thread_destroy(struct capture_thread *thread)
{
    if (thread == NULL) {
        return;
    }

    if (thread->running) {
        capture_thread_stop(thread);
    }

    (void)pthread_cond_destroy(&thread->cond);
    (void)pthread_mutex_destroy(&thread->mutex);
    frame_ring_destroy(thread->ring);
    free(thread);
}

bool capture_thread_finished(struct capture_thread *thread)
{
    bool stopped;

    if (thread == NULL) {
        return true;
    }

    (void)pthread_mutex_lock(&thread->mutex);
    stopped = thread->stopped;
    (void)pthread_mutex_unlock(&thread->mutex);

    return stopped;
}

struct frame_ring *capture_thread_ring(struct capture_thread *thread)
{
    return (thread != NULL) ? thread->ring : NULL;
}

void capture_thread_stats(struct capture_thread *thread,
                          struct capture_thread_stats *stats)
{
    if (thread == NULL || stats == NULL) {
        return;
    }

    (void)pthread_mutex_lock(&thread->mutex);
    stats->captured = thread->captured;
    stats->skipped = thread->skipped;
    stats->capture_errors = thread->capture_errors;
    (void)pthread_mutex_unlock(&thread->mutex);
}

bool capture_thread_failed(struct capture_thread *thread)
{
    bool failed;

    if (thread == NULL) {
        return true;
    }

    (void)pthread_mutex_lock(&thread->mutex);
    failed = thread->failed;
    (void)pthread_mutex_unlock(&thread->mutex);

    return failed;
}
