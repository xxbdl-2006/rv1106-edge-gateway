#ifndef OSD_FONT_H
#define OSD_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A 5x7 bitmap font, embedded in the binary.
 *
 * Why hand-embedded rather than a font file or a TTF library: the rootfs is
 * a stripped Buildroot image. There is no fontconfig, no freetype, and the
 * program has to keep working if somebody deletes everything under
 * /usr/share. A 5x7 cell is also the right size for this job - at 1280x720 a
 * two-or-three line overlay has plenty of room, and at 5x7 with 1px padding a
 * line is 8px tall, so a 720 line frame fits 90 lines. Nothing here needs
 * antialiasing or hinting because the glyphs are drawn at exactly their
 * design size, one bitmap pixel per screen pixel; scaling is a separate
 * concern handled by the caller repeating pixels.
 *
 * Why 5x7 specifically: it is the classic terminal/teletext cell. Every
 * character that matters for a status overlay is legible at this size, and
 * the table stays small enough to read and check by eye - which matters more
 * than density for a font nobody will ever look at except in a debug overlay.
 *
 * ENCODING. Each glyph is 5 columns x 7 rows. A column is stored as a byte,
 * bit 0 is the TOP row and bit 6 is the bottom row. Columns are stored
 * left-to-right, so glyph[0] is the leftmost column. Only bits 0..6 are used;
 * bit 7 is always zero, which leaves room for an underline flag later without
 * changing the table layout.
 *
 * That is "column-major, LSB at top". It is not the most common layout - most
 * published 5x7 tables are row-major - but it is the one that makes the
 * drawing inner loop a shift of consecutive bytes rather than a gather across
 * the table, and the drawing loop runs once per pixel per frame. The cost is
 * that transcribing a glyph from a row-major reference means transposing it,
 * which is worth knowing when adding characters: it is the single easiest
 * thing to get wrong here. The unit test transcribes a few glyphs back to
 * row-major and compares against the expected shape precisely so a
 * transposed entry fails loudly instead of rendering as mirrored garbage.
 */

#define OSD_FONT_WIDTH 5
#define OSD_FONT_HEIGHT 7
#define OSD_FONT_ADVANCE 6
#define OSD_FONT_LINE_STEP 9

/*
 * A glyph as it is stored: five column bytes, top bit of each is row 0.
 */
struct osd_glyph {
    uint8_t columns[OSD_FONT_WIDTH];
};

/*
 * Look up one character.
 *
 * Returns NULL for anything not in the table. The caller decides what to do
 * about it - the renderer substitutes a blank or a box - because "unknown
 * character" is a normal thing to hit when telemetry contains a unit symbol
 * this font never learned, and silently printing nothing is worse than
 * printing something visibly placeholder.
 *
 * Lowercase input is accepted and mapped to the uppercase glyph: a 5x7 cell
 * has no room for real descenders or x-height distinction, and pretending
 * otherwise produces mush. The justification is the same one terminal fonts
 * use.
 */
const struct osd_glyph *osd_font_lookup(char c);

/*
 * Width in pixels of a string drawn at 1x, including the 1px inter-glyph
 * spacing but NOT a trailing space after the last glyph. Returns 0 for NULL
 * or empty. Characters outside the font contribute their advance so a line's
 * measured width stays stable as values change - a width that jumped when a
 * glyph went missing would make the background box twitch.
 */
size_t osd_font_text_width(const char *text);

/* Height in pixels of a single line at 1x. */
size_t osd_font_line_height(void);

#endif /* OSD_FONT_H */
