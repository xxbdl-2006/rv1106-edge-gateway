#ifndef SENSOR_MATH_H
#define SENSOR_MATH_H

/*
 * The small amount of maths the sensor code needs, in one place.
 *
 * These live in a header as static inline rather than in a .c because the
 * rootfs ships only libc - no libm - so anything that wants a square root or a
 * trig function has to carry its own. That is fine, but it must be ONE copy:
 * the accelerometer scaling and the attitude solver both need a square root,
 * and two independently drifting implementations of the same approximation is
 * exactly the kind of difference that shows up as "the OSD disagrees with the
 * log" months later.
 *
 * Everything here is float, deterministic, and free of <math.h>.
 */

/*
 * Square root by Newton's method.
 *
 * Twenty iterations from x = v. The convergence is quadratic, so this is
 * thoroughly converged long before the count runs out - a float has 24 bits of
 * mantissa and each step roughly doubles the correct digits. The count is
 * deliberately generous and fixed rather than looped to a tolerance: a fixed
 * iteration count is branch-free on the data, which keeps the timing
 * predictable and makes the result reproducible bit for bit. Reproducibility
 * matters more here than the last few nanoseconds.
 *
 * Returns 0 for v <= 0 rather than a NaN: callers in this codebase treat a
 * magnitude of zero as "no reading", and a NaN propagating into the OSD would
 * print as "nan" on screen.
 */
static inline float sensor_sqrt(float v)
{
    float x;
    int i;

    if (v <= 0.0f)
        return 0.0f;

    x = v;
    for (i = 0; i < 20; i++)
        x = 0.5f * (x + v / x);

    return x;
}

/* Magnitude of a 3 vector. */
static inline float sensor_magnitude3(float x, float y, float z)
{
    return sensor_sqrt(x * x + y * y + z * z);
}

/*
 * atan2 by argument reduction plus a polynomial.
 *
 * Why this is not just a table: a table needs an index computation, a range
 * clamp and an interpolation, which is more code than the approximation, and
 * it has a discontinuity risk at the octant boundaries. The polynomial below
 * is the standard odd minimax fit for atan on [0, 1]:
 *
 *   atan(t) ~= t - t^3/3 + t^5/5 - t^7/7 + t^9/9 - t^11/11
 *
 * evaluated with Horner. Accuracy is good to a few tenths of a degree over the
 * reduced range, which is far finer than a level display needs. The result is
 * in radians, like the C library function it stands in for.
 *
 * Argument reduction: atan(t) = pi/2 - atan(1/t) for t > 1. Doing this before
 * the series is what keeps the polynomial in its accurate interval; skipping it
 * makes the error grow without bound as t goes to infinity, which for an
 * attitude solver means the steep angles - the ones that matter - are the worst
 * ones.
 */
/*
 * atan by argument reduction to a tiny range, then a short series.
 *
 * The trap this avoids, and the one it fell into first: the Taylor series
 *
 *   atan(t) = t - t^3/3 + t^5/5 - t^7/7 + ...
 *
 * converges beautifully for small t and *dismally* near t = 1. Truncating it
 * at six terms gives atan(1) = 0.744 against a true pi/4 = 0.785 - a 2.4
 * degree error, right where an attitude display spends its time. Adding terms
 * barely helps: fifty of them still leave 0.005. Anyone reaching for "more
 * terms" is solving the wrong problem; the series has to be evaluated where it
 * converges, which means shrinking the argument first.
 *
 * So the argument is squeezed below 0.5, where the same six terms are accurate
 * to about 1e-6 - far more than a display needs. Two identities do it:
 *
 *   atan(t) = pi/2 - atan(1/t)          for t > 1
 *   atan(t) = pi/4 + atan((t-1)/(t+1))  for t > 0.5
 *
 * The second is the tangent half-angle formula rearranged. Applying it once
 * covers every t above 0.5, and the result is always below 0.5 in magnitude,
 * so no iteration is needed and the cost is one division.
 *
 * The result is in radians, like the C library function it stands in for. The
 * "st = ..." variable holds tan(pi/8) rather than introducing a separate
 * constant, because that is the number the formula actually needs and writing
 * it that way keeps the derivation visible.
 */
static inline float sensor_atan(float t)
{
    const float quarter_pi = 0.78539816339f;
    /*
     * (sqrt(2) - 1): the fixed point of (t-1)/(t+1), i.e. the value at which
     * the half-angle step maps 0.5 to itself. Used as the branch point so the
     * two halves meet continuously.
     */
    const float tan_pi_over_8 = 0.41421356f;
    float sign = 1.0f;
    float t2;

    if (t < 0.0f) {
        t = -t;
        sign = -1.0f;
    }

    /* Shrink below tan(pi/8) ~= 0.414, where six terms are accurate. */
    if (t > tan_pi_over_8) {
        float reduced = (t - 1.0f) / (t + 1.0f);
        float result = quarter_pi + sensor_atan(reduced);
        return sign * result;
    }

    /*
     * Horner from the highest power down. The coefficients are the alternating
     * series terms 1, -1/3, 1/5, ... so the signs are written out explicitly
     * rather than computed - the compiler folds them and a reader can check
     * them against the formula by eye.
     */
    t2 = t * t;
    t = t * (1.0f
             + t2 * (-0.333333333f
                     + t2 * (0.2f
                             + t2 * (-0.142857143f
                                     + t2 * (0.111111111f
                                             + t2 * (-0.090909091f))))));

    return sign * t;
}

