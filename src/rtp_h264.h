#ifndef RTP_H264_H
#define RTP_H264_H

#include <stddef.h>
#include <stdint.h>

/*
 * H.264 (RFC 6184) packetizer: Single NAL unit and FU-A fragmentation.
 *
 * Pure buffer in / callbacks out. No sockets, no threads, no allocation, so it
 * can be unit tested on the host.
 *
 * Input is one access unit in Annex-B form (start code delimited NAL units),
 * which is exactly what the Rockit encoder produces.
 */

#define RTP_HEADER_BYTES 12U
#define RTP_H264_MAX_PACKET 1400U
#define RTP_H264_PAYLOAD_TYPE 96U
#define RTP_H264_CLOCK_RATE 90000U

#define RTP_H264_MAX_SPS_BYTES 64U
#define RTP_H264_MAX_PPS_BYTES 64U
#define RTP_H264_MAX_NALS 24U

struct rtp_h264 {
    uint16_t sequence;
    uint32_t ssrc;
    uint32_t timestamp;
    uint32_t step;
    size_t max_packet;
    uint8_t sps[RTP_H264_MAX_SPS_BYTES];
    size_t sps_length;
    uint8_t pps[RTP_H264_MAX_PPS_BYTES];
    size_t pps_length;
    int parameters_cached;
};

/* Called once per RTP packet. <packet> points at the 12 byte RTP header. */
typedef int (*rtp_h264_emit)(void *user, const void *packet, size_t length);

void rtp_h264_init(struct rtp_h264 *rtp, uint32_t fps, uint32_t ssrc);

/*
 * Start a new observation for a client that just joined. Sequence keeps going
 * (it is per stream, not per client) but SPS/PPS must be sent again.
 */
void rtp_h264_new_client(struct rtp_h264 *rtp);

/*
 * Packetize one access unit.
 *
 * Returns the number of RTP packets emitted, 0 when the input contained no
 * Annex-B NAL unit at all, or -1 on a fatal emit failure. A zero result is
 * worth surfacing: it means the encoder output is not Annex-B and the client
 * would otherwise see an empty stream with no explanation.
 */
int rtp_h264_packetize(struct rtp_h264 *rtp,
                       const uint8_t *frame,
                       size_t length,
                       rtp_h264_emit emit,
                       void *user);

#endif
