/*
 * Host side self test for the RTSP stack pieces that do not need sockets:
 * the H.264 RTP packetizer and the RTSP protocol layer.
 *
 *     make test
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtp_h264.h"
#include "rtsp_proto.h"

static int g_checks;
static int g_failures;

#define CHECK(condition, format, ...)                                      \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(condition)) {                                                \
            g_failures++;                                                  \
            printf("FAIL line %d: " format "\n", __LINE__, ##__VA_ARGS__);   \
        }                                                                  \
    } while (0)

#define CAPTURE_MAX 64

struct capture {
    uint8_t data[CAPTURE_MAX][RTP_H264_MAX_PACKET];
    size_t lengths[CAPTURE_MAX];
    size_t count;
    int overflow;
};

static int capture_emit(void *user, const void *packet, size_t length)
{
    struct capture *target = user;

    if (target->count >= CAPTURE_MAX) {
        target->overflow = 1;
        return 0;
    }
    if (length > RTP_H264_MAX_PACKET) {
        target->overflow = 1;
        return -1;
    }

    memcpy(target->data[target->count], packet, length);
    target->lengths[target->count] = length;
    target->count++;

    return 0;
}

static const uint8_t g_sps[] = {0x67, 0x42, 0x80, 0x1E, 0xD9, 0x00, 0xA0};
static const uint8_t g_pps[] = {0x68, 0xCE, 0x3C, 0x80};
static const uint8_t g_slice[] = {0x65, 0x88, 0x84, 0x00, 0x33, 0xFF};

static size_t build_annex_b(uint8_t *out,
                            size_t size,
                            const uint8_t **nals,
                            const size_t *lengths,
                            size_t count)
{
    size_t written = 0U;
    size_t index;

    for (index = 0U; index < count; index++) {
        const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};

        if (written + 4U + lengths[index] > size) {
            break;
        }
        memcpy(out + written, start_code, sizeof(start_code));
        written += sizeof(start_code);
        memcpy(out + written, nals[index], lengths[index]);
        written += lengths[index];
    }

    return written;
}

static void test_single_nal_units(void)
{
    uint8_t frame[256];
    const uint8_t *nals[3];
    size_t lengths[3];
    size_t frame_length;
    struct rtp_h264 rtp;
    struct capture capture;
    unsigned index;

    nals[0] = g_sps;
    lengths[0] = sizeof(g_sps);
    nals[1] = g_pps;
    lengths[1] = sizeof(g_pps);
    nals[2] = g_slice;
    lengths[2] = sizeof(g_slice);

    frame_length = build_annex_b(frame, sizeof(frame), nals, lengths, 3U);

    rtp_h264_init(&rtp, 30U, 0x11223344U);

    memset(&capture, 0, sizeof(capture));
    CHECK(rtp_h264_packetize(&rtp, frame, frame_length, capture_emit,
                             &capture) == 3,
          "packetize must report 3 packets: %d", 1);
    CHECK(capture.count == 3U, "expected 3 packets, got %zu", capture.count);

    /* One access unit means one RTP timestamp for every packet. */
    CHECK((capture.data[0][1] & 0x7FU) == RTP_H264_PAYLOAD_TYPE,
          "wrong payload type %u", capture.data[0][1] & 0x7FU);
    CHECK(capture.data[0][0] == 0x80U, "wrong version byte: %d", 1);
    CHECK((capture.data[0][1] & 0x80U) == 0U,
          "marker must be clear on the first packet: %d", 1);
    CHECK((capture.data[2][1] & 0x80U) != 0U,
          "marker must be set on the last packet: %d", 1);

    for (index = 0U; index < 3U; index++) {
        uint32_t first = ((uint32_t)capture.data[0][4] << 24) |
                         ((uint32_t)capture.data[0][5] << 16) |
                         ((uint32_t)capture.data[0][6] << 8) |
                         (uint32_t)capture.data[0][7];
        uint32_t current = ((uint32_t)capture.data[index][4] << 24) |
                           ((uint32_t)capture.data[index][5] << 16) |
                           ((uint32_t)capture.data[index][6] << 8) |
                           (uint32_t)capture.data[index][7];

        CHECK(current == first, "packet %u timestamp drifted", index);
    }

    /* Sequence numbers advance by one. */
    for (index = 1U; index < capture.count; index++) {
        uint16_t previous = (uint16_t)(((uint16_t)capture.data[index - 1U][2] << 8) |
                                       capture.data[index - 1U][3]);
        uint16_t current = (uint16_t)(((uint16_t)capture.data[index][2] << 8) |
                                      capture.data[index][3]);

        CHECK(current == (uint16_t)(previous + 1U),
              "sequence gap before packet %u", index);
    }

    CHECK(memcmp(capture.data[0] + RTP_HEADER_BYTES, g_sps,
                 sizeof(g_sps)) == 0,
          "first packet must carry the SPS: %d", 1);
    CHECK(capture.lengths[0] == RTP_HEADER_BYTES + sizeof(g_sps),
          "wrong packet length %zu", capture.lengths[0]);

    /* Each frame advances the timeline by exactly 90000 / fps. */
    rtp_h264_packetize(&rtp, frame, frame_length, capture_emit, &capture);
    CHECK(rtp.timestamp == 2U * 3000U,
          "timestamp step wrong, got %u", rtp.timestamp);
}

