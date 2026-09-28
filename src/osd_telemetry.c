#include "osd_telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osd_format.h"
#include "osd_font.h"

/*
 * The overlay as it is actually laid out.
 *
 * One canvas lives here for the life of the telemetry object, rebuilt every
 * frame and composited once. The alternative - build a canvas per frame - would
 * allocate thirty times a second, which the project's own rule forbids (see
 * docs/handoff.md on per-frame allocation) and which buys nothing, because the
 * canvas is exactly the same size every time.
 *
 * The panel geometry is deliberately NOT stored: it is a pure function of the
 * lines that were just formatted, so keeping a copy would be a second source of
 * truth that could disagree with the canvas after a resize.
 */
struct osd_telemetry {
    struct osd_overlay *overlay;
    struct osd_telemetry_config config;
    struct osd_telemetry_stats stats;

    /*
     * Scratch big enough for the most lines the overlay can hold. Sized off
     * OSD_MAX_LINES rather than the, shorter, number this file happens to emit
     * so that adding a line to the layout is not a buffer overflow.
     */
    char lines[OSD_MAX_LINES][OSD_MAX_LINE_CHARS];
};

/*
 * Luma written for a set canvas pixel.
 *
 * 235 is video range white: the frame comes off the camera in limited range, so
 * writing 255 would be brighter than the brightest thing the sensor can produce
 * and would clip visibly against a sunlit scene. Using the top of the legal
 * range keeps the overlay at "as bright as the image can go" without lying
 * about the signal range to anything downstream that measures it.
 */
#define OSD_TELEMETRY_LEVEL_ON 235U

/*
 * Alarm words. Short because they share a line with the value that triggered
 * them, and uppercase because that is what the font renders distinctly - a
 * lowercase glyph in this table is the uppercase shape, so 'tilt' would look
 * like 'TILT' anyway and the intent is clearer spelled out.
 */
static const char kAlarmTilt[] = "TILT";
static const char kAlarmMagnitude[] = "IMU MAG";

static float absolute_float(float value)
{
    return value < 0.0f ? -value : value;
}

/*
 * Format a signed angle to one decimal, with the sign always present.
 *
 * Two reasons this is not just osd_format_fixed. The first is the sign: an
 * angle of +12.3 and one of -12.3 must occupy the same width, because this
 * overlay redraws every frame and a value that dropped its sign would shove the
 * rest of the line sideways the moment the board rocked through level.
 * osd_format_fixed deliberately writes no sign for positives, so the '+' has to
 * be added here.
 *
 * Note what this does NOT promise: the width still grows with the number of
 * integer digits, so +9.9 becomes +10.0 and the line is one character longer.
 * That is honest - the magnitude really did change - and pinning the field to a
 * fixed width would mean choosing a maximum and printing leading spaces or
 * zeros for everything smaller, which on a small overlay is worse than the
 * occasional reflow.
 *
 * The second is where the sign comes from. It is taken from the ROUNDED value,
 * not the input, which is what makes a resting reading of -0.02 print as "0.0"
 * and not "-0.0". Printing a minus on something that rounds to zero looks like
 * a real reading and is the kind of thing a reviewer spends ten minutes
 * chasing. Rounding first, once, and deriving everything from that integer
 * means there is no second rounding step that could disagree with the sign.
 *
 * `out` always receives a NUL terminated string, empty if the buffer is
 * unusable.
 */
