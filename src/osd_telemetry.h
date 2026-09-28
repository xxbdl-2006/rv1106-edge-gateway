#ifndef OSD_TELEMETRY_H
#define OSD_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "osd_overlay.h"
#include "sensor_source.h"

/*
 * Turns a sensor sample and the pipeline counters into the lines the overlay
 * draws.
 *
 * WHY THIS IS A SEPARATE LAYER. Everything below it is numbers and everything
 * above it is pixels, and the decisions that matter most here are about
 * neither: which values belong on screen at all, what precision they need,
 * and what happens when the sensor stops answering. Those are editorial
 * judgements, and keeping them in one file means changing what the overlay
 * says does not mean touching the ring, the font or the compositor.
 *
 * WHAT GOES ON SCREEN, and why each one earns its space:
 *
 *   pitch / roll      The headline reading. The whole reason the MPU6050 is
 *                     on the board, and the only line that means anything to
 *                     somebody glancing at the video rather than reading a log.
 *   |a|               The acceleration magnitude. A still sensor reads 1.00 g,
 *                     and anything else says the board is moving, is falling,
 *                     or is lying. It is the one number that validates all the
 *                     others, because it is the magnitude of the vector the
 *                     pitch and roll were derived from - if it is wrong, they
 *                     are wrong, and seeing it next to them is what makes that
 *                     check possible at a glance. Also the fastest way to spot
 *                     a detached part: it reads 0.00 g.
 *   temperature       Free on this part, and the single best proxy for whether
 *                     the board is in trouble. A reading that climbs when the
 *                     load has not is the earliest sign of a thermal problem,
 *                     and by the time it becomes a frame rate problem it is
 *                     much harder to attribute.
 *   timestamp / frames The pipeline counters. The timestamp is the reason this
 *                     exists as an overlay rather than a log: it is burned into
 *                     the video, so a recording can be lined up against
 *                     everything else after the fact. The frame counter makes
 *                     a dropped frame visible in the recording itself, which is
 *                     otherwise very hard to prove from a phone video of a
 *                     screen.
 *   rate              Frames per second, computed over a window. Distinguishes
 *                     "the stream looks choppy" from "the stream is fine and the
 *                     network is chopping it up".
 *   sensor status     Whether the last read succeeded and how many have failed.
 *                     Without it a frozen pitch reading looks like a still
 *                     board rather than a dead sensor, which is the single most
 *                     misleading thing this overlay could do.
 *   alarm             Shown only when a threshold is crossed, so its presence
 *                     is itself the signal.
 *
 * WHAT IS DELIBERATELY NOT ON SCREEN: the gyro rates. They read zero on a
 * board that is not being deliberately rotated, so they would occupy a line
 * that is empty in almost every recording, and worse, a nonzero reading means
 * "somebody moved it" rather than anything diagnosable. They belong in the log.
 * Raw I2C counters likewise: useful when bringing the part up, not when
 * watching a stream.
 */

/*
 * Alarm thresholds.
 *
 * These are deliberately loose. The point of the overlay alarm is to catch
 * "this is obviously wrong" - the board has fallen over, the part is
 * disconnected, the readings are nonsense - and not to police the normal
 * range of motion. A threshold tight enough to fire when somebody picks the
 * board up would train the reader to ignore it, which is worse than not having
 * it. The tighter limits belong in the log where they can be tuned against
 * real data.
 */
#define OSD_ALARM_TILT_DEG 60.0f      /* beyond this the board is on its side */
#define OSD_ALARM_MAG_TOLERANCE_G 0.25f  /* |a| outside 1 g by this much */

/*
 * How many lines this layer emits, and how wide the widest one can get.
 *
 * Exposed rather than kept private in the .c because the canvas is sized from
 * them at open time, and a caller that wants to know how much of the frame the
 * overlay will occupy needs the same numbers.
 *
 * OSD_TELEMETRY_WIDTH is the width in characters of the longest line the
 * formatter below can produce, with every field at its maximum. That matters
 * more than it looks: the canvas is allocated once and the composite REFUSES a
 * frame it does not fit in, so an over-generous width does not just waste
 * memory - it makes the overlay silently absent on frames narrower than the
 * bound. Sizing off OSD_MAX_LINE_CHARS (64) instead of this would cost 384
 * pixels of reserved width for lines that are about 30 characters long, which
 * is the difference between an overlay that works on a 320-wide preview and one
 * that never appears at all.
 *
 * The value is the longest of:
 *   "PITCH +179.9  ROLL +179.9  TILT"     (31)
 *   "ACC 9.99 g  TEMP 99.9 C  IMU MAG"    (33)
 *   "T 99999:00:00  FRAME 18446744073709551615  999.9 fps"  (unbounded)
 *   "IMU mock STALE e=18446744073709551615  SENSOR"          (unbounded)
 *
 * The last two are genuinely unbounded because they contain 64 bit counters, so
 * a bound has to be chosen rather than derived. 48 is chosen: it covers the
 * counters for any run shorter than about 100 hours at any plausible frame rate,
 * which is far beyond the 8 hour soak this is built for, and it is still narrow
 * enough to fit a 720p frame with room to spare.
 *
 * What happens when the bound is exceeded is deliberate: builder_puts() stops
 * appending at the capacity, so a line clips at its right edge rather than
 * overrunning. A clipped counter is less bad than a corrupted frame, and the
 * alternative - growing the canvas - is not available because this runs per
 * frame.
 */
