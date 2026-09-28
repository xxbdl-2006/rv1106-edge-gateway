#ifndef OSD_FORMAT_H
#define OSD_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The small amount of number formatting the OSD needs, written out.
 *
 * WHY NOT sprintf AND FRIENDS. Three reasons, and only the first is about
 * the rootfs.
 *
 * The buildroot image ships uClibc, which does have snprintf, so this could
 * technically be delegated. But the OSD calls it a few dozen times per frame
 * for values with a fixed, tiny shape - "12.3", "-4.1", "1.00" - and a
 * general printf with float support pulls in the soft-float division and
 * rounding paths plus, in some builds, a nontrivial slice of libc's code
 * size for a feature this program uses none of.
 *
 * The second reason is that %f is not specified to round the way anyone
 * expects at the boundary, and the exact output of printf for -0.0, for
 * values that round up to a carry, and for the 0.05 case can differ between
 * libcs. Overlay text that changes between the board and the host is a bad
 * foundation for a test that compares rendered bytes, and the whole point of
 * the layering here is that the OSD can be verified on the host.
 *
 * The third is that the behaviour I actually want is not printf's: a resting
 * gyro reading of -0.02 should print as "0.0" and not as "-0.0", and a value
 * that overflows the field should be clamped to the field rather than
 * widening it and pushing the panel out of shape. Both are policy, and
 * wrapping printf in enough fixups to get them is more code than this file.
 *
 * Everything here is integer arithmetic on the scaled value. The rounding is
 * half-away-from-zero, which is what a reader comparing two readings by eye
 * expects, and it is the same on every platform because there is no float in
 * the rounding path at all.
 */

/*
 * Format a float with a fixed number of decimals into `out`.
 *
 * `decimals` is 0..3. Values are rounded half-away-from-zero. A value that
 * rounds to zero does not print a minus sign, so tiny negative noise reads
 * as "0.0" rather than "-0.0".
 *
 * Returns the number of characters written, or 0 if the arguments are bad.
 * `out` is always NUL terminated when the function returns non-zero, and
 * truncated (with a NUL) when the buffer is too small.
 */
size_t osd_format_fixed(char *out, size_t out_size, float value,
                        unsigned decimals);

/*
 * Format a signed integer. Uses a leading '-' for negatives and no '+' for
 * positives; use osd_format_signed_explicit() when the sign carries meaning.
 */
size_t osd_format_int(char *out, size_t out_size, long value);

/*
 * Format an integer with an explicit sign, so a pitch of +12.3 and one of
 * -12.3 are the same width. Alignment matters on an overlay that redraws
 * every frame: a value that gains a character would otherwise shove the rest
 * of the line sideways.
 */
size_t osd_format_int_explicit(char *out, size_t out_size, long value);

/*
 * Format a byte count as a rate or size with a unit suffix.
 *
 * Picks B, K, M or G so the string stays short, and one decimal below G.
 * Used for the bitrate and for the frame counters.
 */
size_t osd_format_bytes(char *out, size_t out_size, uint64_t bytes);

/*
 * Format a duration given in microseconds as H:MM:SS, for the run timer.
 * Hours are not wrapped and not zero padded; the rest are.
 */
size_t osd_format_duration(char *out, size_t out_size, uint64_t microseconds);

#endif /* OSD_FORMAT_H */
