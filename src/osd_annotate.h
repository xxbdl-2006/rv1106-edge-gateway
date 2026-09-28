#ifndef OSD_ANNOTATE_H
#define OSD_ANNOTATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "osd_telemetry.h"

/*
 * Turns a captured frame plus a telemetry reading into the frame to encode.
 *
 * WHY THIS IS NOT DONE IN PLACE. The frame arrives from the frame ring as a
 * `const uint8_t *` borrow, and that const is the ring's contract, not a
 * formality: the producer thread owns the buffer and may overwrite it the
 * moment the borrow is released. Drawing into it would mean either casting the
 * const away - which silently breaks the ring's promise that a borrowed frame
 * is stable, a property its test asserts - or widening the borrow, which would
 * hold a slot for the whole composite and put the OSD back in the path of the
 * capture cadence. The ring exists precisely to keep those two apart.
 *
 * So the annotator owns one scratch frame, allocated once at open, and the
 * pipeline copies into it before compositing. That copy is the cost of keeping
 * the ring's contract intact, and it is a good trade: 1.4 MB at 30 fps is about
 * 40 MB/s of sequential memcpy, which is a small fraction of what the encoder
 * itself moves over the same buffer.
 *
 * WHY THIS IS A SEPARATE FILE. Everything below it is V4L2 and the Ring, and
 * everything above it is the SDK's encoder. This file touches neither - it is
 * "given these bytes, produce those bytes" - so the whole annotate-and-composite
 * path runs and is tested on the host, which is the only way to assert that the
 * overlay actually appears in the encoded frame rather than hoping a human
 * notices it on a monitor.
 *
 * ZERO COST WHEN DISABLED. osd_annotate_apply() returns the input pointer
 * unchanged, without touching the scratch buffer, when telemetry is NULL. The
 * default pipeline (no --osd) therefore pays one branch per frame and no copy
 * at all, which matters because this runs in the loop that has to hold 30 fps on
 * a single core.
 */

struct osd_annotate;

struct osd_annotate_config {
    /* Frame geometry, matching the capture format. */
    size_t width;
    size_t height;
    /*
     * Luma written for a set overlay pixel. See osd_overlay_composite_nv12.
     * 0 uses 235, the top of the video range.
     */
    uint8_t level_on;
};

struct osd_annotate_stats {
    /* Frames copied and composited. */
    unsigned long annotated;
    /* Frames returned unchanged because there was nothing to draw. */
    unsigned long passed_through;
    /* Frames where the overlay did not fit and nothing was drawn. */
    unsigned long composite_refused;
};

/*
 * Create an annotator.
 *
 * `telemetry` is the overlay to draw, and this takes ownership of it: it is
 * destroyed by osd_annotate_destroy(). Passing NULL is legal and means "no
 * overlay", which makes osd_annotate_apply() a pass-through.
 *
 * The split is deliberate. The telemetry object owns the canvas and is built
 * once; the annotator owns the scratch frame and the geometry. Neither knows
 * about the other's concern, and a future change that composites with RGA
 * replaces this file without touching the telemetry layer.
 *
 * Allocates the scratch frame once; no later call allocates.
 * Returns 0 on success, -1 on failure with errno set.
 */
int osd_annotate_open(const struct osd_annotate_config *config,
                      struct osd_telemetry *telemetry,
                      struct osd_annotate **annotate);

void osd_annotate_destroy(struct osd_annotate *annotate);

/*
 * Produce the frame to encode.
 *
 * `frame` is a tightly packed NV12 plane of width x height. If an overlay was
 * supplied at open time the overlay is drawn into a private copy and that copy
 * is returned; otherwise `frame` itself is returned and nothing is copied.
 *
 * The returned pointer is owned by the annotator and is stable until the next
 * call, exactly like the pointer passed in. The input buffer is never modified.
 *
 * Also returns `frame` unchanged when the composite is refused (the overlay does
 * not fit), so a mismatched geometry degrades to "no overlay" rather than to a
 * frame the encoder cannot use.
 */
const uint8_t *osd_annotate_apply(struct osd_annotate *annotate,
                                  const uint8_t *frame,
                                  const struct osd_telemetry_input *input);

void osd_annotate_stats(const struct osd_annotate *annotate,
                        struct osd_annotate_stats *stats);

/* The overlay in use, or NULL if this annotator passes frames through. */
const struct osd_overlay *osd_annotate_overlay(const struct osd_annotate *annotate);

/*
 * The scratch frame, for tests and for a future hardware path that wants to
 * composite with RGA instead of writing the bytes itself. NULL for a NULL
 * annotator.
 */
const uint8_t *osd_annotate_scratch(const struct osd_annotate *annotate);

#endif /* OSD_ANNOTATE_H */
