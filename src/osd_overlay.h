#ifndef OSD_OVERLAY_H
#define OSD_OVERLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A 1 bit per pixel canvas, and the code that burns it into an NV12 frame.
 *
 * WHY 1BPP. The overlay is text in two colours: the glyph, and the panel
 * behind it. Two colours is one bit. Storing it as 1bpp means an 8x scale
 * overlay covering a quarter of a 720p frame is about 15 KB, so the whole
 * thing stays in cache and the per-frame cost is a memcpy-shaped loop over
 * pixels that are mostly skipped. An 8bpp canvas would be 8x the memory and
 * 8x the bandwidth to say exactly the same thing.
 *
 * WHY NOT DRAW STRAIGHT INTO THE FRAME. Two reasons. The frame is NV12, so
 * drawing into it means writing luma and chroma in the right ratio and
 * getting the UV plane right at odd coordinates - and the same overlay would
 * then have to be drawn twice for two sinks with different frames. Building
 * the canvas once and compositing it per frame separates "what the overlay
 * says" from "which frame it lands on", which is also what makes the text
 * layer testable without a frame at all.
 *
 * WHY THE BACKGROUND PANEL IS NOT OPTIONAL. A camera pointed at a real scene
 * has light and dark regions in it. White text over a bright patch is
 * invisible, and black text over a shadow is invisible. Drawing a solid panel
 * behind the text costs nothing and is the difference between an overlay that
 * works in a lab and one that works on a wall. The panel is drawn dark and
 * the glyphs light, which is the same convention every camera OSD uses and
 * for the same reason: the eye reads bright-on-dark at small sizes better
 * than the reverse.
 *
 * SCALING. Glyphs are 5x7 and on a 720p frame that is unreadably small, so
 * the canvas is drawn at an integer scale and then composited at that scale.
 * Integer only: a fractional scale would require resampling the bitmap and
 * would turn 1px strokes into blurry grey, which is the one thing a 1bpp
 * overlay exists to avoid.
 */

/*
 * Longest overlay line we will draw, and the most lines we will draw.
 *
 * These are capacities, not preferences: they size the fixed buffers inside
 * the overlay so nothing allocates after open. Both are generous relative to
 * what the telemetry layer actually emits (which is a handful of short lines)
 * because the cost of headroom here is a few hundred bytes and the cost of a
 * clipped line is a bug report about missing data.
 */
#define OSD_MAX_LINE_CHARS 64
#define OSD_MAX_LINES 12

struct osd_overlay;

struct osd_overlay_config {
    /*
     * Overlay size in pixels at 1x, before scaling. The canvas is allocated
     * once at this size and never grows; text that does not fit is clipped
     * rather than reallocating, because this runs per frame.
     */
    size_t width;
    size_t height;
    /* Integer upscale factor applied at composite time. 1 to disable. */
    unsigned scale;
};

struct osd_overlay_stats {
    /* Frames composited since open. */
    unsigned long composites;
    /* Lines drawn, and lines refused because there was no room left. */
    unsigned long lines_drawn;
    unsigned long lines_clipped;
    /* Characters refused because the line was longer than OSD_MAX_LINE_CHARS. */
    unsigned long chars_clipped;
};

/*
 * Create a canvas. Allocates once; every later call is allocation free.
 *
 * scale 0 is treated as 1. A width or height of 0 uses the defaults.
 */
int osd_overlay_open(const struct osd_overlay_config *config,
                     struct osd_overlay **overlay);

void osd_overlay_destroy(struct osd_overlay *overlay);

/* Clear the canvas and reset the cursor to the top left. */
void osd_overlay_reset(struct osd_overlay *overlay);

/*
 * Set where the next osd_overlay_puts() lands, in canvas pixels at 1x.
 * Out of range coordinates are clamped, so a layout that overflows the canvas
 * draws partially rather than corrupting memory.
 */
void osd_overlay_move_to(struct osd_overlay *overlay, size_t x, size_t y);

/*
 * Draw one line of text at the cursor and advance to the next line.
 *
 * Returns true if the whole line was drawn, false if it was clipped - either
 * because the cursor had run past the bottom of the canvas or because the
 * string was longer than OSD_MAX_LINE_CHARS. A false return is not an error;
 * the caller is expected to keep going and the statistics record it.
 */
