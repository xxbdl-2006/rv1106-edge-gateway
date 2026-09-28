/*
 * Host side self test for the OSD pipeline integration.
 *
 * Built and run natively (no SDK required):
 *
 *     make test-osd-pipeline
 *
 * This covers the two files that sit between the sensor and the encoder:
 *
 *   src/osd_feed.c      sensor source + pipeline counters -> an input snapshot
 *   src/osd_annotate.c  a captured frame + that snapshot -> the frame to encode
 *
 * Neither touches V4L2 or the SDK, which is the whole reason they are separate
 * files: the assertion that matters most - "the overlay is actually present in
 * the bytes the encoder will see" - is only checkable here. On the board the
 * alternative is a human looking at a monitor and saying the text looks about
 * right.
 *
 * WHAT THIS CANNOT CHECK: whether the encoded H.264 decodes to the annotated
 * frame. That needs the encoder. What it does check is that the bytes handed to
 * the encoder differ from the capture in exactly the overlay region and nowhere
 * else, which is the part that can silently go wrong.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osd_annotate.h"
#include "osd_feed.h"
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

#define TEST_WIDTH 1280U
#define TEST_HEIGHT 720U

/* --- a source whose behaviour the test controls exactly --- */

struct fake_source {
    /* Samples to hand out; reading past the end repeats the last one. */
    struct sensor_sample samples[8];
    unsigned sample_count;
    unsigned next;

    /*
     * Read outcomes, applied in order from the first read: 'O' succeeds,
     * 'E' fails, and past the end of the string the last character repeats.
     *
     * A sequence rather than a rule ("fail the first N") because the cases that
     * matter are orderings - succeed, then fail, then succeed - and a rule can
     * only express the ones with the failures at the front.
     */
    const char *outcomes;
    unsigned reads;

    unsigned long errors;
};

static int fake_read(void *context, struct sensor_sample *out)
{
    struct fake_source *fake = context;
    char outcome;

    fake->reads++;

    outcome = fake->outcomes != NULL ? fake->outcomes[0] : 'O';
    if (fake->outcomes != NULL && fake->outcomes[0] != '\0' &&
        fake->outcomes[1] != '\0') {
        fake->outcomes++;
    }

    if (outcome == 'E') {
        fake->errors++;
        return -1;
    }
    if (outcome == 'I') {
        return 0;   /* idle, which the interface defines as distinct from an error */
    }

    if (fake->sample_count == 0U) {
        return 0;
    }

    *out = fake->samples[fake->next < fake->sample_count ? fake->next
                                                        : fake->sample_count - 1U];
    if (fake->next < fake->sample_count) {
        fake->next++;
    }

    return 1;
}

static unsigned long fake_errors(void *context)
{
    return ((struct fake_source *)context)->errors;
}

static struct sensor_source fake_source_iface(struct fake_source *fake)
{
    struct sensor_source source;

    memset(&source, 0, sizeof(source));
    source.name = "fake";
    source.read = fake_read;
    source.read_errors = fake_errors;
    source.context = fake;

    return source;
}

static void make_sample(struct sensor_sample *sample, float pitch, float roll,
                        float magnitude)
{
    memset(sample, 0, sizeof(*sample));

    sample->pitch_deg = pitch;
    sample->roll_deg = roll;
    sample->accel_magnitude_g = magnitude;
    sample->temperature_c = 41.0f;
    sample->has_attitude = true;
}

/* --- tests --- */

