#include "h264_util.h"

unsigned h264_frame_flags(const uint8_t *data, size_t length)
{
    size_t index;

    if (data == NULL || length < 4U) {
        return 0U;
    }

    /*
     * Scan for the first byte of every Annex-B start code. A 4 byte start code
     * (00 00 00 01) is matched by its trailing 3 byte form at index + 1, so the
     * NAL header always sits three bytes past the match.
     */
    for (index = 0U; index + 3U < length; index++) {
        if (data[index] == 0U && data[index + 1U] == 0U &&
            data[index + 2U] == 1U) {
            unsigned type = (unsigned)(data[index + 3U] & 0x1FU);

            if (type == 5U) {
                return H264_FLAG_KEY;
            }

            index += 2U;
        }
    }

    return 0U;
}
