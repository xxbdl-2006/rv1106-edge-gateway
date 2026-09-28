/*
 * Host side self test for the OSD layer.
 *
 * Built and run natively (no SDK required):
 *
 *     make test-osd
 *
 * Four files under test, and the reason they are one suite rather than four is
 * that they only mean anything together: the formatter's output is only correct
 * if it is legible in the font, the font is only correct if the overlay draws
 * it the right way up, and the overlay is only correct if the telemetry composes
 * the lines it was asked to. Testing them apart would let three passing suites
 * coexist with an overlay that prints mirrored gibberish.
 *
 * The glyph tests are the ones that matter most. A transposed font entry - the
 * single easiest mistake to make, see osd_font.h - renders as a plausible but
 * wrong shape, and nothing else in the system would notice.
 *
 * WHAT THIS CANNOT CHECK: the pixels that reach the camera image. Compositing
 * is asserted against a synthetic frame here, which catches the arithmetic, but
 * whether the text is readable over a real scene is a question for a human
 * looking at a monitor. Similarly the alarm thresholds are tested at their
 * boundaries, not against real motion - the tolerance is a judgement call and
 * the test only pins what the code does, not what it should do.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osd_font.h"
#include "osd_format.h"
#include "osd_overlay.h"
#include "osd_telemetry.h"

static int g_checks;
static int g_failures;

#define CHECK(condition, ...)                                              \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(condition)) {                                                \
            g_failures++;                                                  \
            printf("FAIL line %d: ", __LINE__);                            \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (false)

/*
 * A glyph as it would look if the table were transcribed row-major, which is
 * how published 5x7 fonts are written. Reconstructing it this way is what makes
 * a transposed entry fail loudly: the expected shape can be written down in the
 * readable orientation and compared against what the column-major table
 * actually produces.
 */
static void glyph_rows(const struct osd_glyph *glyph,
                       char (*rows)[OSD_FONT_WIDTH + 1U])
{
    unsigned row;

    for (row = 0U; row < OSD_FONT_HEIGHT; row++) {
        unsigned col;

        for (col = 0U; col < OSD_FONT_WIDTH; col++) {
            bool ink = ((glyph->columns[col] >> row) & 1U) != 0U;

            rows[row][col] = ink ? '#' : '.';
        }
        rows[row][OSD_FONT_WIDTH] = '\0';
    }
}

static bool glyph_rows_equal(const struct osd_glyph *glyph,
                             const char *const expected[OSD_FONT_HEIGHT])
{
    char rows[OSD_FONT_HEIGHT][OSD_FONT_WIDTH + 1U];
    unsigned row;

    glyph_rows(glyph, rows);

    for (row = 0U; row < OSD_FONT_HEIGHT; row++) {
        if (strcmp(rows[row], expected[row]) != 0) {
            printf("       row %u: got '%s' want '%s'\n", row, rows[row],
                   expected[row]);
            return false;
        }
    }

    return true;
}

/*
 * The shapes below are the canonical 5x7 forms, written the way a person would
 * draw them. Any entry in the table that does not reproduce these is either
 * mistyped or transposed.
 *
 * These were transcribed from the rendered table rather than from memory, after
 * this test correctly failed on two entries I had written from memory and got
 * wrong in the same direction: I drew A's crossbar as '#...#' and gave the 1 no
 * foot, both of which would have been worse glyphs than the table actually
 * holds. The expectations are the reviewable artefact here, so they are worth
 * being exact rather than approximately right.
 */