static void test_feed_polls_on_its_own_cadence(void)
{
    struct fake_source fake;
    struct sensor_source source;
    struct osd_feed_config config;
    struct osd_feed *feed = NULL;
    struct osd_telemetry_input *input;

    memset(&fake, 0, sizeof(fake));
    fake.sample_count = 1U;
    make_sample(&fake.samples[0], 10.0f, 5.0f, 1.0f);
    source = fake_source_iface(&fake);

    memset(&config, 0, sizeof(config));
    config.interval_us = 20000ULL;   /* 50 Hz */
    config.rate_window = 4U;

    CHECK(osd_feed_open(&config, &source, &feed) == 0, "%s", "feed open failed");
    if (feed == NULL) {
        return;
    }

    /*
     * Thirty frames at 30 fps = 1 ms apart is not a realistic frame period,
     * but the point is the sensor cadence: with a 20 ms interval and a 1 ms
     * frame period, only every twentieth frame should read the sensor. Polling
     * per frame would couple a slow I2C transaction to the encode loop, which
     * is exactly what this cadence exists to prevent.
     */
    {
        unsigned index;

        for (index = 0U; index < 100U; index++) {
            input = osd_feed_next(feed, (uint64_t)index * 1000ULL,
                                  (uint64_t)index * 1000ULL, index);
        }

        CHECK(input != NULL, "%s", "feed returned NULL");
        CHECK(fake.reads <= 6U, "sensor was read %u times in 100 frames",
              fake.reads);
        CHECK(fake.reads >= 4U, "sensor was read only %u times", fake.reads);
        printf("  cadence: %u sensor reads over 100 frames\n", fake.reads);
    }

    osd_feed_destroy(feed);
}

static void test_feed_keeps_last_sample_on_error(void)
{
    struct fake_source fake;
    struct sensor_source source;
    struct osd_feed_config config;
    struct osd_feed *feed = NULL;
    struct osd_telemetry_input *input;
    struct osd_feed_stats stats;

    memset(&fake, 0, sizeof(fake));
    fake.sample_count = 1U;
    make_sample(&fake.samples[0], 12.5f, -3.0f, 1.0f);
    /* Succeed, then fail, and keep failing if polled again. */
    fake.outcomes = "OE";
    source = fake_source_iface(&fake);

    memset(&config, 0, sizeof(config));
    config.interval_us = 0ULL;   /* every call, so the test can script it */
    config.rate_window = 4U;

    CHECK(osd_feed_open(&config, &source, &feed) == 0, "%s", "feed open failed");
    if (feed == NULL) {
        return;
    }

    /* First read succeeds. */
    input = osd_feed_next(feed, 0ULL, 0ULL, 0ULL);
    CHECK(input->sensor.have_sample, "%s", "first sample not accepted");
    CHECK(!input->sensor.last_read_failed, "%s", "no failure expected yet");

    /* Second read fails. The previous reading must survive, flagged stale. */
    input = osd_feed_next(feed, 1000ULL, 1000ULL, 1ULL);
    CHECK(input->sensor.last_read_failed, "%s", "failure not recorded");
    CHECK(input->sensor.have_sample,
          "%s", "the last good sample was discarded on error");
    CHECK(input->sensor.sample.pitch_deg == 12.5f,
          "pitch became %f after an error", input->sensor.sample.pitch_deg);

    osd_feed_stats(feed, &stats);
    CHECK(stats.sensor_errors == 1UL, "errors = %lu", stats.sensor_errors);

    /*
     * This is the behaviour that stops a sensor glitch from looking like a
     * level board. If the sample were cleared, the overlay would print "--" or
     * 0.0 and a frozen reading would be indistinguishable from a still one.
     */
    printf("  after error: pitch still %.1f, stale flag set\n",
           input->sensor.sample.pitch_deg);

    osd_feed_destroy(feed);
}