bool osd_overlay_puts(struct osd_overlay *overlay, const char *text);

/*
 * Draw one line with the glyphs knocked out of the line box instead of drawn
 * onto it: every pixel of the text cell is set except the glyph itself.
 *
 * This is how the overlay gets dark text on a light panel without a second
 * canvas. A 1bpp canvas has two states, set and clear, and compositing gives
 * them two meanings: paint white, or leave the frame alone. To show dark text
 * on a white panel the text has to be the CLEAR pixels inside a SET box - so
 * the box is set, the glyph pixels are cleared, and the frame shows through
 * the letter shapes.
 *
 * Which mode to use is a question of what is behind the text. Knockout is the
 * right default over live video: it gives a solid panel with readable letters
 * and no dependence on the scene being dark. Plain puts() is for drawing
 * white text directly with no panel.
 *
 * With a light panel (high channel_on) knockout gives dark letters. With a
 * dark panel it gives light letters, which is just puts() inside a panel.
 */
bool osd_overlay_puts_knockout(struct osd_overlay *overlay, const char *text);

/* Width in canvas pixels of one line at 1x, for laying out a panel around it. */
size_t osd_overlay_text_width(const char *text);

/* Height in canvas pixels of `lines` lines at 1x, including the gaps. */
size_t osd_overlay_lines_height(size_t lines);

/*
 * Fill a rectangle, in canvas pixels at 1x. Used for the background panel
 * and for the alarm bar. Clipped to the canvas rather than refused.
 */
void osd_overlay_fill_rect(struct osd_overlay *overlay,
                           size_t x, size_t y, size_t width, size_t height,
                           bool on);

/* Draw a horizontal bar whose filled fraction is num/den, for the level and
 * alarm indicators. den == 0 draws nothing. */
void osd_overlay_draw_bar(struct osd_overlay *overlay,
                          size_t x, size_t y, size_t width, unsigned num,
                          unsigned den);

/*
 * Composite the canvas into the luma plane of a tightly packed NV12 frame.
 *
 * The frame comes from the V4L2 capture path, which repacks rows with no
 * stride padding, so a pixel is at (y * frame_width + x). Only the Y plane is
 * touched: the overlay is monochrome, and leaving UV alone keeps the text
 * neutral grey rather than picking up the colour of whatever it covers.
 *
 * TRANSPARENCY. Only set canvas pixels are written. A clear pixel leaves the
 * frame untouched, so the overlay regions the caller never drew into show the
 * camera image through. To get the opaque panel that makes text readable over
 * a bright scene, draw it into the canvas first with osd_overlay_fill_rect -
 * only the caller knows how large a panel it wants.
 *
 * channel_on is the luma value written for a set canvas pixel. Pass e.g. 235
 * (video range white) or 255 (full range). There is deliberately no
 * channel_off: painting the frame's dark value everywhere would cover a
 * rectangle even where nothing was drawn, which is exactly the behaviour the
 * transparency rule exists to prevent.
 *
 * Returns the number of canvas pixels written, or 0 if the arguments do not
 * make sense - in particular if the frame is too small to hold the overlay,
 * in which case nothing is written rather than a partial corruption.
 */
size_t osd_overlay_composite_nv12(struct osd_overlay *overlay,
                                  uint8_t *frame,
                                  size_t frame_width,
                                  size_t frame_height,
                                  size_t origin_x,
                                  size_t origin_y,
                                  uint8_t channel_on);

/*
 * The canvas, for tests and for the RGA path which wants the raw bits.
 *
 * Rows are packed 1bpp, MSB first, and each row starts on a byte boundary.
 * Returns NULL if the overlay is NULL.
 */
const uint8_t *osd_overlay_canvas(const struct osd_overlay *overlay,
                                  size_t *width, size_t *height);

/* True if the canvas pixel at (x, y) is set. Out of range returns false. */
bool osd_overlay_pixel(const struct osd_overlay *overlay, size_t x, size_t y);

void osd_overlay_stats(const struct osd_overlay *overlay,
                       struct osd_overlay_stats *stats);

#endif /* OSD_OVERLAY_H */