static void test_fu_a_fragmentation(void)
{
    uint8_t big_nal[4000];
    uint8_t frame[4200];
    const uint8_t *nals[1];
    size_t lengths[1];
    size_t frame_length;
    struct rtp_h264 rtp;
    struct capture capture;
    uint8_t reconstructed[4200];
    size_t reconstructed_length = 0U;
    size_t index;
    size_t expected_fragments;
    size_t chunk_max;

    big_nal[0] = 0x65;
    for (index = 1U; index < sizeof(big_nal); index++) {
        big_nal[index] = (uint8_t)(index * 7U + 3U);
    }

    nals[0] = big_nal;
    lengths[0] = sizeof(big_nal);
    frame_length = build_annex_b(frame, sizeof(frame), nals, lengths, 1U);

    rtp_h264_init(&rtp, 30U, 0xAABBCCDDU);

    chunk_max = RTP_H264_MAX_PACKET - RTP_HEADER_BYTES - 2U;
    expected_fragments = (sizeof(big_nal) - 1U + chunk_max - 1U) / chunk_max;

    memset(&capture, 0, sizeof(capture));
    CHECK(rtp_h264_packetize(&rtp, frame, frame_length, capture_emit,
                             &capture) == (int)expected_fragments,
          "packetize must report %zu fragments: %d", expected_fragments, 1);

    CHECK(capture.count == expected_fragments,
          "expected %zu fragments, got %zu", expected_fragments,
          capture.count);
    CHECK(capture.overflow == 0, "capture overflowed: %d", 1);

    for (index = 0U; index < capture.count; index++) {
        const uint8_t *payload = capture.data[index] + RTP_HEADER_BYTES;
        size_t payload_length = capture.lengths[index] - RTP_HEADER_BYTES;
        int start;
        int end;

        CHECK(capture.lengths[index] <= RTP_H264_MAX_PACKET,
              "fragment %zu exceeds the MTU", index);
        CHECK((payload[0] & 0x1FU) == 28U,
              "fragment %zu is not FU-A", index);

        start = (payload[1] & 0x80U) != 0U ? 1 : 0;
        end = (payload[1] & 0x40U) != 0U ? 1 : 0;

        if (index == 0U) {
            uint8_t header = (uint8_t)((payload[0] & 0xE0U) |
                                       (payload[1] & 0x1FU));

            CHECK(start == 1, "first fragment must set S: %d", 1);
            CHECK(header == big_nal[0],
                  "reconstructed NAL header 0x%02X differs from 0x%02X",
                  header, big_nal[0]);
            reconstructed[0] = header;
            reconstructed_length = 1U;
        } else {
            CHECK(start == 0, "only the first fragment may set S: %d", 1);
        }

        if (index + 1U == capture.count) {
            CHECK(end == 1, "last fragment must set E: %d", 1);
            CHECK((capture.data[index][1] & 0x80U) != 0U,
                  "marker must be set on the final fragment: %d", 1);
        } else {
            CHECK(end == 0, "only the last fragment may set E: %d", 1);
        }

        memcpy(reconstructed + reconstructed_length, payload + 2U,
               payload_length - 2U);
        reconstructed_length += payload_length - 2U;
    }

    CHECK(reconstructed_length == sizeof(big_nal),
          "reassembled %zu bytes, expected %zu",
          reconstructed_length, sizeof(big_nal));
    CHECK(memcmp(reconstructed, big_nal, sizeof(big_nal)) == 0,
          "reassembled NAL does not match the original: %d", 1);
}