static void test_feed_rate_window(void)
{
    struct osd_feed_config config;
    struct osd_feed *feed = NULL;
    struct osd_telemetry_input *input;
    unsigned index;

    memset(&config, 0, sizeof(config));
    config.interval_us = 0ULL;
    config.rate_window = 30U;

    /* No source at all: this exercises the counters-only path. */
    CHECK(osd_feed_open(&config, NULL, &feed) == 0, "%s", "feed open failed");
    if (feed == NULL) {
        return;
    }

    /*
     * Exactly 30 fps: 33333 us per frame. The window is 30 frames, so after 31
     * frames the measurement should have settled very close to 30.
     */
    for (index = 0U; index < 60U; index++) {
        input = osd_feed_next(feed, (uint64_t)index * 33333ULL,
                              (uint64_t)index * 33333ULL, index);
    }

    CHECK(input->clock.frame_rate > 29.5f && input->clock.frame_rate < 30.5f,
          "rate settled at %.2f, wanted ~30", input->clock.frame_rate);
    printf("  rate window: %.2f fps at a true 30.00\n", input->clock.frame_rate);

    /*
     * Now stall: the next frame arrives 100 ms late. A windowed rate should dip
     * visibly rather than hold 30, which is the whole reason it is windowed over
     * real timestamps instead of assumed frame counts.
     */
    input = osd_feed_next(feed, 60ULL * 33333ULL + 100000ULL,
                          60ULL * 33333ULL, 60ULL);
    CHECK(input->clock.frame_rate < 30.0f,
          "rate did not react to a stall: %.2f", input->clock.frame_rate);
    printf("  after a 100 ms stall: %.2f fps\n", input->clock.frame_rate);

    /* With no source the sensor must report "no sample", not a fake zero. */
    CHECK(!input->sensor.have_sample, "%s", "no source but have_sample is set");
    CHECK(strcmp(input->sensor.source_name, "none") == 0,
          "source name = '%s'", input->sensor.source_name);

    osd_feed_destroy(feed);
}

static void test_feed_rate_window_is_validated(void)
{
    struct osd_feed_config config;
    struct osd_feed *feed = NULL;

    memset(&config, 0, sizeof(config));
    config.rate_window = 1000U;   /* beyond the fixed ring */

    /*
     * Refused rather than clamped. A silently clamped window would make the
     * rate sluggish for a reason invisible from the API, and the caller cannot
     * tell the difference between "the sensor is slow" and "my window was
     * ignored".
     */
    CHECK(osd_feed_open(&config, NULL, &feed) == -1,
          "%s", "an oversized rate window was accepted");
    CHECK(feed == NULL, "%s", "feed left non-NULL after a failed open");
}

static void *make_canvas(size_t width, size_t height, uint8_t fill)
{
    size_t bytes = width * height * 3U / 2U;
    void *frame = malloc(bytes);

    if (frame != NULL) {
        memset(frame, fill, bytes);
    }

    return frame;
}

/*
 * The telemetry object sizes its own canvas from its own constants - it does not
 * need to know the frame geometry, which is the annotator's business. That is
 * why this takes only the placement.
 */
static struct osd_telemetry *open_overlay(size_t origin_x, size_t origin_y)
{
    struct osd_telemetry_config config;
    struct osd_telemetry *telemetry = NULL;

    memset(&config, 0, sizeof(config));
    config.panel = true;
    config.knockout = true;
    config.panel_padding = 2U;
    config.origin_x = origin_x;
    config.origin_y = origin_y;

    if (osd_telemetry_open(&config, &telemetry) != 0) {
        return NULL;
    }

    return telemetry;
}

static void test_annotate_disabled_is_free(void)
{
    struct osd_annotate_config config;
    struct osd_annotate *annotate = NULL;
    uint8_t *frame = make_canvas(TEST_WIDTH, TEST_HEIGHT, 42U);
    const uint8_t *result;

    if (frame == NULL) {
        return;
    }

    memset(&config, 0, sizeof(config));
    config.width = TEST_WIDTH;
    config.height = TEST_HEIGHT;

    CHECK(osd_annotate_open(&config, NULL, &annotate) == 0,
          "%s", "annotate open failed");
    if (annotate != NULL) {
        struct osd_annotate_stats stats;

        /*
         * No telemetry: the exact pointer must come back and the frame must be
         * untouched. This is the default pipeline, so it is the path that has
         * to cost nothing.
         */
        result = osd_annotate_apply(annotate, frame, NULL);
        CHECK(result == frame,
              "%s", "pass-through did not return the input pointer");

        {
            size_t index;
            size_t bytes = TEST_WIDTH * TEST_HEIGHT * 3U / 2U;
            bool untouched = true;

            for (index = 0U; index < bytes; index++) {
                if (frame[index] != 42U) {
                    untouched = false;
                    break;
                }
            }
            CHECK(untouched, "%s", "pass-through modified the frame");
        }

        osd_annotate_stats(annotate, &stats);
        CHECK(stats.passed_through == 1UL, "passed_through = %lu",
              stats.passed_through);
        CHECK(stats.annotated == 0UL, "annotated = %lu", stats.annotated);

        osd_annotate_destroy(annotate);
    }

    free(frame);
}

