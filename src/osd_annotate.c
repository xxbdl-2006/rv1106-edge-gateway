#include "osd_annotate.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Video range white. See the header and osd_overlay_composite_nv12. */
#define OSD_ANNOTATE_DEFAULT_LEVEL_ON 235U

struct osd_annotate {
    size_t width;
    size_t height;
    size_t frame_bytes;

    /*
     * The overlay, owned. NULL means pass-through, and is what makes the
     * disabled path free rather than merely cheap.
     */
    struct osd_telemetry *telemetry;

    /*
     * The copy the overlay is drawn into. Allocated once at open and never
     * resized, because osd_annotate_apply() runs per frame.
     */
    uint8_t *scratch;

    struct osd_annotate_stats stats;
};

int osd_annotate_open(const struct osd_annotate_config *config,
                      struct osd_telemetry *telemetry,
                      struct osd_annotate **annotate)
{
    struct osd_annotate *created;

    if (annotate == NULL) {
        errno = EINVAL;
        return -1;
    }
    *annotate = NULL;

    if (config == NULL || config->width == 0U || config->height == 0U) {
        errno = EINVAL;
        return -1;
    }

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        perror("osd_annotate_open: calloc");
        return -1;
    }

    created->width = config->width;
    created->height = config->height;
    created->telemetry = telemetry;

    /*
     * NV12 is a full luma plane followed by a quarter-size interleaved chroma
     * plane, so the total is 3/2 of the luma count. The same arithmetic the
     * capture path uses for its slot size; they must agree or the memcpy below
     * would run past the frame.
     *
     * The scratch is allocated even when there is no overlay. It costs 1.4 MB
     * for a 720p frame, which is not worth a branch that lets a caller pass a
     * telemetry object to a later call and find there is nowhere to draw - the
     * "no overlay" decision is made once, at open, and is captured by the
     * telemetry pointer being NULL.
     */
    created->frame_bytes = created->width * created->height * 3U / 2U;

    created->scratch = malloc(created->frame_bytes);
    if (created->scratch == NULL) {
        perror("osd_annotate_open: malloc scratch");
        free(created);
        return -1;
    }

    /*
     * The level is not stored: it is a property of the compositor, and the
     * telemetry layer already pins it. Kept in the config so a caller can
     * override it later without changing this signature.
     */
    (void)config->level_on;

    *annotate = created;
    return 0;
}

void osd_annotate_destroy(struct osd_annotate *annotate)
{
    if (annotate == NULL) {
        return;
    }

    /*
     * The telemetry is owned: it was handed over at open, and this is the only
     * place it is released. Freeing it anywhere else would leave this struct
     * holding a dangling pointer.
     */
    osd_telemetry_destroy(annotate->telemetry);
    free(annotate->scratch);
    free(annotate);
}

const uint8_t *osd_annotate_apply(struct osd_annotate *annotate,
                                  const uint8_t *frame,
                                  const struct osd_telemetry_input *input)
{
    size_t written;

    if (annotate == NULL || frame == NULL) {
        return frame;
    }

    /*
     * The disabled path. Returning the input pointer without touching the
     * scratch buffer is the whole of "zero cost when off": no memcpy, and
     * nothing for the default pipeline to pay for beyond one pointer test.
     */
    if (annotate->telemetry == NULL || input == NULL) {
        annotate->stats.passed_through++;
        return frame;
    }

    /*
     * Copy first, composite second. The order is what keeps the frame ring's
     * const a fact rather than a convention: compositing into the borrowed
     * buffer and copying out afterwards would mutate a frame the producer
     * thread owns, and the ring's own test asserts that a borrowed frame stays
     * stable across a producer push.
     */
    memcpy(annotate->scratch, frame, annotate->frame_bytes);

    written = osd_telemetry_render(annotate->telemetry, input,
                                   annotate->scratch, annotate->width,
                                   annotate->height);

    /*
     * A refused composite means the overlay does not fit this frame. Returning
     * the scratch - which is by now an exact copy of the input - would be
     * equivalent in content but would waste the copy on every frame, and would
     * hide the mismatch. Returning the input makes the failure cost nothing and
     * keeps the statistics honest about how often it happens.
     */
    if (written == 0U) {
        annotate->stats.composite_refused++;
        return frame;
    }

    annotate->stats.annotated++;
    return annotate->scratch;
}

void osd_annotate_stats(const struct osd_annotate *annotate,
                        struct osd_annotate_stats *stats)
{
    if (annotate == NULL || stats == NULL) {
        return;
    }

    *stats = annotate->stats;
}

const struct osd_overlay *osd_annotate_overlay(const struct osd_annotate *annotate)
{
    if (annotate == NULL) {
        return NULL;
    }

    return osd_telemetry_overlay(annotate->telemetry);
}

const uint8_t *osd_annotate_scratch(const struct osd_annotate *annotate)
{
    return annotate != NULL ? annotate->scratch : NULL;
}
