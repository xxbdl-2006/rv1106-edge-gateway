#include "osd_format.h"

/*
 * Powers of ten as integers, so the decimal scaling is exact integer maths.
 * Indexed by the number of decimals requested.
 */
static const uint32_t kPow10[4] = { 1U, 10U, 100U, 1000U };

/*
 * Write a NUL and return how much of the buffer was used, handling the case
 * where out_size is 1 (room for the terminator only) or 0 (nothing at all).
 */
static size_t terminate(char *out, size_t out_size, size_t written)
{
    if (out_size == 0U) {
        return 0U;
    }

    if (written >= out_size) {
        out[out_size - 1U] = '\0';
        return out_size - 1U;
    }

    out[written] = '\0';
    return written;
}

/*
 * Convert a magnitude to decimal digits into `scratch`, most significant
 * first, and return the digit count. `scratch` must hold at least 20 bytes:
 * a 64 bit value never needs more.
 *
 * Always writes at least one digit, so 0 becomes "0" rather than "".
 */
static size_t to_digits(unsigned long long magnitude, char *scratch)
{
    size_t length = 0U;
    size_t i;

    if (magnitude == 0ULL) {
        scratch[0] = '0';
        return 1U;
    }

    while (magnitude != 0ULL) {
        scratch[length] = (char)('0' + (magnitude % 10ULL));
        magnitude /= 10ULL;
        length++;
    }

    /* Reversed on the way out. */
    for (i = 0U; i < length / 2U; i++) {
        char swap = scratch[i];
        scratch[i] = scratch[length - 1U - i];
        scratch[length - 1U - i] = swap;
    }

    return length;
}

/*
 * Emit the absolute value of `value` scaled by 10^decimals.
 *
 * The float is rounded to the nearest scaled integer here, once, which is why
 * the rest of this file has no float in it. Doing the rounding in double
 * rather than float keeps a value like 0.145 from landing on the wrong side of
 * the boundary purely because the float approximation was slightly low; the
 * conversion is on a value that came from a float anyway, so nothing is lost.
 */
static unsigned long long scaled_magnitude(float value, unsigned decimals)
{
    double magnitude = (double)value;
    double scale = (double)kPow10[decimals];

    if (magnitude < 0.0) {
        magnitude = -magnitude;
    }

    /*
     * +0.5 then truncate is round-half-up on the magnitude, which on a signed
     * value is round-half-away-from-zero. Adding 0.5 to a very large double
     * cannot overflow into UB because the value is bounded by the caller's
     * float, and the cast to unsigned long long is only UB for values that
     * big numbers would not reach in this application.
     */
    return (unsigned long long)(magnitude * scale + 0.5);
}

size_t osd_format_fixed(char *out, size_t out_size, float value,
                        unsigned decimals)
{
    char scratch[24];
    size_t written = 0U;
    size_t digits;
    size_t integer_digits;
    size_t total;
    unsigned long long scaled;
    bool negative;
    size_t position;

    if (out == NULL || out_size == 0U || decimals > 3U) {
        return 0U;
    }

    scaled = scaled_magnitude(value, decimals);

    /*
     * The sign is taken from the rounded magnitude, not from the input. That
     * is what turns a tiny negative noise reading into "0.0" instead of
     * "-0.0": -0.02 with one decimal rounds to 0, the sign is then false, and
     * the minus never gets written. Printing "-0.0" on a resting sensor looks
     * like a real reading and is exactly the kind of thing a reviewer spends
     * ten minutes chasing.
     */
    negative = (value < 0.0f) && (scaled != 0ULL);

    /*
     * Render the whole thing as an integer digit string first - the value
     * times 10^decimals - and put the point in afterwards. Working on one
     * digit array rather than trying to interleave the integer and fractional
     * halves is the difference between an obvious loop and one that needs a
     * comment per line to follow.
     */
    digits = to_digits(scaled, scratch);

    /*
     * Pad on the left so there is always at least one digit before the point
     * and exactly `decimals` after it: 5 with two decimals must become "005"
     * so the point lands as "0.05", not ".5" or "0.5".
     */
    integer_digits = digits > decimals ? digits - decimals : 1U;
    total = integer_digits + decimals;

    if (negative) {
        if (written + 1U >= out_size) {
            return terminate(out, out_size, written);
        }
        out[written++] = '-';
    }

    for (position = 0U; position < total; position++) {
        /*
         * Where this output position falls in the padded digit string. The
         * fractional part is the low `decimals` digits, right aligned, so its
         * first output position maps to the low digit's left neighbour.
         */
        size_t from_end = total - position - 1U;
        char digit;

        if (position == integer_digits && decimals != 0U) {
            if (written + 1U >= out_size) {
                return terminate(out, out_size, written);
            }
            out[written++] = '.';
        }

        if (from_end < digits) {
            digit = scratch[digits - 1U - from_end];
        } else {
            /* Left padding, only ever a leading zero before the point. */
            digit = '0';
        }

        if (written + 1U >= out_size) {
            return terminate(out, out_size, written);
        }
        out[written++] = digit;
    }

    return terminate(out, out_size, written);
}