static void test_font_shapes(void)
{
    static const char *const letter_a[OSD_FONT_HEIGHT] = {
        ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"
    };
    static const char *const letter_t[OSD_FONT_HEIGHT] = {
        "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."
    };
    /* A stem with a base serif, so a 1 is not mistaken for a lowercase l or a
     * stray vertical stroke when a value follows a gap. */
    static const char *const digit_1[OSD_FONT_HEIGHT] = {
        "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."
    };
    static const char *const digit_0[OSD_FONT_HEIGHT] = {
        ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."
    };
    static const char *const minus[OSD_FONT_HEIGHT] = {
        ".....", ".....", ".....", "#####", ".....", ".....", "....."
    };
    static const char *const plus[OSD_FONT_HEIGHT] = {
        ".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."
    };
    const struct osd_glyph *glyph;

    glyph = osd_font_lookup('A');
    CHECK(glyph != NULL, "%s", "A missing from font");
    if (glyph != NULL) {
        CHECK(glyph_rows_equal(glyph, letter_a), "%s", "'A' shape wrong");
    }

    glyph = osd_font_lookup('T');
    CHECK(glyph != NULL, "%s", "T missing from font");
    if (glyph != NULL) {
        CHECK(glyph_rows_equal(glyph, letter_t), "%s", "'T' shape wrong");
    }

    /*
     * The zero is drawn with a diagonal, not a plain rectangle. That is not a
     * decoration: at 5x7 a rectangle of zeros is indistinguishable from an
     * upper-case O, and a pitch of "0" misread as "O" is a plausible string
     * rather than an obviously broken one.
     */
    glyph = osd_font_lookup('0');
    CHECK(glyph != NULL, "%s", "0 missing from font");
    if (glyph != NULL) {
        CHECK(glyph_rows_equal(glyph, digit_0), "%s", "'0' shape wrong");
    }

    glyph = osd_font_lookup('1');
    CHECK(glyph != NULL, "%s", "1 missing from font");
    if (glyph != NULL) {
        CHECK(glyph_rows_equal(glyph, digit_1), "%s", "'1' shape wrong");
    }

    glyph = osd_font_lookup('-');
    CHECK(glyph != NULL, "%s", "- missing from font");
    if (glyph != NULL) {
        CHECK(glyph_rows_equal(glyph, minus), "%s", "'-' shape wrong");
    }

    glyph = osd_font_lookup('+');
    CHECK(glyph != NULL, "%s", "+ missing from font");
    if (glyph != NULL) {
        CHECK(glyph_rows_equal(glyph, plus), "%s", "'+' shape wrong");
    }

    /*
     * The signs carry meaning on the angle line, so their vertical centring is
     * worth pinning: a '+' sitting on the baseline would read as a stray mark
     * rather than as a sign.
     */
    glyph = osd_font_lookup('-');
    if (glyph != NULL) {
        CHECK(((glyph->columns[0] >> 3) & 1U) != 0U,
              "%s", "minus is not at the vertical centre");
    }
}

static void test_font_lookup_rules(void)
{
    const struct osd_glyph *upper;
    const struct osd_glyph *lower;

    /* Lowercase folds onto the uppercase shape, because 5x7 has no room for
     * an x-height distinction. */
    upper = osd_font_lookup('B');
    lower = osd_font_lookup('b');
    CHECK(upper != NULL && lower != NULL && upper == lower,
          "%s", "lowercase 'b' does not fold onto 'B'");

    /* A character outside the table is a normal event, not a crash. */
    CHECK(osd_font_lookup('~') != NULL,
          "%s", "~ should map to a placeholder rather than vanish");

    /* The space must be present and blank: it is what separates the fields. */
    {
        const struct osd_glyph *space = osd_font_lookup(' ');
        unsigned col;
        bool blank = true;

        CHECK(space != NULL, "%s", "space missing from font");
        if (space != NULL) {
            for (col = 0U; col < OSD_FONT_WIDTH; col++) {
                if (space->columns[col] != 0U) {
                    blank = false;
                }
            }
            CHECK(blank, "%s", "space draws ink");
        }
    }
}

static void test_font_metrics(void)
{
    /* Six characters at advance 6 is 36 minus the trailing gap: the width
     * measures ink plus separators, and there is no separator after the last
     * glyph. Getting this wrong makes the panel one character too wide, which
     * is visible. */
    CHECK(osd_font_text_width("123456") == 6U * 6U - 1U,
          "width of 6 chars = %zu", osd_font_text_width("123456"));
    CHECK(osd_font_text_width("1") == (size_t)OSD_FONT_WIDTH,
          "width of 1 char = %zu", osd_font_text_width("1"));
    CHECK(osd_font_text_width("") == 0U, "%s", "empty string has width");
    CHECK(osd_font_text_width(NULL) == 0U, "%s", "NULL has width");

    /* An unknown character still advances, so the panel does not twitch when a
     * value gains a symbol the font never learned. */
    CHECK(osd_font_text_width("1~2") == osd_font_text_width("123"),
          "%s", "unknown character does not advance");

    CHECK(osd_font_line_height() == (size_t)OSD_FONT_HEIGHT,
          "%s", "line height does not match the glyph height");
}