static void format_angle(char *out, size_t out_size, float degrees)
{
    char digits[24];
    /* Tenths of a degree, rounded half away from zero. */
    long scaled;
    long whole;
    long fraction;
    size_t written = 0U;

    if (out == NULL || out_size == 0U) {
        return;
    }

    out[0] = '\0';

    scaled = (long)(degrees * 10.0f + (degrees < 0.0f ? -0.5f : 0.5f));
    whole = scaled / 10L;
    fraction = scaled % 10L;
    if (fraction < 0L) {
        fraction = -fraction;
    }

    /*
     * The sign is written for a rounded non-zero value only, so "-0.0" cannot
     * be produced. `scaled < 0` is exactly that test: a value that rounded to
     * zero has scaled == 0 and takes the positive branch.
     */
    if (written + 1U < out_size) {
        out[written++] = scaled < 0L ? '-' : '+';
    }

    if (osd_format_int(digits, sizeof(digits), whole < 0L ? -whole : whole) ==
        0U) {
        out[0] = '\0';
        return;
    }

    {
        size_t i;
        for (i = 0U; digits[i] != '\0' && written + 2U < out_size; i++) {
            out[written++] = digits[i];
        }
    }

    if (written + 2U < out_size) {
        out[written++] = '.';
        out[written++] = (char)('0' + fraction);
    }

    out[written] = '\0';
}

/*
 * True when the magnitude is outside the tolerance band around 1 g.
 *
 * Guarded against the "no sample yet" case, which is not an alarm - a sensor
 * that has not reported is a different condition from one reporting nonsense,
 * and conflating them would light the alarm on every boot.
 */
static bool magnitude_alarm(const struct osd_telemetry_input *input)
{
    float deviation;

    if (!input->sensor.have_sample) {
        return false;
    }

    deviation = input->sensor.sample.accel_magnitude_g - 1.0f;
    return absolute_float(deviation) > OSD_ALARM_MAG_TOLERANCE_G;
}

static bool tilt_alarm(const struct osd_telemetry_input *input)
{
    const struct sensor_sample *sample;

    if (!input->sensor.have_sample) {
        return false;
    }

    sample = &input->sensor.sample;
    if (!sample->has_attitude) {
        /*
         * No attitude means the solver refused the reading, which is already
         * the magnitude alarm's business. Reporting a tilt of 0 degrees here
         * (the value left in the struct) would be inventing a measurement.
         */
        return false;
    }

    return absolute_float(sample->pitch_deg) > OSD_ALARM_TILT_DEG ||
           absolute_float(sample->roll_deg) > OSD_ALARM_TILT_DEG;
}

bool osd_telemetry_alarm_active(const struct osd_telemetry_input *input)
{
    if (input == NULL) {
        return false;
    }

    return tilt_alarm(input) || magnitude_alarm(input) ||
           input->sensor.last_read_failed;
}

/*
 * A tiny fixed-buffer string builder.
 *
 * WHY NOT snprintf. The overlay runs per frame and its whole formatting layer
 * (osd_format.h) was written without libc's float support on purpose, so that
 * the bytes it produces are identical on the board's uClibc and on the host's
 * glibc - which is what lets the rendering be tested on the host at all. String
 * concatenation is the one part of that promise snprintf would quietly break,
 * because %s with a truncation is exactly where the two libcs have historically
 * differed on whether the terminator is written. Building the line from three
 * primitives keeps the guarantee the rest of the layer already keeps, and it
 * costs less code than the snprintf calls it replaces.
 */
struct line_builder {
    char *buffer;
    size_t capacity;
    size_t length;
};

static void builder_init(struct line_builder *builder, char *buffer,
                         size_t capacity)
{
    builder->buffer = buffer;
    builder->capacity = capacity;
    builder->length = 0U;

    if (capacity != 0U) {
        buffer[0] = '\0';
    }
}

static void builder_puts(struct line_builder *builder, const char *text)
{
    if (builder->capacity == 0U || text == NULL) {
        return;
    }

    while (*text != '\0' && builder->length + 1U < builder->capacity) {
        builder->buffer[builder->length++] = *text++;
    }

    builder->buffer[builder->length] = '\0';
}

static void builder_put_int(struct line_builder *builder, unsigned long value)
{
    char digits[24];

    if (osd_format_int(digits, sizeof(digits), (long)value) == 0U) {
        return;
    }

    builder_puts(builder, digits);
}

static void builder_put_fixed(struct line_builder *builder, float value,
                              unsigned decimals)
{
    char text[24];

    if (osd_format_fixed(text, sizeof(text), value, decimals) == 0U) {
        return;
    }

    builder_puts(builder, text);
}

