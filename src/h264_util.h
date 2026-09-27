#ifndef H264_UTIL_H
#define H264_UTIL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Annex-B helpers shared by the packet queue (to recognise IDR frames) and,
 * later, by the RTP packetizer (to slice NAL units).
 */

/* The frame contains at least one NAL unit of type 5 (coded slice, IDR). */
#define H264_FLAG_KEY 0x1U

/*
 * Inspect a complete, contiguous Annex-B access unit.
 *
 * Returns a bit mask of H264_FLAG_*. Returns 0 for input that is too short to
 * contain a start code, which keeps callers from having to special case it.
 */
unsigned h264_frame_flags(const uint8_t *data, size_t length);

#endif