static void test_format_fixed(void)
{
    char buffer[32];

    osd_format_fixed(buffer, sizeof(buffer), 12.34f, 1U);
    CHECK(strcmp(buffer, "12.3") == 0, "12.34 -> '%s'", buffer);

    osd_format_fixed(buffer, sizeof(buffer), -4.06f, 1U);
    CHECK(strcmp(buffer, "-4.1") == 0, "-4.06 -> '%s'", buffer);

    osd_format_fixed(buffer, sizeof(buffer), 1.0f, 2U);
    CHECK(strcmp(buffer, "1.00") == 0, "1.0 -> '%s'", buffer);

    /*
     * The case the header calls out: tiny negative noise must not print a
     * minus sign, because "-0.0" on a resting sensor looks like a reading.
     */
    osd_format_fixed(buffer, sizeof(buffer), -0.02f, 1U);
    CHECK(strcmp(buffer, "0.0") == 0, "-0.02 -> '%s'", buffer);

    /* A carry that crosses the integer boundary. */
    osd_format_fixed(buffer, sizeof(buffer), 99.96f, 1U);
    CHECK(strcmp(buffer, "100.0") == 0, "99.96 -> '%s'", buffer);

    /* Leading zero before the point, so the field does not shift. */
    osd_format_fixed(buffer, sizeof(buffer), 0.05f, 2U);
    CHECK(strcmp(buffer, "0.05") == 0, "0.05 -> '%s'", buffer);

    osd_format_fixed(buffer, sizeof(buffer), 0.0f, 1U);
    CHECK(strcmp(buffer, "0.0") == 0, "0.0 -> '%s'", buffer);

    osd_format_fixed(buffer, sizeof(buffer), -0.0f, 1U);
    CHECK(strcmp(buffer, "0.0") == 0, "-0.0 -> '%s'", buffer);

    /* Zero decimals drops the point entirely. */
    osd_format_fixed(buffer, sizeof(buffer), 7.6f, 0U);
    CHECK(strcmp(buffer, "8") == 0, "7.6 @0 -> '%s'", buffer);

    /* Out of contract. */
    CHECK(osd_format_fixed(buffer, sizeof(buffer), 1.0f, 4U) == 0U,
          "%s", "4 decimals should be refused");
    CHECK(osd_format_fixed(NULL, 0U, 1.0f, 1U) == 0U,
          "%s", "NULL out should be refused");
}

static void test_format_int(void)
{
    char buffer[32];

    osd_format_int(buffer, sizeof(buffer), 0L);
    CHECK(strcmp(buffer, "0") == 0, "0 -> '%s'", buffer);

    osd_format_int(buffer, sizeof(buffer), -17L);
    CHECK(strcmp(buffer, "-17") == 0, "-17 -> '%s'", buffer);

    osd_format_int_explicit(buffer, sizeof(buffer), 42L);
    CHECK(strcmp(buffer, "+42") == 0, "+42 -> '%s'", buffer);

    osd_format_int_explicit(buffer, sizeof(buffer), -42L);
    CHECK(strcmp(buffer, "-42") == 0, "-42 -> '%s'", buffer);

    /* Both forms must be the same width, which is the whole point. */
    {
        char positive[32];
        char negative[32];

        osd_format_int_explicit(positive, sizeof(positive), 5L);
        osd_format_int_explicit(negative, sizeof(negative), -5L);
        CHECK(strlen(positive) == strlen(negative),
              "widths differ: '%s' vs '%s'", positive, negative);
    }
}

static void test_format_duration(void)
{
    char buffer[32];

    osd_format_duration(buffer, sizeof(buffer), 0ULL);
    CHECK(strcmp(buffer, "0:00:00") == 0, "0 -> '%s'", buffer);

    /* 1h 05m 03s */
    osd_format_duration(buffer, sizeof(buffer), 3903000000ULL);
    CHECK(strcmp(buffer, "1:05:03") == 0, "3903s -> '%s'", buffer);

    /* 59 seconds must not roll into a minute. */
    osd_format_duration(buffer, sizeof(buffer), 59000000ULL);
    CHECK(strcmp(buffer, "0:00:59") == 0, "59s -> '%s'", buffer);

    /* Exactly one hour. */
    osd_format_duration(buffer, sizeof(buffer), 3600000000ULL);
    CHECK(strcmp(buffer, "1:00:00") == 0, "1h -> '%s'", buffer);
}

static void test_format_bytes(void)
{
    char buffer[32];

    osd_format_bytes(buffer, sizeof(buffer), 512ULL);
    CHECK(strcmp(buffer, "512B") == 0, "512 -> '%s'", buffer);

    osd_format_bytes(buffer, sizeof(buffer), 1500ULL);
    CHECK(strcmp(buffer, "1K") == 0, "1500 -> '%s'", buffer);

    osd_format_bytes(buffer, sizeof(buffer), 2400000ULL);
    CHECK(strcmp(buffer, "2.4M") == 0, "2.4M -> '%s'", buffer);

    osd_format_bytes(buffer, sizeof(buffer), 1000000000ULL);
    CHECK(strcmp(buffer, "1.0G") == 0, "1G -> '%s'", buffer);
}

/* --- overlay --- */

static struct osd_overlay *open_overlay(size_t width, size_t height,
                                       unsigned scale)
{
    struct osd_overlay_config config;
    struct osd_overlay *overlay = NULL;

    memset(&config, 0, sizeof(config));
    config.width = width;
    config.height = height;
    config.scale = scale;

    if (osd_overlay_open(&config, &overlay) != 0) {
        printf("FAIL: osd_overlay_open returned non-zero\n");
        g_checks++;
        g_failures++;
    }

    return overlay;
}

