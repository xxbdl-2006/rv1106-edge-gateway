#include "osd_overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osd_font.h"

/* Canvas defaults: big enough for a dozen short lines at a readable scale. */
#define OSD_DEFAULT_WIDTH 320U
#define OSD_DEFAULT_HEIGHT 120U

/*
 * A blank canvas means "no panel here". Everything below relies on this one
 * convention: a clear pixel is transparent and leaves the frame alone, and a
 * set pixel is opaque and writes channel_on. The background panel works by
 * setting a whole rectangle, so the only genuinely transparent part of the
 * overlay is the margin outside the panel.
 */
struct osd_overlay {
    uint8_t *canvas;      /* 1bpp, rows byte aligned, MSB first */
    size_t row_bytes;
    size_t width;
    size_t height;
    unsigned scale;

    size_t cursor_x;
    size_t cursor_y;

    struct osd_overlay_stats stats;
};

static void set_pixel(struct osd_overlay *overlay, size_t x, size_t y, bool on)
{
    uint8_t *byte;
    uint8_t mask;

    if (x >= overlay->width || y >= overlay->height) {
        return;
    }

    byte = &overlay->canvas[y * overlay->row_bytes + (x >> 3)];
    mask = (uint8_t)(0x80U >> (x & 7U));

    if (on) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t)~mask;
    }
}

int osd_overlay_open(const struct osd_overlay_config *config,
                     struct osd_overlay **overlay)
{
    struct osd_overlay *created;
    size_t width;
    size_t height;

    if (overlay == NULL) {
        return -1;
    }
    *overlay = NULL;

    width = (config != NULL && config->width != 0U) ? config->width
                                                     : OSD_DEFAULT_WIDTH;
    height = (config != NULL && config->height != 0U) ? config->height
                                                       : OSD_DEFAULT_HEIGHT;

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        perror("osd_overlay_open: calloc");
        return -1;
    }

    /*
     * Rows are byte aligned. That wastes at most 7 bits per row, which is
     * nothing, and it means the row address is a multiply rather than a shift
     * with a remainder term - and more importantly it means a row can be
     * memcpy'd or handed to RGA as a unit, which a bit-continuous layout
     * would not allow.
     */
    created->row_bytes = (width + 7U) / 8U;
    created->width = width;
    created->height = height;
    created->scale = (config != NULL && config->scale != 0U) ? config->scale
                                                             : 1U;

    created->canvas = calloc(created->row_bytes, height);
    if (created->canvas == NULL) {
        perror("osd_overlay_open: calloc canvas");
        free(created);
        return -1;
    }

    *overlay = created;
    return 0;
}

void osd_overlay_destroy(struct osd_overlay *overlay)
{
    if (overlay == NULL) {
        return;
    }

    free(overlay->canvas);
    free(overlay);
}

void osd_overlay_reset(struct osd_overlay *overlay)
{
    if (overlay == NULL) {
        return;
    }

    memset(overlay->canvas, 0, overlay->row_bytes * overlay->height);
    overlay->cursor_x = 0U;
    overlay->cursor_y = 0U;
}

void osd_overlay_move_to(struct osd_overlay *overlay, size_t x, size_t y)
{
    if (overlay == NULL) {
        return;
    }

    /*
     * Clamp rather than refuse. A caller computing a position from data can
     * land outside the canvas for legitimate reasons (a longer string than
     * expected, a resized frame), and drawing at the edge is more useful than
     * drawing nothing.
     */
    overlay->cursor_x = x < overlay->width ? x : overlay->width;
    overlay->cursor_y = y < overlay->height ? y : overlay->height;
}

/*
 * Draw one glyph at (x, y) at 1x, in canvas pixels.
 *
 * Clipping is per pixel rather than per glyph, so a glyph on the right edge
 * shows its left half instead of vanishing. That matters because the overlay
 * width is a round number chosen for the layout, not a multiple of 6, so the
 * last character of a long line routinely overhangs.
 *
 * When `knockout` is true the sense is inverted: the whole cell is set and
 * the glyph's own pixels are cleared, which is how dark letters get cut out
 * of a light panel on a two-colour canvas. See the header.
 */
static void draw_glyph(struct osd_overlay *overlay, const struct osd_glyph *glyph,
                       size_t x, size_t y, bool knockout)
{
    size_t col;

    for (col = 0; col < OSD_FONT_WIDTH; col++) {
        uint8_t bits = glyph != NULL ? glyph->columns[col] : 0U;
        unsigned row;

        for (row = 0; row < OSD_FONT_HEIGHT; row++) {
            bool ink = ((bits >> row) & 1U) != 0U;

            if (knockout) {
                set_pixel(overlay, x + col, y + row, !ink);
            } else if (ink) {
                set_pixel(overlay, x + col, y + row, true);
            }
        }
    }
}