/*
 * Compose the lines.
 *
 * The order is deliberate and matches the header's ranking: the readings a
 * person watches come first, the pipeline counters next, and the fault text
 * last, because on a healthy system the last line is the only one that ever
 * changes and it is the one worth being able to ignore when it is empty.
 */
static size_t format_lines_impl(const struct osd_telemetry_input *input,
                                char lines[][OSD_MAX_LINE_CHARS],
                                size_t max_lines)
{
    size_t count = 0U;
    struct line_builder builder;
    bool tilt;
    bool magnitude;

    if (input == NULL || lines == NULL || max_lines == 0U) {
        return 0U;
    }

    tilt = tilt_alarm(input);
    magnitude = magnitude_alarm(input);

    /*
     * Line 1: the headline reading. Both angles on one line because they are
     * read together - a pitch of 20 with a roll of 1 is a different situation
     * from the reverse, and splitting them across lines makes the reader do the
     * pairing in their head.
     *
     * A sample that has not arrived yet prints "--" for each value rather than
     * 0.0. Zero is a real reading (flat and level) and printing it for "no data"
     * is the single most misleading thing this overlay could do, which is the
     * same distinction the attitude solver's has_attitude flag exists to make.
     */
    builder_init(&builder, lines[count], OSD_MAX_LINE_CHARS);
    if (!input->sensor.have_sample) {
        builder_puts(&builder, "PITCH --    ROLL --");
    } else if (!input->sensor.sample.has_attitude) {
        builder_puts(&builder, "PITCH --    ROLL --   NO ATT");
    } else {
        builder_puts(&builder, "PITCH ");
        {
            char pitch_text[16];
            format_angle(pitch_text, sizeof(pitch_text),
                         input->sensor.sample.pitch_deg);
            builder_puts(&builder, pitch_text);
        }
        builder_puts(&builder, "  ROLL ");
        {
            char roll_text[16];
            format_angle(roll_text, sizeof(roll_text),
                         input->sensor.sample.roll_deg);
            builder_puts(&builder, roll_text);
        }
        if (tilt) {
            builder_puts(&builder, "  ");
            builder_puts(&builder, kAlarmTilt);
        }
    }
    count++;

    /* Line 2: acceleration magnitude, with the temperature sharing the line.
     * Both are "is the board itself healthy" numbers and both are short, so
     * pairing them keeps the overlay to a size that does not fight the video. */
    builder_init(&builder, lines[count], OSD_MAX_LINE_CHARS);
    if (!input->sensor.have_sample) {
        builder_puts(&builder, "ACC --  TEMP --");
    } else {
        builder_puts(&builder, "ACC ");
        builder_put_fixed(&builder, input->sensor.sample.accel_magnitude_g, 2U);
        builder_puts(&builder, " g  TEMP ");
        builder_put_fixed(&builder, input->sensor.sample.temperature_c, 1U);
        builder_puts(&builder, " C");
        if (magnitude) {
            builder_puts(&builder, "  ");
            builder_puts(&builder, kAlarmMagnitude);
        }
    }
    count++;

    /*
     * Line 3: the pipeline counters.
     *
     * The timestamp is the reason an overlay is worth more than a log here: it
     * is burned into the recording, so a video can be lined up against
     * everything else after the fact without a burned-in clock on the scene.
     * The frame counter makes a dropped frame visible in the recording itself.
     *
     * Both are in the same line for the same reason as the angles.
     *
     * frame_rate is a float and goes through the same fixed point formatter as
     * everything else, so a rate that reports 29.9999 still prints as "30.0".
     */
    builder_init(&builder, lines[count], OSD_MAX_LINE_CHARS);
    builder_puts(&builder, "T ");
    {
        char duration[16];

        if (osd_format_duration(duration, sizeof(duration),
                                input->clock.now_us) != 0U) {
            builder_puts(&builder, duration);
        }
    }
    builder_puts(&builder, "  FRAME ");
    builder_put_int(&builder, (unsigned long)input->clock.frame_count);
    builder_puts(&builder, "  ");
    builder_put_fixed(&builder, input->clock.frame_rate, 1U);
    builder_puts(&builder, " fps");
    count++;

    /*
     * Line 4: the sensor's own health.
     *
     * This exists so a frozen pitch reading cannot be mistaken for a still
     * board. Without it, a dead sensor and a level board render identically
     * once the last sample stops updating - which is exactly the failure this
     * overlay is most likely to be asked to diagnose.
     *
     * The source name is included so a mock is never mistaken for hardware
     * while demoing, which has already happened once in this project.
     */
    builder_init(&builder, lines[count], OSD_MAX_LINE_CHARS);
    builder_puts(&builder, "IMU ");
    builder_puts(&builder, input->sensor.source_name != NULL
                               ? input->sensor.source_name
                               : "?");
    if (!input->sensor.have_sample) {
        builder_puts(&builder, " WAIT");
    } else if (input->sensor.last_read_failed) {
        builder_puts(&builder, " STALE e=");
        builder_put_int(&builder, input->sensor.read_errors);
        builder_puts(&builder, "  SENSOR");
    } else {
        builder_puts(&builder, " OK e=");
        builder_put_int(&builder, input->sensor.read_errors);
    }
    count++;

    return count;
}

