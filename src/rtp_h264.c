#include "rtp_h264.h"

#include <string.h>

#include "h264_util.h"

#define NAL_TYPE_SPS 7U
#define NAL_TYPE_PPS 8U
#define NAL_TYPE_IDR 5U
#define FU_A_TYPE 28U

struct nal_view {
    const uint8_t *data;
    size_t length;
};

static size_t find_start_code(const uint8_t *data, size_t length, size_t from)
{
    size_t index;

    for (index = from; index + 2U < length; index++) {
        if (data[index] == 0U && data[index + 1U] == 0U &&
            data[index + 2U] == 1U) {
            return index;
        }
    }

    return (size_t)-1;
}

/*
 * Split an Annex-B access unit into NAL units.
 *
 * Returns how many were found, saturating at <max>. A 4 byte start code is
 * matched through its trailing 3 byte form, so the NAL header always sits
 * three bytes after the reported index.
 */
static size_t collect_nals(const uint8_t *data,
                           size_t length,
                           struct nal_view *nals,
                           size_t max)
{
    size_t count = 0U;
    size_t position = 0U;

    if (data == NULL || length < 4U || max == 0U) {
        return 0U;
    }

    for (;;) {
        size_t start;
        size_t next;
        size_t end;

        if (count >= max) {
            break;
        }

        start = find_start_code(data, length, position);
        if (start == (size_t)-1) {
            break;
        }

        start += 3U;
        if (start >= length) {
            break;
        }

        next = find_start_code(data, length, start);
        if (next == (size_t)-1) {
            end = length;
        } else {
            /*
             * Any zero bytes directly in front of a start code belong to it
             * (trailing_zero_8bits), not to the NAL unit we just finished.
             * Without this, every NAL ended with a stray 0x00 byte that came
             * from a 4 byte start code, which corrupts SPS and PPS.
             */
            while (next > start && data[next - 1U] == 0U) {
                next--;
            }
            end = next;
        }

        nals[count].data = data + start;
        nals[count].length = end - start;
        count++;

        if (next == (size_t)-1) {
            break;
        }
        position = next;
    }

    return count;
}

static void cache_parameters(struct rtp_h264 *rtp,
                             const struct nal_view *nals,
                             size_t count,
                             int *frame_has_sps,
                             int *frame_has_pps)
{
    size_t index;

    for (index = 0U; index < count; index++) {
        unsigned type;

        if (nals[index].length == 0U) {
            continue;
        }

        type = (unsigned)(nals[index].data[0U] & 0x1FU);

        if (type == NAL_TYPE_SPS) {
            *frame_has_sps = 1;
            if (nals[index].length <= RTP_H264_MAX_SPS_BYTES) {
                memcpy(rtp->sps, nals[index].data, nals[index].length);
                rtp->sps_length = nals[index].length;
                rtp->parameters_cached = 1;
            }
        } else if (type == NAL_TYPE_PPS) {
            *frame_has_pps = 1;
            if (nals[index].length <= RTP_H264_MAX_PPS_BYTES) {
                memcpy(rtp->pps, nals[index].data, nals[index].length);
                rtp->pps_length = nals[index].length;
            }
        }
    }
}

static void write_rtp_header(uint8_t *packet,
                             int marker,
                             uint16_t sequence,
                             uint32_t timestamp,
                             uint32_t ssrc)
{
    packet[0] = 0x80U;
    packet[1] = (uint8_t)((marker ? 0x80U : 0x00U) |
                          (RTP_H264_PAYLOAD_TYPE & 0x7FU));
    packet[2] = (uint8_t)((sequence >> 8) & 0xFFU);
    packet[3] = (uint8_t)(sequence & 0xFFU);
    packet[4] = (uint8_t)((timestamp >> 24) & 0xFFU);
    packet[5] = (uint8_t)((timestamp >> 16) & 0xFFU);
    packet[6] = (uint8_t)((timestamp >> 8) & 0xFFU);
    packet[7] = (uint8_t)(timestamp & 0xFFU);
    packet[8] = (uint8_t)((ssrc >> 24) & 0xFFU);
    packet[9] = (uint8_t)((ssrc >> 16) & 0xFFU);
    packet[10] = (uint8_t)((ssrc >> 8) & 0xFFU);
    packet[11] = (uint8_t)(ssrc & 0xFFU);
}

/*
 * Send one NAL unit, either as a single packet or split into FU-A fragments.
 * The marker bit lands on whatever packet ends this NAL unit.
 * Returns the number of packets emitted, or -1 on failure.
 */