static void test_parameter_set_injection(void)
{
    uint8_t full_frame[256];
    uint8_t slice_only[64];
    const uint8_t *nals[3];
    size_t lengths[3];
    size_t full_length;
    size_t slice_length;
    struct rtp_h264 rtp;
    struct capture capture;

    nals[0] = g_sps;
    lengths[0] = sizeof(g_sps);
    nals[1] = g_pps;
    lengths[1] = sizeof(g_pps);
    nals[2] = g_slice;
    lengths[2] = sizeof(g_slice);
    full_length = build_annex_b(full_frame, sizeof(full_frame), nals, lengths,
                                3U);

    nals[0] = g_slice;
    lengths[0] = sizeof(g_slice);
    slice_length = build_annex_b(slice_only, sizeof(slice_only), nals, lengths,
                                 1U);

    rtp_h264_init(&rtp, 30U, 0U);

    /* Teach the packetizer the parameter sets first. */
    CHECK(rtp_h264_packetize(&rtp, full_frame, full_length, NULL, NULL) == -1,
          "packetize must fail without a callback: %d", 1);

    memset(&capture, 0, sizeof(capture));
    rtp_h264_packetize(&rtp, full_frame, full_length, capture_emit, &capture);
    CHECK(capture.count == 3U,
          "frame already carries its own sets, got %zu packets",
          capture.count);
    CHECK(capture.data[0][RTP_HEADER_BYTES] == 0x67U,
          "unexpected first NAL 0x%02X", capture.data[0][RTP_HEADER_BYTES]);

    memset(&capture, 0, sizeof(capture));
    rtp_h264_packetize(&rtp, slice_only, slice_length, capture_emit, &capture);
    CHECK(capture.count == 3U,
          "cached sets must be injected before a bare IDR, got %zu packets",
          capture.count);
    CHECK(capture.data[0][RTP_HEADER_BYTES] == 0x67U,
          "injected SPS missing, first NAL 0x%02X",
          capture.data[0][RTP_HEADER_BYTES]);
    CHECK(capture.data[1][RTP_HEADER_BYTES] == 0x68U,
          "injected PPS missing, second NAL 0x%02X",
          capture.data[1][RTP_HEADER_BYTES]);
    CHECK(capture.data[2][RTP_HEADER_BYTES] == 0x65U,
          "slice must follow the parameter sets, got 0x%02X",
          capture.data[2][RTP_HEADER_BYTES]);

    rtp_h264_new_client(&rtp);
    CHECK(rtp.timestamp == 0U, "new client must restart the timeline: %d", 1);
}

static void test_base64(void)
{
    char output[64];
    size_t length;
    const uint8_t three[] = {0x00, 0x01, 0x02};
    const uint8_t one[] = {0x41};
    const uint8_t two[] = {0x41, 0x42};

    length = rtsp_proto_base64(three, sizeof(three), output, sizeof(output));
    CHECK(length == 4U && strcmp(output, "AAEC") == 0,
          "expected AAEC, got %s", output);

    length = rtsp_proto_base64(one, sizeof(one), output, sizeof(output));
    CHECK(length == 4U && strcmp(output, "QQ==") == 0,
          "expected QQ==, got %s", output);

    length = rtsp_proto_base64(two, sizeof(two), output, sizeof(output));
    CHECK(length == 4U && strcmp(output, "QUI=") == 0,
          "expected QUI=, got %s", output);

    length = rtsp_proto_base64(two, sizeof(two), output, 3U);
    CHECK(length == 0U, "truncated output must be rejected, got %zu", length);
}