/*
 * Shared body of puts() and puts_knockout().
 *
 * The two differ only in how a glyph's pixels map onto the canvas, and in
 * knockout the cell must be filled even where the glyph has no ink, so the
 * cell width is OSD_FONT_WIDTH rather than the glyph's own extent.
 */
static bool puts_impl(struct osd_overlay *overlay, const char *text,
                      bool knockout)
{
    size_t x;
    size_t y;
    size_t i;

    if (overlay == NULL || text == NULL) {
        return false;
    }

    x = overlay->cursor_x;
    y = overlay->cursor_y;

    /*
     * Past the bottom: nothing to draw, and the caller should know. This is
     * the case that catches an overlay whose layout has outgrown its canvas,
     * which is a configuration mistake rather than a runtime condition, but
     * silently dropping the line with no signal is how it goes unnoticed.
     */
    if (y + OSD_FONT_HEIGHT > overlay->height) {
        overlay->stats.lines_clipped++;
        return false;
    }

    for (i = 0; text[i] != '\0'; i++) {
        const struct osd_glyph *glyph;
        unsigned col;

        if (i >= (size_t)OSD_MAX_LINE_CHARS) {
            overlay->stats.chars_clipped++;
            break;
        }

        if (x + OSD_FONT_WIDTH > overlay->width) {
            /* Ran off the right edge: stop, but the line still advanced. */
            overlay->stats.chars_clipped++;
            break;
        }

        /*
         * A character the font does not have draws no ink, but still
         * advances. Leaving the gap keeps the rest of the line aligned, which
         * is what a reader wants when one symbol in a value is unsupported.
         * In knockout mode the cell is still filled, so a missing glyph shows
         * as a solid block rather than as a hole - visibly wrong, which is
         * better than invisibly wrong.
         */
        glyph = osd_font_lookup(text[i]);
        if (glyph != NULL) {
            draw_glyph(overlay, glyph, x, y, knockout);
        } else if (knockout) {
            for (col = 0; col < OSD_FONT_WIDTH; col++) {
                unsigned row;

                for (row = 0; row < OSD_FONT_HEIGHT; row++) {
                    set_pixel(overlay, x + col, y + row, true);
                }
            }
        }

        x += (size_t)OSD_FONT_ADVANCE;

        /*
         * Knockout needs the inter-glyph column set too, or the panel is
         * sliced by one-pixel gaps that let the camera image through in a
         * vertical stripe pattern. The advance is 6 for a 5 wide glyph, so
         * that last column is always the separator.
         */
        if (knockout && x <= overlay->width) {
            unsigned row;

            for (row = 0; row < OSD_FONT_HEIGHT; row++) {
                set_pixel(overlay, x - 1U, y + row, true);
            }
        }
    }

    overlay->cursor_x = 0U;
    overlay->cursor_y = y + (size_t)OSD_FONT_LINE_STEP;
    overlay->stats.lines_drawn++;

    return true;
}

bool osd_overlay_puts(struct osd_overlay *overlay, const char *text)
{
    return puts_impl(overlay, text, false);
}

bool osd_overlay_puts_knockout(struct osd_overlay *overlay, const char *text)
{
    return puts_impl(overlay, text, true);
}

size_t osd_overlay_text_width(const char *text)
{
    return osd_font_text_width(text);
}

size_t osd_overlay_lines_height(size_t lines)
{
    if (lines == 0U) {
        return 0U;
    }

    /*
     * n lines occupy n-1 full line steps plus one glyph height: the step
     * includes the gap, but there is no gap after the last line.
     */
    return (lines - 1U) * (size_t)OSD_FONT_LINE_STEP +
           (size_t)OSD_FONT_HEIGHT;
}

void osd_overlay_fill_rect(struct osd_overlay *overlay,
                           size_t x, size_t y, size_t width, size_t height,
                           bool on)
{
    size_t row;
    size_t col;

    if (overlay == NULL || width == 0U || height == 0U) {
        return;
    }

    /*
     * Clip the rectangle to the canvas once, then draw it without per-pixel
     * range checks. A panel that is larger than the canvas is a layout bug
     * worth surviving rather than worth refusing.
     */
    if (x >= overlay->width || y >= overlay->height) {
        return;
    }
    if (width > overlay->width - x) {
        width = overlay->width - x;
    }
    if (height > overlay->height - y) {
        height = overlay->height - y;
    }

    for (row = 0; row < height; row++) {
        for (col = 0; col < width; col++) {
            set_pixel(overlay, x + col, y + row, on);
        }
    }
}