static int emit_nal(struct rtp_h264 *rtp,
                    const uint8_t *nal,
                    size_t nal_length,
                    uint32_t timestamp,
                    int marker,
                    rtp_h264_emit emit,
                    void *user)
{
    uint8_t packet[RTP_H264_MAX_PACKET];
    size_t payload_max;
    size_t offset;
    size_t chunk_max;
    size_t chunk;
    size_t emitted = 0U;
    int first;

    if (nal == NULL || nal_length == 0U || emit == NULL) {
        return -1;
    }

    payload_max = rtp->max_packet - RTP_HEADER_BYTES;

    if (nal_length <= payload_max) {
        if (RTP_HEADER_BYTES + nal_length > rtp->max_packet) {
            return -1;
        }

        write_rtp_header(packet, marker, rtp->sequence, timestamp, rtp->ssrc);
        memcpy(packet + RTP_HEADER_BYTES, nal, nal_length);
        rtp->sequence = (uint16_t)(rtp->sequence + 1U);

        return emit(user, packet, RTP_HEADER_BYTES + nal_length) != 0 ? -1 : 1;
    }

    if (payload_max <= 2U) {
        return -1;
    }

    chunk_max = payload_max - 2U;
    offset = 1U;
    first = 1;

    while (offset < nal_length) {
        int last;
        size_t total;

        chunk = nal_length - offset;
        if (chunk > chunk_max) {
            chunk = chunk_max;
        }

        last = (offset + chunk >= nal_length) ? 1 : 0;

        write_rtp_header(packet,
                         (marker && last) ? 1 : 0,
                         rtp->sequence,
                         timestamp,
                         rtp->ssrc);

        packet[RTP_HEADER_BYTES] =
            (uint8_t)((nal[0U] & 0xE0U) | FU_A_TYPE);
        packet[RTP_HEADER_BYTES + 1U] =
            (uint8_t)((nal[0U] & 0x1FU) | (first ? 0x80U : 0x00U) |
                      (last ? 0x40U : 0x00U));

        memcpy(packet + RTP_HEADER_BYTES + 2U, nal + offset, chunk);

        total = RTP_HEADER_BYTES + 2U + chunk;
        rtp->sequence = (uint16_t)(rtp->sequence + 1U);

        if (emit(user, packet, total) != 0) {
            return -1;
        }

        emitted++;
        offset += chunk;
        first = 0;
    }

    return (int)emitted;
}

void rtp_h264_init(struct rtp_h264 *rtp, uint32_t fps, uint32_t ssrc)
{
    if (rtp == NULL) {
        return;
    }

    memset(rtp, 0, sizeof(*rtp));

    rtp->ssrc = (ssrc == 0U) ? 0x12345678U : ssrc;
    rtp->max_packet = RTP_H264_MAX_PACKET;
    rtp->step = (fps == 0U) ? 1U : (RTP_H264_CLOCK_RATE / fps);
    rtp->sequence = (uint16_t)(rtp->ssrc & 0xFFFFU);
}

void rtp_h264_new_client(struct rtp_h264 *rtp)
{
    if (rtp == NULL) {
        return;
    }

    /* Start a clean timeline so a late client does not see a huge RTP time. */
    rtp->timestamp = 0U;
}

int rtp_h264_packetize(struct rtp_h264 *rtp,
                       const uint8_t *frame,
                       size_t length,
                       rtp_h264_emit emit,
                       void *user)
{
    struct nal_view nals[RTP_H264_MAX_NALS];
    struct nal_view pending[RTP_H264_MAX_NALS + 2U];
    size_t count;
    size_t pending_count = 0U;
    size_t index;
    uint32_t timestamp;
    int packets = 0;
    int frame_has_sps = 0;
    int frame_has_pps = 0;
    int is_key;

    if (rtp == NULL || frame == NULL || emit == NULL) {
        return -1;
    }

    count = collect_nals(frame, length, nals, RTP_H264_MAX_NALS);
    if (count == 0U) {
        return 0;
    }

    cache_parameters(rtp, nals, count, &frame_has_sps, &frame_has_pps);

    is_key = (h264_frame_flags(frame, length) & H264_FLAG_KEY) != 0U;

    /*
     * A decoder that joined mid stream needs the parameter sets, so hand them
     * out again before every IDR that does not already carry them.
     */
    if (is_key && rtp->parameters_cached != 0 &&
        (frame_has_sps == 0 || frame_has_pps == 0)) {
        if (rtp->sps_length > 0U) {
            pending[pending_count].data = rtp->sps;
            pending[pending_count].length = rtp->sps_length;
            pending_count++;
        }
        if (rtp->pps_length > 0U) {
            pending[pending_count].data = rtp->pps;
            pending[pending_count].length = rtp->pps_length;
            pending_count++;
        }
    }

    for (index = 0U; index < count; index++) {
        pending[pending_count] = nals[index];
        pending_count++;
    }

    timestamp = rtp->timestamp;

    for (index = 0U; index < pending_count; index++) {
        int marker = (index + 1U == pending_count) ? 1 : 0;
        int emitted;

        emitted = emit_nal(rtp,
                           pending[index].data,
                           pending[index].length,
                           timestamp,
                           marker,
                           emit,
                           user);
        if (emitted < 0) {
            return -1;
        }

        packets += emitted;
    }

    /*
     * Timestamp advances once per access unit, by a fixed 1/fps step. Using
     * wall clock differences here would make VLC see a jittery frame rate.
     */
    rtp->timestamp += rtp->step;

    return packets;
}