static void test_request_parsing(void)
{
    struct rtsp_request request;
    const char request_text[] =
        "DESCRIBE rtsp://172.32.0.93:8554/live/0 RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "Accept: application/sdp\r\n"
        "\r\n";
    const char setup_text[] =
        "SETUP rtsp://172.32.0.93:8554/live/0/trackID=0 RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        "\r\n";
    const char teardown_text[] =
        "TEARDOWN rtsp://172.32.0.93:8554/live/0 RTSP/1.0\r\n"
        "CSeq: 7\r\n"
        "Session: 12345678;timeout=60\r\n"
        "\r\n";
    const char hex_session_text[] =
        "PLAY rtsp://172.32.0.93:8554/live/0 RTSP/1.0\r\n"
        "CSeq: 4\r\n"
        "Session: 5F0A1B2C\r\n"
        "\r\n";
    int consumed;

    consumed = rtsp_proto_parse(request_text, strlen(request_text), &request);
    CHECK(consumed > 0, "DESCRIBE must parse: %d", consumed);
    CHECK(strcmp(request.method, "DESCRIBE") == 0, "method %s",
          request.method);
    CHECK(strcmp(request.path, "rtsp://172.32.0.93:8554/live/0") == 0,
          "path %s", request.path);
    CHECK(request.has_cseq != 0 && request.cseq == 2L, "cseq %ld",
          request.cseq);
    CHECK(request.has_transport == 0, "DESCRIBE has no transport: %d", 1);
    CHECK((size_t)consumed == strlen(request_text),
          "consumed %d of %zu", consumed, strlen(request_text));

    consumed = rtsp_proto_parse(setup_text, strlen(setup_text), &request);
    CHECK(consumed > 0, "SETUP must parse: %d", consumed);
    CHECK(strcmp(request.method, "SETUP") == 0, "method %s", request.method);
    CHECK(request.has_transport != 0, "missing transport: %d", 1);
    CHECK(strstr(request.transport, "interleaved=0-1") != NULL,
          "transport %s", request.transport);

    consumed = rtsp_proto_parse(teardown_text, strlen(teardown_text),
                                &request);
    CHECK(request.has_session != 0 && request.session == 12345678L,
          "session %ld", request.session);
    CHECK(strcmp(request.session_token, "12345678") == 0,
          "session token '%s'", request.session_token);

    /*
     * The server hands out hexadecimal session ids. The client echoes them
     * verbatim, so the token must survive unchanged: parsing it as a number
     * would truncate "5F0A1B2C" to 5 and produce a bogus 454.
     */
    consumed = rtsp_proto_parse(hex_session_text, strlen(hex_session_text),
                                &request);
    CHECK(consumed > 0, "PLAY with a hex session must parse: %d", consumed);
    CHECK(strcmp(request.session_token, "5F0A1B2C") == 0,
          "hex session token '%s'", request.session_token);
    CHECK(request.session != 0x5F0A1B2CL,
          "numeric parse of a hex session is expected to be wrong: %ld",
          request.session);

    /* Incomplete requests must ask for more data. */
    CHECK(rtsp_proto_parse("OPTIONS rtsp://x RTSP/1.0\r\nCSeq: 1\r\n",
                           strlen("OPTIONS rtsp://x RTSP/1.0\r\nCSeq: 1\r\n"),
                           &request) == 0,
          "partial header must return 0: %d", 1);

    /* Malformed request line. */
    CHECK(rtsp_proto_parse("MALFORMED\r\n\r\n", strlen("MALFORMED\r\n\r\n"),
                           &request) == -1,
          "request without a URI must be rejected: %d", 1);
}