void osd_overlay_draw_bar(struct osd_overlay *overlay,
                          size_t x, size_t y, size_t width, unsigned num,
                          unsigned den)
{
    size_t filled;

    if (overlay == NULL || width == 0U || den == 0U) {
        return;
    }

    if (num > den) {
        num = den;
    }

    /*
     * Rounded to nearest rather than truncated. A bar that only lights up
     * once a whole cell's worth of value has accumulated reads as zero for
     * the bottom few percent, which on a level indicator looks like the
     * sensor is dead when it is merely small.
     */
    filled = ((size_t)num * width + den / 2U) / den;

    osd_overlay_fill_rect(overlay, x, y, filled, 1U, true);
}

size_t osd_overlay_composite_nv12(struct osd_overlay *overlay,
                                  uint8_t *frame,
                                  size_t frame_width,
                                  size_t frame_height,
                                  size_t origin_x,
                                  size_t origin_y,
                                  uint8_t channel_on)
{
    size_t out_width;
    size_t out_height;
    size_t wrote = 0U;
    size_t sy;

    if (overlay == NULL || frame == NULL) {
        return 0U;
    }

    out_width = overlay->width * overlay->scale;
    out_height = overlay->height * overlay->scale;

    /*
     * Refuse rather than clip. A frame too small for the overlay means the
     * caller has mismatched the overlay to the stream, and a partially
     * written overlay on every frame would be a lot harder to diagnose than
     * an overlay that simply does not appear.
     */
    if (origin_x + out_width > frame_width ||
        origin_y + out_height > frame_height) {
        return 0U;
    }

    for (sy = 0; sy < overlay->height; sy++) {
        const uint8_t *row = &overlay->canvas[sy * overlay->row_bytes];
        size_t sx;

        for (sx = 0; sx < overlay->width; sx++) {
            uint8_t pixel = (uint8_t)((row[sx >> 3] >> (7U - (sx & 7U))) & 1U);
            size_t dy;

            /*
             * Only set pixels write. A clear canvas pixel is transparent and
             * leaves the frame alone.
             *
             * The alternative - writing channel_off for every clear pixel -
             * is worse than it looks. It would paint a solid rectangle over
             * a region the caller may never have meant to cover, so the
             * overlay would show as a dark box even with no text drawn yet,
             * and the only way to get a tight panel around the text would be
             * for the canvas to be exactly the size of the text. Skipping is
             * also strictly less work per frame, which matters because this
             * runs thirty times a second on a single core.
             *
             * The visible panel is therefore the caller's job, drawn with
             * osd_overlay_fill_rect before the text. That is the right split:
             * only the caller knows how big a panel it wants.
             */
            if (pixel == 0U) {
                continue;
            }

            /*
             * Nearest-neighbour upscale: replicate the canvas pixel into a
             * scale x scale block. Integer scale only, see the header - a
             * fractional one would resample the 1px strokes into grey and
             * lose the crispness that is the whole point of a bitmap font.
             */
            for (dy = 0; dy < overlay->scale; dy++) {
                uint8_t *dst_row =
                    frame + (origin_y + sy * overlay->scale + dy) * frame_width;
                size_t dx;

                for (dx = 0; dx < overlay->scale; dx++) {
                    dst_row[origin_x + sx * overlay->scale + dx] = channel_on;
                }
            }

            wrote++;
        }
    }

    overlay->stats.composites++;
    return wrote;
}

const uint8_t *osd_overlay_canvas(const struct osd_overlay *overlay,
                                  size_t *width, size_t *height)
{
    if (overlay == NULL) {
        return NULL;
    }

    if (width != NULL) {
        *width = overlay->width;
    }
    if (height != NULL) {
        *height = overlay->height;
    }

    return overlay->canvas;
}

bool osd_overlay_pixel(const struct osd_overlay *overlay, size_t x, size_t y)
{
    uint8_t byte;

    if (overlay == NULL || x >= overlay->width || y >= overlay->height) {
        return false;
    }

    byte = overlay->canvas[y * overlay->row_bytes + (x >> 3)];
    return ((byte >> (7U - (x & 7U))) & 1U) != 0U;
}

void osd_overlay_stats(const struct osd_overlay *overlay,
                       struct osd_overlay_stats *stats)
{
    if (overlay == NULL || stats == NULL) {
        return;
    }

    *stats = overlay->stats;
}