size_t osd_telemetry_format_lines(const struct osd_telemetry_input *input,
                                  char lines[][OSD_MAX_LINE_CHARS],
                                  size_t max_lines)
{
    return format_lines_impl(input, lines, max_lines);
}

int osd_telemetry_open(const struct osd_telemetry_config *config,
                       struct osd_telemetry **telemetry)
{
    struct osd_telemetry *created;
    struct osd_overlay_config overlay_config;

    if (telemetry == NULL) {
        return -1;
    }
    *telemetry = NULL;

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        perror("osd_telemetry_open: calloc");
        return -1;
    }

    if (config != NULL) {
        created->config = *config;
    }

    /*
     * Knockout without a panel is meaningless: knockout works by cutting the
     * glyphs out of a filled box, and with no box there is nothing to cut.
     * Coercing it off rather than refusing the configuration keeps a caller
     * that toggles the panel at runtime from having to remember to toggle both.
     */
    if (!created->config.panel) {
        created->config.knockout = false;
    }

    /*
     * The canvas is sized for the worst case the layout can produce: the
     * longest line the formatter can emit, over the full complement of lines,
     * plus the panel padding on both sides. Sizing it off the current values
     * would mean the overlay changed size when a sensor went stale, and a
     * per-frame reallocation is exactly what the project forbids.
     *
     * OSD_MAX_LINE_CHARS is the true worst case rather than a guess at the
     * longest string below: a guess has to be updated whenever a line is added,
     * and the failure mode when somebody forgets is a clipped line, which is
     * silent.
     */
    overlay_config.width = OSD_MAX_LINE_CHARS * (size_t)OSD_FONT_ADVANCE;
    overlay_config.height =
        osd_overlay_lines_height(OSD_TELEMETRY_LINES) +
        2U * created->config.panel_padding;
    overlay_config.scale = 1U;

    if (osd_overlay_open(&overlay_config, &created->overlay) != 0) {
        free(created);
        return -1;
    }

    *telemetry = created;
    return 0;
}

void osd_telemetry_destroy(struct osd_telemetry *telemetry)
{
    if (telemetry == NULL) {
        return;
    }

    osd_overlay_destroy(telemetry->overlay);
    free(telemetry);
}

/*
 * Draw the panel behind the text, sized to the text that was actually drawn.
 *
 * The panel is measured from the line array rather than from the canvas, so a
 * stale line that happened to be longer does not leave the panel wide. The
 * height comes from the line count, which is why the caller passes it in.
 */