static void test_sdp_generation(void)
{
    char buffer[1024];
    int length;

    length = rtsp_proto_sdp(buffer, sizeof(buffer), "/live/0/trackID=0",
                            g_sps, sizeof(g_sps), g_pps, sizeof(g_pps));
    CHECK(length > 0, "sdp generation failed: %d", length);
    CHECK(strstr(buffer, "m=video 0 RTP/AVP 96") != NULL, "missing media line");
    CHECK(strstr(buffer, "a=rtpmap:96 H264/90000") != NULL,
          "missing rtpmap");
    CHECK(strstr(buffer, "packetization-mode=1") != NULL,
          "missing packetization mode");
    CHECK(strstr(buffer, "sprop-parameter-sets=") != NULL,
          "missing parameter sets");
    CHECK(strstr(buffer, "profile-level-id=42801E") != NULL,
          "wrong profile level id in %s", buffer);
    CHECK(strstr(buffer, "a=control:/live/0/trackID=0") != NULL,
          "missing control line");

    length = rtsp_proto_sdp(buffer, sizeof(buffer), "/live/0", NULL, 0U, NULL,
                            0U);
    CHECK(length > 0, "sdp without parameter sets failed: %d", length);
    CHECK(strstr(buffer, "sprop-parameter-sets=") == NULL,
          "parameter sets must be omitted when unknown: %d", 1);

    /* A too small buffer must be refused instead of truncated silently. */
    CHECK(rtsp_proto_sdp(buffer, 8U, "/live/0", NULL, 0U, NULL, 0U) == -1,
          "overflowing sdp must fail: %d", 1);
}

static void test_response_formatting(void)
{
    char buffer[1024];
    int length;
    size_t body_length;
    char *body;

    length = rtsp_proto_response(buffer, sizeof(buffer), 200, "OK", 3L,
                                 "91827364", NULL, NULL);
    CHECK(length > 0, "response failed: %d", length);
    CHECK(strncmp(buffer, "RTSP/1.0 200 OK\r\n", 17) == 0, "status line %s",
          buffer);
    CHECK(strstr(buffer, "CSeq: 3\r\n") != NULL, "missing CSeq: %s", buffer);
    CHECK(strstr(buffer, "Session: 91827364\r\n") != NULL,
          "missing Session: %s", buffer);
    CHECK(strstr(buffer, "Content-Length: 0\r\n\r\n") != NULL,
          "missing content length terminator: %s", buffer);

    length = rtsp_proto_response(buffer, sizeof(buffer), 461,
                                 "Unsupported Transport", 3L, NULL, NULL,
                                 NULL);
    CHECK(strstr(buffer, "RTSP/1.0 461 Unsupported Transport\r\n") != NULL,
          "wrong status line %s", buffer);
    CHECK(strstr(buffer, "Session:") == NULL,
          "session must be omitted: %s", buffer);

    length = rtsp_proto_response(buffer, sizeof(buffer), 200, "OK", 2L,
                                 "4711",
                                 "Content-Type: application/sdp\r\n",
                                 "v=0\r\n");
    body = strstr(buffer, "\r\n\r\n");
    CHECK(body != NULL, "no header/body separator: %s", buffer);
    CHECK(strncmp(body + 4, "v=0\r\n", 5) == 0, "body not appended: %s", body);
    body_length = strlen("v=0\r\n");
    CHECK(strstr(buffer, "Content-Length: 5\r\n") != NULL,
          "wrong content length for %zu body bytes", body_length);
    CHECK((size_t)length == strlen(buffer), "length %d vs %zu", length,
          strlen(buffer));
}

static void test_non_annex_b_input(void)
{
    const uint8_t garbage[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33,
                                 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
                                 0xBB, 0xCC};
    struct rtp_h264 rtp;
    struct capture capture;

    rtp_h264_init(&rtp, 30U, 0U);
    memset(&capture, 0, sizeof(capture));

    /* Input without start codes must report zero, not a silent success. */
    CHECK(rtp_h264_packetize(&rtp, garbage, sizeof(garbage), capture_emit,
                             &capture) == 0,
          "garbage input must report 0 packets: %d", 1);
    CHECK(capture.count == 0U, "nothing must be emitted, got %zu",
          capture.count);
    CHECK(rtp.timestamp == 0U,
          "timestamp must not advance without packets, got %u", rtp.timestamp);
}

int main(void)
{
    test_single_nal_units();
    test_fu_a_fragmentation();
    test_parameter_set_injection();
    test_non_annex_b_input();
    test_base64();
    test_request_parsing();
    test_sdp_generation();
    test_response_formatting();

    printf("rtp/rtsp: %d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