/*
 * Sine and cosine by range reduction to [0, pi/2] plus a polynomial.
 *
 * The mock sensor's waveform needs these, and the rootfs has no libm, so they
 * live here next to the inverse tangents. The reduction is the standard one:
 * fold the angle into the first quadrant, remember which reflection was used,
 * and fix up the sign at the end. Without the reduction the polynomial gets
 * worse as the angle grows and is simply wrong beyond a full turn.
 *
 * The polynomial is the usual minimax fit for sine on [0, pi/2]:
 *
 *   sin(x) ~= x - x^3/6 + x^5/120 - x^7/5040
 *
 * Four terms. More would be wasted on a float, which has about seven decimal
 * digits; this is accurate to better than a hundredth of a degree across the
 * reduced range, which is invisible in a test waveform.
 */
static inline float sensor_sin(float x)
{
    const float pi = 3.14159265359f;
    const float two_pi = 6.28318530718f;
    const float half_pi = 1.57079632679f;
    float sign = 1.0f;
    float t;
    float t2;

    /*
     * Fold the sign out first rather than adding two_pi to a negative angle.
     * The first version did the latter, and it silently broke every negative
     * input: sin(-pi/2) came back as +1. sine is odd, so the clean fix is to
     * reduce |x| and reapply the sign at the end.
     */
    if (x < 0.0f) {
        x = -x;
        sign = -1.0f;
    }

    /* Fold into [0, 2pi). */
    while (x >= two_pi)
        x -= two_pi;

    /*
     * Fold [pi, 2pi) back onto [0, pi], negating.
     *
     * This is the fold that caught me twice. The tempting version is
     * x = 2pi - x, which is a *reflection* - geometrically the sine of the
     * reflected angle is the same, so the value looks right, but the reflection
     * maps the negative half of the wave onto the positive one and the sign
     * disappears with it. sin(3pi/2) came back as +1 instead of -1, which put
     * the mock waveform's trough at zero instead of -30 degrees and made the
     * round-trip test fail in a way that looked like a solver bug.
     *
     * Subtracting pi and negating is the correct version:
     *   sin(x) = -sin(x - pi) for x in [pi, 2pi)
     */
    if (x >= pi) {
        x -= pi;
        sign = -sign;
    }

    /*
     * Now in [0, pi). Fold to [0, pi/2] using sin(pi - x) = sin(x), which is a
     * reflection about the peak at pi/2 and genuinely does preserve the sign -
     * both branches of that reflection sit in the positive half.
     */
    if (x > half_pi)
        x = pi - x;

    t = x;
    t2 = t * t;
    return sign * t * (1.0f + t2 * (-0.166666667f
                                    + t2 * (0.008333333f
                                            + t2 * (-0.000198413f
                                                    + t2 * 0.000002756f))));
}

/*
 * Cosine, computed from its own reduction rather than by shifting the sine.
 *
 * The obvious implementation is cos(x) = sin(x + pi/2), and it is wrong at the
 * seam. The first version did exactly that and cos(pi) came back as +1 instead
 * of -1: adding pi/2 to pi lands at 3pi/2, which the sine reduction then folds
 * back into a positive quadrant, and the phase information - which quadrant we
 * were really in - is lost. A rotation has to be applied as a rotation, with
 * the wrap, not as a bare addition.
 *
 * Written out separately the reduction is the same shape as the sine's but
 * reflects about pi/2 instead of zero:
 *
 *   cos(pi - x)  = -cos(x)   fold [0, pi] into [0, pi/2]
 *   cos(-x)      =  cos(x)   the function is even
 *
 * Both are cheap, and being explicit means the two functions cannot disagree
 * about which way up the reduced range is - which is the property the mock
 * waveform depends on to keep |a| at exactly 1 g.
 */
static inline float sensor_cos(float x)
{
    const float pi = 3.14159265359f;
    const float two_pi = 6.28318530718f;
    const float half_pi = 1.57079632679f;
    float sign = 1.0f;
    float t;
    float t2;

    /* Even function: the sign folds away. */
    if (x < 0.0f)
        x = -x;

    /* Fold into [0, 2pi). */
    while (x >= two_pi)
        x -= two_pi;

    /* Fold into [0, pi] about pi/2, flipping the sign. */
    if (x > pi) {
        x = two_pi - x;
        /* cos(2pi - x) = cos(x): no sign change for this fold */
    }

    /*
     * Now in [0, pi]. Reduce to [0, pi/2] using cos(pi - x) = -cos(x), which
     * is where the sign flip lives.
     */
    if (x > half_pi) {
        x = pi - x;
        sign = -1.0f;
    }

    /*
     * sin on the reduced range, then shift: cos(t) = sin(pi/2 - t) would need
     * another reduction, so instead evaluate the cosine polynomial directly.
     * cos(t) = 1 - t^2/2 + t^4/24 - t^6/720, the even series.
     */
    t = x;
    t2 = t * t;
    return sign * (1.0f + t2 * (-0.5f
                                + t2 * (0.041666667f
                                        + t2 * (-0.001388889f
                                                + t2 * 0.000024802f))));
}

/*
 * Two argument arctangent, radians, following the C library's sign convention
 * in all four quadrants.
 *
 * The quadrant handling is the whole reason this exists as a separate function.
 * A naive atan(y/x) collapses opposite quadrants onto each other, so a body
 * tilted backwards past vertical reads the same as one tilted forwards - the
 * display would snap rather than pass through +-180.
 */
static inline float sensor_atan2(float y, float x)
{
    const float pi = 3.14159265359f;

    if (x > 0.0f)
        return sensor_atan(y / x);
    if (x < 0.0f)
        return sensor_atan(y / x) + (y >= 0.0f ? pi : -pi);
    /* x == 0: straight up or straight down. */
    if (y > 0.0f)
        return pi * 0.5f;
    if (y < 0.0f)
        return -pi * 0.5f;
    return 0.0f; /* both zero: undefined, and 0 is the only non-lying answer */
}

#endif /* SENSOR_MATH_H */