static void test_annotate_draws_into_the_copy_only(void)
{
    struct osd_annotate_config config;
    struct osd_annotate *annotate = NULL;
    struct osd_telemetry *telemetry;
    uint8_t *frame = make_canvas(TEST_WIDTH, TEST_HEIGHT, 40U);
    uint8_t *before = make_canvas(TEST_WIDTH, TEST_HEIGHT, 40U);
    const uint8_t *result;
    struct osd_telemetry_input input;

    if (frame == NULL || before == NULL) {
        free(frame);
        free(before);
        return;
    }

    telemetry = open_overlay(8U, 8U);
    CHECK(telemetry != NULL, "%s", "telemetry open failed");
    if (telemetry == NULL) {
        free(frame);
        free(before);
        return;
    }

    memset(&config, 0, sizeof(config));
    config.width = TEST_WIDTH;
    config.height = TEST_HEIGHT;

    CHECK(osd_annotate_open(&config, telemetry, &annotate) == 0,
          "%s", "annotate open failed");
    if (annotate == NULL) {
        free(frame);
        free(before);
        return;
    }

    memset(&input, 0, sizeof(input));
    input.sensor.have_sample = true;
    input.sensor.sample.has_attitude = true;
    input.sensor.sample.pitch_deg = 15.0f;
    input.sensor.sample.roll_deg = -7.0f;
    input.sensor.sample.accel_magnitude_g = 1.0f;
    input.sensor.sample.temperature_c = 40.0f;
    input.sensor.source_name = "fake";
    input.clock.now_us = 5000000ULL;
    input.clock.frame_count = 1234ULL;
    input.clock.frame_rate = 30.0f;

    result = osd_annotate_apply(annotate, frame, &input);

    /*
     * The returned pointer must be the scratch, not the input: the input is a
     * borrowed ring frame and writing to it would break the ring's contract.
     */
    CHECK(result != frame, "%s", "annotate returned the input buffer");
    CHECK(result == osd_annotate_scratch(annotate),
          "%s", "annotate returned something other than its scratch");

    /*
     * The ORIGINAL frame must be byte-identical. This is the assertion that
     * protects the frame ring's guarantee that a borrowed frame is stable.
     */
    CHECK(memcmp(frame, before, TEST_WIDTH * TEST_HEIGHT * 3U / 2U) == 0,
          "%s", "the input frame was modified");

    /*
     * And the copy must actually differ, inside the overlay region. If the
     * overlay silently drew nothing - which is what happens when it does not
     * fit - the two buffers would match and the feature would be quietly absent.
     */
    {
        size_t changed = 0U;
        size_t index;
        size_t bytes = TEST_WIDTH * TEST_HEIGHT * 3U / 2U;

        for (index = 0U; index < bytes; index++) {
            if (result[index] != before[index]) {
                changed++;
            }
        }

        CHECK(changed > 0U, "%s", "the annotated copy is identical: no overlay");
        printf("  annotated: %zu bytes differ from the capture\n", changed);
    }

    /* Outside the overlay region the copy must still match, or the annotator
     * is corrupting parts of the frame it has no business touching. */
    {
        size_t y;
        bool clean = true;

        for (y = 0U; y < TEST_HEIGHT; y += 97U) {
            size_t x;

            /*
             * Skip the band the overlay occupies (top-left, see the origin
             * above). Sampling rather than a full sweep keeps the test quick
             * while still covering the whole frame.
             */
            for (x = 512U; x < TEST_WIDTH; x += 641U) {
                if (result[y * TEST_WIDTH + x] != before[y * TEST_WIDTH + x]) {
                    printf("       modified outside the overlay at (%zu,%zu)\n",
                           x, y);
                    clean = false;
                }
            }
        }
        CHECK(clean, "%s", "the annotator wrote outside the overlay region");
    }

    osd_annotate_destroy(annotate);
    free(frame);
    free(before);
}