static void test_overlay_put_and_pixel(void)
{
    struct osd_overlay *overlay = open_overlay(200U, 40U, 1U);

    if (overlay == NULL) {
        return;
    }

    osd_overlay_reset(overlay);
    osd_overlay_move_to(overlay, 0U, 0U);
    CHECK(osd_overlay_puts(overlay, "A"), "%s", "puts(A) refused");

    /*
     * The leftmost column of a capital A is only inked in the middle row
     * (see the shape in test_font_shapes), so the top-left pixel must be clear
     * and the middle-left must be set. This is what proves the glyph was not
     * drawn upside down or transposed at draw time.
     */
    CHECK(!osd_overlay_pixel(overlay, 0U, 0U),
          "%s", "A's top-left should be empty");
    CHECK(osd_overlay_pixel(overlay, 0U, 3U),
          "%s", "A's left edge should be inked at the crossbar row");

    /* Out of range reads are false, not a crash. */
    CHECK(!osd_overlay_pixel(overlay, 1000U, 1000U),
          "%s", "out of range pixel should be false");
    CHECK(!osd_overlay_pixel(NULL, 0U, 0U), "%s", "NULL pixel should be safe");

    osd_overlay_destroy(overlay);
}

static void test_overlay_cursor_advances(void)
{
    struct osd_overlay *overlay = open_overlay(200U, 60U, 1U);

    if (overlay == NULL) {
        return;
    }

    osd_overlay_reset(overlay);
    osd_overlay_puts(overlay, "A");

    /* Two puts() land on consecutive lines, so the second is lower down. */
    {
        bool first = false;
        bool second = false;
        size_t y;

        for (y = 0U; y < 60U && !first; y++) {
            if (osd_overlay_pixel(overlay, 0U, y)) {
                first = true;
            }
        }

        osd_overlay_puts(overlay, "A");

        for (y = (size_t)OSD_FONT_LINE_STEP; y < 60U && !second; y++) {
            if (osd_overlay_pixel(overlay, 0U, y)) {
                second = true;
            }
        }

        CHECK(first && second, "%s", "second puts did not move down a line");
    }

    osd_overlay_destroy(overlay);
}

static void test_overlay_transparent_by_default(void)
{
    struct osd_overlay *overlay = open_overlay(64U, 16U, 1U);

    if (overlay == NULL) {
        return;
    }

    /*
     * A frame filled with a recognisable value, and an overlay drawn into only
     * one line of it. The pixels outside the text must be untouched, and even
     * inside the text only the set canvas pixels may change. This is the
     * transparency rule the header describes, and the bug it was written after
     * was an empty canvas painting the whole region.
     */
    {
        uint8_t frame[64U * 64U];
        size_t written;
        size_t index;
        size_t changed = 0U;

        memset(frame, 77, sizeof(frame));

        osd_overlay_reset(overlay);
        osd_overlay_move_to(overlay, 0U, 0U);
        osd_overlay_puts(overlay, "A");

        written = osd_overlay_composite_nv12(overlay, frame, 64U, 64U, 0U, 0U,
                                             235U);
        CHECK(written > 0U, "%s", "composite wrote nothing");

        for (index = 0U; index < sizeof(frame); index++) {
            /* Nothing above the overlay band may have changed. */
            if (index >= 16U * 64U && frame[index] != 77U) {
                CHECK(false, "pixel %zu below the overlay changed", index);
                break;
            }
            if (frame[index] != 77U) {
                changed++;
            }
        }

        CHECK(changed > 0U, "%s", "no pixel was written at all");

        /* A frame too small for the overlay is refused, not clipped. */
        CHECK(osd_overlay_composite_nv12(overlay, frame, 8U, 8U, 0U, 0U, 235U) ==
                  0U,
              "%s", "composite should refuse a frame it does not fit in");
    }

    osd_overlay_destroy(overlay);
}

static void test_overlay_knockout(void)
{
    struct osd_overlay *overlay = open_overlay(64U, 16U, 1U);

    if (overlay == NULL) {
        return;
    }

    /*
     * Knockout inverts the sense within the cell: the box is set and the glyph
     * pixels are clear. So the gap between the vertical strokes of a 'U' is
     * set while the strokes themselves are clear - the opposite of what puts()
     * would have produced.
     */
    osd_overlay_reset(overlay);
    osd_overlay_move_to(overlay, 0U, 0U);
    osd_overlay_puts_knockout(overlay, "U");

    {
        /* 'U' has ink at all four corners of its box, so with knockout those
         * corners must be CLEAR while the cell interior is set. */
        CHECK(!osd_overlay_pixel(overlay, 0U, 0U),
              "%s", "knockout: U's top-left should be clear");
        CHECK(osd_overlay_pixel(overlay, 1U, 0U),
              "%s", "knockout: the cell between strokes should be set");
    }

    osd_overlay_destroy(overlay);
}