size_t osd_format_int(char *out, size_t out_size, long value)
{
    char scratch[24];
    size_t written = 0U;
    unsigned long long magnitude;
    size_t digits;
    size_t i;

    if (out == NULL || out_size == 0U) {
        return 0U;
    }

    /*
     * Negate through unsigned so LONG_MIN does not overflow. On a 32 bit
     * target LONG_MIN is representable but -LONG_MIN is not.
     */
    if (value < 0L) {
        magnitude = (unsigned long long)(-(long long)value);
    } else {
        magnitude = (unsigned long long)value;
    }

    digits = to_digits(magnitude, scratch);

    if (value < 0L) {
        if (written + 1U < out_size) {
            out[written++] = '-';
        } else {
            return terminate(out, out_size, written);
        }
    }

    for (i = 0U; i < digits; i++) {
        if (written + 1U < out_size) {
            out[written++] = scratch[i];
        } else {
            return terminate(out, out_size, written);
        }
    }

    return terminate(out, out_size, written);
}

size_t osd_format_int_explicit(char *out, size_t out_size, long value)
{
    size_t written;

    if (out == NULL || out_size == 0U) {
        return 0U;
    }

    if (value >= 0L) {
        if (out_size < 2U) {
            return terminate(out, out_size, 0U);
        }
        out[0] = '+';
        written = osd_format_int(out + 1, out_size - 1U, value);
        return written == 0U ? 0U : written + 1U;
    }

    return osd_format_int(out, out_size, value);
}

size_t osd_format_bytes(char *out, size_t out_size, uint64_t bytes)
{
    /*
     * Thresholds chosen so the integer part stays at most four digits. Using
     * 1000 rather than 1024 because these are rates and counters being read
     * by a human, not memory sizes, and a reader comparing against a datasheet
     * in Mbps expects the decimal sense.
     */
    static const char *const kSuffix[4] = { "B", "K", "M", "G" };
    unsigned unit = 0U;
    double value = (double)bytes;

    if (out == NULL || out_size == 0U) {
        return 0U;
    }

    while (value >= 1000.0 && unit < 3U) {
        value /= 1000.0;
        unit++;
    }

    /*
     * One decimal below the top two units, none for plain bytes or K. A
     * bitrate is the case that matters ("2.4M"), and it wants the decimal;
     * a byte count does not.
     */
    if (unit >= 2U) {
        size_t written = osd_format_fixed(out, out_size, (float)value, 1U);

        if (written == 0U || written + 2U > out_size) {
            return written;
        }

        out[written] = kSuffix[unit][0];
        out[written + 1U] = '\0';
        return written + 1U;
    }

    {
        size_t written = osd_format_int(out, out_size, (long)value);

        if (written == 0U || written + 2U > out_size) {
            return written;
        }

        out[written] = kSuffix[unit][0];
        out[written + 1U] = '\0';
        return written + 1U;
    }
}

size_t osd_format_duration(char *out, size_t out_size, uint64_t microseconds)
{
    uint64_t total_seconds = microseconds / 1000000ULL;
    uint64_t hours = total_seconds / 3600ULL;
    unsigned minutes = (unsigned)((total_seconds / 60ULL) % 60ULL);
    unsigned seconds = (unsigned)(total_seconds % 60ULL);
    char scratch[24];
    size_t written = 0U;
    size_t digits;
    size_t i;

    if (out == NULL || out_size == 0U) {
        return 0U;
    }

    digits = to_digits((unsigned long long)hours, scratch);

    for (i = 0U; i < digits; i++) {
        if (written + 1U >= out_size) {
            return terminate(out, out_size, written);
        }
        out[written++] = scratch[i];
    }

    /*
     * A fixed H:MM:SS shape means the field never changes width, which keeps
     * the whole overlay from twitching once an hour when the minutes roll
     * over. Hours are not zero padded because the value starts at 0 and "0:05:03"
     * reads better than "00:05:03" on a small overlay, but minutes and seconds
     * always are.
     */
    {
        static const char kColon = ':';

        if (written + 1U >= out_size) {
            return terminate(out, out_size, written);
        }
        out[written++] = kColon;

        out[written++] = (char)('0' + ((minutes / 10U) % 10U));
        if (written + 1U >= out_size) {
            return terminate(out, out_size, written);
        }
        out[written++] = (char)('0' + (minutes % 10U));

        if (written + 1U >= out_size) {
            return terminate(out, out_size, written);
        }
        out[written++] = kColon;

        out[written++] = (char)('0' + ((seconds / 10U) % 10U));
        if (written + 1U >= out_size) {
            return terminate(out, out_size, written);
        }
        out[written++] = (char)('0' + (seconds % 10U));
    }

    return terminate(out, out_size, written);
}