static void draw_panel(const struct osd_telemetry *telemetry, size_t line_count)
{
    size_t text_width = 0U;
    size_t line;
    size_t padding = telemetry->config.panel_padding;
    size_t height;

    for (line = 0U; line < line_count; line++) {
        size_t width = osd_overlay_text_width(telemetry->lines[line]);

        if (width > text_width) {
            text_width = width;
        }
    }

    if (text_width == 0U || line_count == 0U) {
        return;
    }

    height = osd_overlay_lines_height(line_count);

    /*
     * The panel is filled with SET pixels, and the text is then written in
     * knockout mode when the caller asked for it - which clears the glyph
     * pixels back out of the box. Doing it in that order is what gives dark
     * letters on a light panel: the box is the bright thing and the frame shows
     * through the letter shapes.
     *
     * When knockout is off the panel is instead filled and the glyphs are drawn
     * on top with puts(), which gives light text on a dark box. Both are
     * readable; knockout is the better default over live video because it does
     * not depend on the scene being dark. See the overlay header.
     */
    osd_overlay_fill_rect(telemetry->overlay, 0U, 0U,
                          text_width + 2U * padding, height + 2U * padding,
                          true);
}

size_t osd_telemetry_render(struct osd_telemetry *telemetry,
                            const struct osd_telemetry_input *input,
                            uint8_t *frame,
                            size_t frame_width,
                            size_t frame_height)
{
    size_t line_count;
    size_t line;
    size_t written;
    size_t padding;

    if (telemetry == NULL || input == NULL || frame == NULL) {
        return 0U;
    }

    if (!input->sensor.have_sample) {
        telemetry->stats.waited_for_sample++;
    }
    if (input->sensor.last_read_failed) {
        telemetry->stats.stale_frames++;
    }
    if (osd_telemetry_alarm_active(input)) {
        telemetry->stats.alarm_frames++;
    }

    line_count = osd_telemetry_format_lines(input, telemetry->lines,
                                            OSD_MAX_LINES);
    if (line_count == 0U) {
        return 0U;
    }

    /*
     * Rebuild rather than clear-and-redraw: a canvas that is cleared and then
     * partially drawn leaves the un-drawn part transparent, so the previous
     * frame's text cannot ghost through even if the new text is shorter. Since
     * the canvas is owned here and never read before being written, resetting
     * is the whole of the invalidation.
     */
    osd_overlay_reset(telemetry->overlay);

    padding = telemetry->config.panel ? telemetry->config.panel_padding : 0U;

    if (telemetry->config.panel) {
        draw_panel(telemetry, line_count);
    }

    osd_overlay_move_to(telemetry->overlay, padding, padding);

    for (line = 0U; line < line_count; line++) {
        bool drawn;

        /*
         * Knockout for every line, not just the alarming ones: mixing the two
         * modes would mean one line had light text and another dark, which
         * reads as an error in the overlay rather than as information.
         */
        if (telemetry->config.panel && telemetry->config.knockout) {
            drawn = osd_overlay_puts_knockout(telemetry->overlay,
                                              telemetry->lines[line]);
        } else {
            drawn = osd_overlay_puts(telemetry->overlay,
                                     telemetry->lines[line]);
        }

        (void)drawn;
    }

    written = osd_overlay_composite_nv12(telemetry->overlay, frame, frame_width,
                                         frame_height,
                                         telemetry->config.origin_x,
                                         telemetry->config.origin_y,
                                         (uint8_t)OSD_TELEMETRY_LEVEL_ON);

    /*
     * A zero return here is not necessarily "nothing to draw": it is also what
     * a frame too small for the overlay produces. Those are different
     * conditions, and counting the frame as rendered only in the first case is
     * what makes the statistic useful for diagnosing a mismatched frame size.
     */
    if (written != 0U) {
        telemetry->stats.rendered++;
    }

    return written;
}

const struct osd_overlay *osd_telemetry_overlay(
    const struct osd_telemetry *telemetry)
{
    return telemetry != NULL ? telemetry->overlay : NULL;
}

void osd_telemetry_stats(const struct osd_telemetry *telemetry,
                         struct osd_telemetry_stats *stats)
{
    if (telemetry == NULL || stats == NULL) {
        return;
    }

    *stats = telemetry->stats;
}