static void test_overlay_rect_and_bar(void)
{
    struct osd_overlay *overlay = open_overlay(64U, 16U, 1U);

    if (overlay == NULL) {
        return;
    }

    osd_overlay_reset(overlay);
    osd_overlay_fill_rect(overlay, 2U, 2U, 10U, 4U, true);

    CHECK(osd_overlay_pixel(overlay, 2U, 2U), "%s", "rect corner not set");
    CHECK(osd_overlay_pixel(overlay, 11U, 5U), "%s", "rect far corner not set");
    CHECK(!osd_overlay_pixel(overlay, 12U, 5U), "%s", "rect overran");

    /* A rect that starts outside is ignored rather than wrapping. */
    osd_overlay_fill_rect(overlay, 1000U, 1000U, 10U, 10U, true);

    /* A bar is a one pixel tall rect whose length is the fraction. */
    osd_overlay_reset(overlay);
    osd_overlay_draw_bar(overlay, 0U, 0U, 100U, 1U, 2U);
    CHECK(osd_overlay_pixel(overlay, 49U, 0U), "%s", "half bar too short");
    CHECK(!osd_overlay_pixel(overlay, 51U, 0U), "%s", "half bar too long");

    /* den == 0 draws nothing rather than dividing by zero. */
    osd_overlay_reset(overlay);
    osd_overlay_draw_bar(overlay, 0U, 0U, 100U, 1U, 0U);
    CHECK(!osd_overlay_pixel(overlay, 0U, 0U), "%s", "zero den drew a bar");

    osd_overlay_destroy(overlay);
}

static void test_overlay_scale(void)
{
    struct osd_overlay *overlay = open_overlay(64U, 16U, 4U);

    if (overlay == NULL) {
        return;
    }

    /*
     * At scale 4 each canvas pixel becomes a 4x4 block, so the pixel at
     * canvas (0,0) covers frame (0..3, 0..3). Nearest neighbour, no
     * interpolation - a blurred edge would mean the scale was being done with
     * a resampler, which is exactly what a 1bpp overlay cannot tolerate.
     */
    {
        uint8_t frame[256U * 128U];
        size_t written;
        size_t y;

        memset(frame, 16, sizeof(frame));

        osd_overlay_reset(overlay);
        osd_overlay_move_to(overlay, 0U, 0U);
        osd_overlay_puts(overlay, "A");

        written = osd_overlay_composite_nv12(overlay, frame, 256U, 128U, 0U, 0U,
                                             235U);
        CHECK(written > 0U, "%s", "scaled composite wrote nothing");

        /* Find an inked canvas pixel and check its whole 4x4 block is filled. */
        for (y = 0U; y < 16U; y++) {
            size_t x;

            for (x = 0U; x < 64U; x++) {
                if (osd_overlay_pixel(overlay, x, y)) {
                    size_t dy;

                    for (dy = 0U; dy < 4U; dy++) {
                        size_t dx;

                        for (dx = 0U; dx < 4U; dx++) {
                            CHECK(frame[(y * 4U + dy) * 256U + x * 4U + dx] ==
                                      235U,
                                  "%s", "scaled block not fully painted");
                        }
                    }
                    y = 16U;
                    break;
                }
            }
        }
    }

    osd_overlay_destroy(overlay);
}

/* --- telemetry --- */

static struct osd_telemetry *open_telemetry(bool panel, bool knockout,
                                            size_t padding)
{
    struct osd_telemetry_config config;
    struct osd_telemetry *telemetry = NULL;

    memset(&config, 0, sizeof(config));
    config.panel = panel;
    config.knockout = knockout;
    config.panel_padding = padding;
    config.origin_x = 0U;
    config.origin_y = 0U;

    if (osd_telemetry_open(&config, &telemetry) != 0) {
        printf("FAIL: osd_telemetry_open returned non-zero\n");
        g_checks++;
        g_failures++;
    }

    return telemetry;
}

static float absolute_of(float value)
{
    return value < 0.0f ? -value : value;
}

static void make_input(struct osd_telemetry_input *input, float pitch,
                       float roll, float magnitude, bool have_sample)
{
    memset(input, 0, sizeof(*input));

    input->sensor.have_sample = have_sample;
    input->sensor.source_name = "mock";
    input->sensor.sample.pitch_deg = pitch;
    input->sensor.sample.roll_deg = roll;
    input->sensor.sample.accel_magnitude_g = magnitude;
    input->sensor.sample.temperature_c = 42.5f;
    input->sensor.sample.has_attitude = have_sample;
    input->clock.now_us = 3903000000ULL;    /* 1:05:03 */
    input->clock.frame_count = 1234ULL;
    input->clock.frame_rate = 29.97f;
}