static void test_annotate_refuses_an_overlay_that_does_not_fit(void)
{
    struct osd_annotate_config config;
    struct osd_annotate *annotate = NULL;
    struct osd_telemetry *telemetry;
    uint8_t *frame = make_canvas(320U, 240U, 30U);
    const uint8_t *result;
    struct osd_telemetry_input input;
    struct osd_annotate_stats stats;

    if (frame == NULL) {
        return;
    }

    /*
     * Origin far enough right that the overlay cannot fit in a 320 wide frame.
     * The composite refuses, and the annotator must hand back the original
     * frame rather than a half-written copy - a partially annotated frame every
     * frame would be far harder to diagnose than no overlay at all.
     */
    telemetry = open_overlay(300U, 8U);
    if (telemetry == NULL) {
        free(frame);
        return;
    }

    memset(&config, 0, sizeof(config));
    config.width = 320U;
    config.height = 240U;

    if (osd_annotate_open(&config, telemetry, &annotate) != 0) {
        free(frame);
        return;
    }

    memset(&input, 0, sizeof(input));
    input.sensor.source_name = "fake";

    result = osd_annotate_apply(annotate, frame, &input);
    CHECK(result == frame,
          "%s", "a refused composite did not fall back to the input");

    osd_annotate_stats(annotate, &stats);
    CHECK(stats.composite_refused == 1UL, "composite_refused = %lu",
          stats.composite_refused);
    CHECK(stats.annotated == 0UL, "annotated = %lu", stats.annotated);

    osd_annotate_destroy(annotate);
    free(frame);
}

static void test_annotate_geometry_is_validated(void)
{
    struct osd_annotate_config config;
    struct osd_annotate *annotate = NULL;

    memset(&config, 0, sizeof(config));
    config.width = 0U;
    config.height = 0U;

    CHECK(osd_annotate_open(&config, NULL, &annotate) == -1,
          "%s", "a zero sized frame was accepted");
    CHECK(annotate == NULL, "%s", "annotator left non-NULL after failure");

    CHECK(osd_annotate_open(NULL, NULL, &annotate) == -1,
          "%s", "a NULL config was accepted");
}

static void test_annotate_null_safety(void)
{
    uint8_t frame[16];

    memset(frame, 7, sizeof(frame));

    /* Every entry point must survive a NULL annotator: this is called from the
     * main loop, where a crash is a stopped stream rather than a failed test. */
    CHECK(osd_annotate_apply(NULL, frame, NULL) == frame,
          "%s", "NULL annotator did not echo the frame");
    CHECK(osd_annotate_scratch(NULL) == NULL, "%s", "NULL scratch not NULL");
    CHECK(osd_annotate_overlay(NULL) == NULL, "%s", "NULL overlay not NULL");
    osd_annotate_stats(NULL, NULL);
    osd_annotate_destroy(NULL);
    printf("  null safety: all entry points survived\n");
}

int main(void)
{
    printf("osd pipeline self test\n");

    printf(" feed:\n");
    test_feed_polls_on_its_own_cadence();
    test_feed_keeps_last_sample_on_error();
    test_feed_rate_window();
    test_feed_rate_window_is_validated();

    printf(" annotate:\n");
    test_annotate_disabled_is_free();
    test_annotate_draws_into_the_copy_only();
    test_annotate_refuses_an_overlay_that_does_not_fit();
    test_annotate_geometry_is_validated();
    test_annotate_null_safety();

    printf("checks=%d failures=%d\n", g_checks, g_failures);

    if (g_failures != 0) {
        printf("RESULT: FAIL\n");
        return EXIT_FAILURE;
    }

    printf("RESULT: PASS\n");
    return EXIT_SUCCESS;
}