#define OSD_TELEMETRY_LINES 4U
#define OSD_TELEMETRY_WIDTH 48U

struct osd_telemetry_clock {
    /* Monotonic microseconds since the program started. */
    uint64_t now_us;
    /* Capture timestamp of the frame being annotated, for the video clock. */
    uint64_t frame_pts_us;
    /* Frames encoded so far, and the rate over the recent window. */
    uint64_t frame_count;
    float frame_rate;
};

struct osd_telemetry_sensor {
    /* The most recent sample, and whether one has arrived at all. */
    bool have_sample;
    struct sensor_sample sample;
    /* Consecutive failed reads, and total failures, from the source. */
    unsigned long read_errors;
    /* True if the last read failed. */
    bool last_read_failed;
    /* Source name, shown so a mock is never mistaken for hardware. */
    const char *source_name;
};

struct osd_telemetry_input {
    struct osd_telemetry_clock clock;
    struct osd_telemetry_sensor sensor;
};

struct osd_telemetry_config {
    /* Draw a filled panel behind the text. Off gives bare text. */
    bool panel;
    /* Padding between the panel edge and the text, in canvas pixels. */
    size_t panel_padding;
    /* Where on the canvas the panel goes, in canvas pixels. */
    size_t origin_x;
    size_t origin_y;
    /*
     * Use knockout text: dark letters cut out of a light panel. Requires
     * panel, since without a panel there is nothing to cut out of.
     */
    bool knockout;
};

struct osd_telemetry_stats {
    /* Frames annotated. */
    unsigned long rendered;
    /* Frames where the sensor had no sample yet. */
    unsigned long waited_for_sample;
    /* Frames drawn while the last sensor read had failed. */
    unsigned long stale_frames;
    /* Frames with at least one alarm active. */
    unsigned long alarm_frames;
};

struct osd_telemetry;

int osd_telemetry_open(const struct osd_telemetry_config *config,
                       struct osd_telemetry **telemetry);

void osd_telemetry_destroy(struct osd_telemetry *telemetry);

/*
 * Draw the overlay for one frame and composite it.
 *
 * `frame` is a tightly packed NV12 luma plane of frame_width x frame_height.
 * The overlay is composited at the configured origin; the panel is sized to
 * whatever the text actually needed, so a shorter reading does not leave a
 * ragged panel.
 *
 * Returns the number of pixels written, or 0 if nothing was drawn - which
 * happens when the overlay does not fit in the frame, and is not an error to
 * be retried.
 */
size_t osd_telemetry_render(struct osd_telemetry *telemetry,
                            const struct osd_telemetry_input *input,
                            uint8_t *frame,
                            size_t frame_width,
                            size_t frame_height);

/*
 * Format the lines without drawing them, for tests and for a future
 * text-to-log path that must agree with the overlay exactly.
 *
 * Writes at most `max_lines` NUL terminated strings into `lines`, each of
 * which must hold at least OSD_MAX_LINE_CHARS bytes. Returns the number of
 * lines written.
 */
size_t osd_telemetry_format_lines(const struct osd_telemetry_input *input,
                                  char lines[][OSD_MAX_LINE_CHARS],
                                  size_t max_lines);

/* True if any alarm threshold is crossed for this input. */
bool osd_telemetry_alarm_active(const struct osd_telemetry_input *input);

const struct osd_overlay *osd_telemetry_overlay(
    const struct osd_telemetry *telemetry);

void osd_telemetry_stats(const struct osd_telemetry *telemetry,
                         struct osd_telemetry_stats *stats);

#endif /* OSD_TELEMETRY_H */