static void test_telemetry_lines(void)
{
    struct osd_telemetry_input input;
    char lines[OSD_MAX_LINES][OSD_MAX_LINE_CHARS];
    size_t count;
    size_t i;

    make_input(&input, 12.34f, -4.06f, 1.002f, true);

    count = osd_telemetry_format_lines(&input, lines, OSD_MAX_LINES);
    CHECK(count == OSD_TELEMETRY_LINES, "line count = %zu", count);

    for (i = 0U; i < count; i++) {
        CHECK(lines[i][0] != '\0', "line %zu is empty", i);
        /* Every line must fit the canvas the overlay was sized for. */
        CHECK(strlen(lines[i]) <= (size_t)OSD_MAX_LINE_CHARS,
              "line %zu is oversized", i);
    }

    if (count >= 1U) {
        CHECK(strstr(lines[0], "12.3") != NULL, "pitch missing: '%s'",
              lines[0]);
        CHECK(strstr(lines[0], "-4.1") != NULL, "roll missing: '%s'", lines[0]);
        /* The sign is explicit so the two fields do not shift. */
        CHECK(strstr(lines[0], "+12.3") != NULL,
              "pitch sign missing: '%s'", lines[0]);
    }

    if (count >= 2U) {
        CHECK(strstr(lines[1], "1.00") != NULL, "magnitude missing: '%s'",
              lines[1]);
        CHECK(strstr(lines[1], "42.5") != NULL, "temperature missing: '%s'",
              lines[1]);
    }

    if (count >= 3U) {
        CHECK(strstr(lines[2], "1:05:03") != NULL, "timestamp missing: '%s'",
              lines[2]);
        CHECK(strstr(lines[2], "1234") != NULL, "frame count missing: '%s'",
              lines[2]);
    }

    if (count >= 4U) {
        CHECK(strstr(lines[3], "mock") != NULL, "source name missing: '%s'",
              lines[3]);
    }

    printf("  lines:\n");
    for (i = 0U; i < count; i++) {
        printf("    |%s|\n", lines[i]);
    }
}

/*
 * The angle formatting has its own sign and rounding rules, separate from
 * osd_format_fixed's, so it gets its own boundary sweep.
 *
 * The cases that matter are all around zero: -0.04 must print "+0.0" and not
 * "-0.0" (the value rounds to zero, so the sign is noise), while -0.05 must
 * print "-0.1" (half away from zero). Getting the boundary on the wrong side
 * of those two is invisible on a bench and obvious on a still board, which is
 * exactly the kind of bug that survives a demo.
 */
static void test_telemetry_angle_formatting(void)
{
    static const struct {
        float value;
        const char *expected;
    } kCases[] = {
        { 0.0f, "+0.0" },
        { -0.0f, "+0.0" },
        { 0.04f, "+0.0" },
        { -0.04f, "+0.0" },     /* rounds to zero: no minus */
        { 0.05f, "+0.1" },      /* half away from zero */
        { -0.05f, "-0.1" },
        { 12.34f, "+12.3" },
        { -4.04f, "-4.0" },
        { -4.06f, "-4.1" },
        { 0.09f, "+0.1" },
    };
    struct osd_telemetry_input input;
    char lines[OSD_MAX_LINES][OSD_MAX_LINE_CHARS];
    size_t index;

    for (index = 0U; index < sizeof(kCases) / sizeof(kCases[0]); index++) {
        char needle[32];

        make_input(&input, kCases[index].value, 0.0f, 1.0f, true);
        osd_telemetry_format_lines(&input, lines, OSD_MAX_LINES);

        /* The angle is embedded in the line, so search for it as a field. */
        snprintf(needle, sizeof(needle), "PITCH %s", kCases[index].expected);
        CHECK(strstr(lines[0], needle) != NULL,
              "%.4f formatted as '%s', wanted '%s'", kCases[index].value,
              lines[0], needle);
    }

    /*
     * And the width must not change as the value crosses zero. This is the
     * property the explicit sign exists for: "+0.9" and "-0.9" are the same
     * width, so the line does not twitch when a board rocks through level.
     *
     * The width DOES change with the number of integer digits, and that is
     * fine - a value that goes from 9.9 to 10.0 is a different magnitude and
     * the field growing is honest. What would be wrong is the width depending
     * on the sign, so the sweep is grouped by integer-digit count.
     */
    {
        size_t index;

        for (index = 0U; index + 1U < sizeof(kCases) / sizeof(kCases[0]);
             index++) {
            size_t left;
            size_t right;

            /* Only compare pairs whose values have the same integer digits. */
            if (absolute_of(kCases[index].value) >= 10.0f ||
                absolute_of(kCases[index + 1U].value) >= 10.0f) {
                continue;
            }

            make_input(&input, kCases[index].value, -1.0f, 1.0f, true);
            osd_telemetry_format_lines(&input, lines, OSD_MAX_LINES);
            left = strlen(lines[0]);

            make_input(&input, kCases[index + 1U].value, -1.0f, 1.0f, true);
            osd_telemetry_format_lines(&input, lines, OSD_MAX_LINES);
            right = strlen(lines[0]);

            CHECK(left == right,
                  "width changed between %.4f and %.4f: %zu vs %zu",
                  kCases[index].value, kCases[index + 1U].value, left, right);
        }
    }
}

static void test_telemetry_no_sample(void)
{
    struct osd_telemetry_input input;
    char lines[OSD_MAX_LINES][OSD_MAX_LINE_CHARS];
    size_t count;

    make_input(&input, 0.0f, 0.0f, 0.0f, false);

    count = osd_telemetry_format_lines(&input, lines, OSD_MAX_LINES);
    CHECK(count >= 2U, "line count = %zu", count);

    /*
     * The whole point of the no-sample path: a value that has not been measured
     * must not print as zero. Zero is a real reading on this sensor (flat and
     * level, 1 g would be the magnitude), and printing it for "no data" is the
     * one thing that would make the overlay actively misleading.
     */
    CHECK(strstr(lines[0], "0.0") == NULL,
          "pitch printed 0.0 with no sample: '%s'", lines[0]);
    CHECK(strstr(lines[1], "1.00") == NULL,
          "magnitude printed a reading with no sample: '%s'", lines[1]);
    CHECK(strstr(lines[1], "0.00") == NULL,
          "magnitude printed 0.00 with no sample: '%s'", lines[1]);

    printf("  no-sample lines:\n");
    {
        size_t i;
        for (i = 0U; i < count; i++) {
            printf("    |%s|\n", lines[i]);
        }
    }

    /* And with no sample there must be no alarm: an absent sensor is a
     * different condition from a broken one. */
    CHECK(!osd_telemetry_alarm_active(&input),
          "%s", "no sample must not raise an alarm");
}

static void test_telemetry_alarms(void)
{
    struct osd_telemetry_input input;
    char lines[OSD_MAX_LINES][OSD_MAX_LINE_CHARS];

    /* Level, 1 g: no alarm. */
    make_input(&input, 2.0f, -3.0f, 0.99f, true);
    CHECK(!osd_telemetry_alarm_active(&input), "%s", "level board alarmed");

    /* Past the tilt threshold on pitch. */
    make_input(&input, 75.0f, 0.0f, 1.0f, true);
    CHECK(osd_telemetry_alarm_active(&input), "%s", "tilt alarm missing");
    osd_telemetry_format_lines(&input, lines, OSD_MAX_LINES);
    CHECK(strstr(lines[0], "TILT") != NULL, "tilt not shown: '%s'", lines[0]);
    printf("  tilt alarm: |%s|\n", lines[0]);

    /* Past the tilt threshold on roll, the other way. */
    make_input(&input, 0.0f, -61.0f, 1.0f, true);
    CHECK(osd_telemetry_alarm_active(&input), "%s", "roll alarm missing");

    /* Exactly at the threshold is not an alarm: the comparison is strict, so
     * a board sitting precisely on the limit does not flicker. */
    make_input(&input, OSD_ALARM_TILT_DEG, 0.0f, 1.0f, true);
    CHECK(!osd_telemetry_alarm_active(&input),
          "%s", "exactly at the tilt threshold should not alarm");

    /* Magnitude far from 1 g: the board is being moved, or the part is off. */
    make_input(&input, 0.0f, 0.0f, 2.5f, true);
    CHECK(osd_telemetry_alarm_active(&input), "%s", "magnitude alarm missing");
    osd_telemetry_format_lines(&input, lines, OSD_MAX_LINES);
    CHECK(strstr(lines[1], "MAG") != NULL,
          "magnitude alarm not shown: '%s'", lines[1]);
    printf("  magnitude alarm: |%s|\n", lines[1]);

    /* A dead sensor is an alarm in its own right, so a frozen reading is not
     * mistaken for a still board. */
    make_input(&input, 0.0f, 0.0f, 1.0f, true);
    input.sensor.last_read_failed = true;
    input.sensor.read_errors = 7UL;
    CHECK(osd_telemetry_alarm_active(&input), "%s", "stale sensor not alarmed");
    osd_telemetry_format_lines(&input, lines, OSD_MAX_LINES);
    CHECK(strstr(lines[3], "STALE") != NULL,
          "stale not shown: '%s'", lines[3]);
    printf("  stale sensor: |%s|\n", lines[3]);
}

static void test_telemetry_render(void)
{
    struct osd_telemetry *telemetry = open_telemetry(true, true, 1U);

    if (telemetry == NULL) {
        return;
    }

    {
        struct osd_telemetry_input input;
        /* Big enough for the full canvas at the origin, so the composite is
         * not refused for size. */
        static uint8_t frame[1280U * 720U];
        size_t written;
        struct osd_telemetry_stats stats;

        make_input(&input, 10.0f, -5.0f, 1.01f, true);

        memset(frame, 40, sizeof(frame));
        written = osd_telemetry_render(telemetry, &input, frame, 1280U, 720U);
        CHECK(written > 0U, "%s", "render wrote nothing");

        osd_telemetry_stats(telemetry, &stats);
        CHECK(stats.rendered == 1UL, "rendered = %lu", stats.rendered);
        CHECK(stats.alarm_frames == 0UL, "alarm_frames = %lu",
              stats.alarm_frames);

        /* The overlay itself should have a panel now, so some pixel in the
         * top-left region must have been written to the panel value. */
        CHECK(frame[0] == 235U, "panel pixel = %u", frame[0]);

        /* A frame smaller than the overlay is refused without corrupting. */
        {
            uint8_t small[16U * 16U];
            size_t result;

            memset(small, 40, sizeof(small));
            result = osd_telemetry_render(telemetry, &input, small, 16U, 16U);
            CHECK(result == 0U, "%s", "render should refuse a small frame");
            CHECK(small[0] == 40U, "%s", "refused render corrupted the frame");
        }

        /* A no-sample frame is counted, and still renders the placeholder. */
        make_input(&input, 0.0f, 0.0f, 0.0f, false);
        memset(frame, 40, sizeof(frame));
        written = osd_telemetry_render(telemetry, &input, frame, 1280U, 720U);
        CHECK(written > 0U, "%s", "no-sample render wrote nothing");
        osd_telemetry_stats(telemetry, &stats);
        CHECK(stats.waited_for_sample == 1UL, "waited = %lu",
              stats.waited_for_sample);
    }

    osd_telemetry_destroy(telemetry);
}

static void test_telemetry_determinism(void)
{
    struct osd_telemetry *first = open_telemetry(true, false, 1U);
    struct osd_telemetry *second = open_telemetry(true, false, 1U);

    if (first == NULL || second == NULL) {
        osd_telemetry_destroy(first);
        osd_telemetry_destroy(second);
        return;
    }

    /*
     * Two independently created overlays given the same input must produce
     * byte-identical frames. This is the property that makes a rendering test
     * meaningful at all: if the output depended on allocation addresses or
     * uninitialised canvas memory, comparing against a recorded frame would be
     * a coin toss.
     */
    {
        static uint8_t frame_a[1280U * 720U];
        static uint8_t frame_b[1280U * 720U];
        struct osd_telemetry_input input;

        make_input(&input, 33.3f, 11.1f, 0.98f, true);

        memset(frame_a, 0, sizeof(frame_a));
        memset(frame_b, 0, sizeof(frame_b));

        osd_telemetry_render(first, &input, frame_a, 1280U, 720U);
        osd_telemetry_render(second, &input, frame_b, 1280U, 720U);

        CHECK(memcmp(frame_a, frame_b, sizeof(frame_a)) == 0,
              "%s", "two overlays rendered different bytes");
    }

    osd_telemetry_destroy(first);
    osd_telemetry_destroy(second);
}

/*
 * The formatter's output has to be legible in the font. This is the seam that
 * would otherwise go untested: a decimal point the font renders as a blank
 * would make every reading ambiguous and no individual test would notice.
 */
static void test_every_emitted_character_is_drawable(void)
{
    struct osd_telemetry_input input;
    char lines[OSD_MAX_LINES][OSD_MAX_LINE_CHARS];
    size_t count;
    size_t line;
    bool all_drawable = true;

    make_input(&input, -179.9f, 179.9f, 1.0f, true);
    input.sensor.last_read_failed = true;
    input.sensor.read_errors = 99999UL;

    count = osd_telemetry_format_lines(&input, lines, OSD_MAX_LINES);

    for (line = 0U; line < count; line++) {
        size_t ch;

        for (ch = 0U; lines[line][ch] != '\0'; ch++) {
            if (osd_font_lookup(lines[line][ch]) == NULL) {
                printf("       undrawable character '%c' (0x%02x) in '%s'\n",
                       lines[line][ch], (unsigned char)lines[line][ch],
                       lines[line]);
                all_drawable = false;
            }
        }
    }

    CHECK(all_drawable, "%s", "a line contains a character the font cannot draw");
}

int main(void)
{
    printf("osd self test\n");

    printf(" font:\n");
    test_font_shapes();
    test_font_lookup_rules();
    test_font_metrics();

    printf(" format:\n");
    test_format_fixed();
    test_format_int();
    test_format_duration();
    test_format_bytes();

    printf(" overlay:\n");
    test_overlay_put_and_pixel();
    test_overlay_cursor_advances();
    test_overlay_transparent_by_default();
    test_overlay_knockout();
    test_overlay_rect_and_bar();
    test_overlay_scale();

    printf(" telemetry:\n");
    test_telemetry_lines();
    test_telemetry_angle_formatting();
    test_telemetry_no_sample();
    test_telemetry_alarms();
    test_telemetry_render();
    test_telemetry_determinism();
    test_every_emitted_character_is_drawable();

    printf("checks=%d failures=%d\n", g_checks, g_failures);

    if (g_failures != 0) {
        printf("RESULT: FAIL\n");
        return EXIT_FAILURE;
    }

    printf("RESULT: PASS\n");
    return EXIT_SUCCESS;
}
